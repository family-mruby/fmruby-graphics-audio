#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <map>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "lgfx/Fonts/misaki/lgfx_misaki_fonts.hpp"

extern "C" {
#include "graphics_handler.h"
#include "fmrb_link_protocol.h"
#include "fmrb_gfx.h"
#include "fmrb_bmp332.h"
#include "esp_log.h"
#include "display_interface.h"
#include "../mempool/fmrb_mempool.h"
#include "../mempool/fmrb_sprite_pool.h"
#include "sprite_manager.h"
#include "gfx_vm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
#include <sys/stat.h>       // mkdir, for EXPORT_FRAME
#include <stdlib.h>
#include "socket_server.h"  // For socket_server_send_ack
#else
#include "comm_interface.h"
#include "esp_heap_caps.h"
#endif
}

static const char *TAG = "graphics_handler";

// Present counter for stats
static uint32_t s_present_count = 0;

// Boot screen hand-off. Set when the core presents its first frame; until then
// the render loop leaves the framebuffer alone so graphics_task's boot text
// stays on screen. See graphics_handler_render_frame_internal.
static bool s_first_present_seen = false;
static bool s_boot_screen_cleared = false;

extern "C" uint32_t graphics_handler_get_and_reset_present_count(void) {
    uint32_t count = s_present_count;
    s_present_count = 0;
    return count;
}

// Mutex to protect canvas state (render_buffer, push_x/y, is_visible, etc.)
// between graphics_task (render) and message_handler_task (command processing)
static SemaphoreHandle_t g_canvas_mutex = nullptr;

// Get g_lgfx from display interface (defined in display_sdl2.cpp or display_cvbs.cpp)
static LovyanGFX* g_lgfx = nullptr;

// Next canvas ID to allocate
static uint16_t g_next_canvas_id = 1;

// Canvas state structure
typedef struct {
    uint16_t canvas_id;
    LGFX_Sprite* draw_buffer;      // Drawing buffer (front buffer for user drawing)
    LGFX_Sprite* render_buffer;    // Rendering buffer (back buffer for composition)
    void* draw_buffer_mem;         // External memory for draw_buffer
    void* render_buffer_mem;       // External memory for render_buffer
    int16_t z_order;               // Z-order (0=bottom, higher=front, SystemApp=0 fixed)
    int16_t push_x, push_y;        // Position to push to screen
    bool is_visible;               // Visibility flag
    uint16_t width, height;        // Canvas allocated dimensions (always max screen size)
    uint16_t active_width, active_height;  // Active drawing area (can be resized)
    bool dirty;                    // Redraw flag
    bool use_transparent;          // Use transparent color key during composition
    uint8_t transparent_color;     // RGB332 color treated as transparent

    // Sub-rect compositing regions. When region_count > 0 the compositor copies
    // only these regions (each with its own transparent/opaque mode) instead of
    // pushing the whole active area. region_count = 0 restores the default
    // full-area pushSprite path. Updated by SET_COMPOSITE_REGIONS RPC.
    uint8_t region_count;
    fmrb_link_graphics_composite_region_t regions[FMRB_LINK_MAX_COMPOSITE_REGIONS];

    // Sprite compositing clip. Sprites land on the render buffer after the
    // drawn content, so without a clip they cover the window frame the app
    // drew into the same canvas. clip_w = 0 means no clip (canvas bounds
    // only, the default). Held here rather than on the sprite itself because
    // LGFX_Sprite::setBuffer() resets the clip rect on every resize.
    // Updated by SET_SPRITE_CLIP RPC.
    uint16_t clip_x, clip_y;
    uint16_t clip_w, clip_h;
} canvas_state_t;

// Maximum number of canvases
#define MAX_CANVAS_COUNT 16

// Canvas management
static canvas_state_t g_canvases[MAX_CANVAS_COUNT];
static size_t g_canvas_count = 0;

// Cursor management
static LGFX_Sprite* g_cursor_sprite = nullptr;
static LGFX_Sprite* g_cursor_save = nullptr;   // Saved pixels under cursor
// Default false: cursor stays hidden until Core sends CURSOR_SET_VISIBLE(true)
// (system_desktop triggers it after the boot animation finishes).
static bool g_cursor_visible = false;
static bool g_cursor_drawn = false;             // Whether cursor is currently baked into screen_buffer
static int g_cursor_x = 240;  // Default: screen center
static int g_cursor_y = 135;
static int g_cursor_save_x = 0;  // Position where pixels were saved
static int g_cursor_save_y = 0;
static const uint32_t CURSOR_TRANSPARENT_COLOR = 0xFF00FF;  // Magenta

// 16x16 arrow cursor pattern (0=transparent, 1=white outline, 2=black body)
// Classic Windows-style pointer arrow with clean lines
static const uint8_t cursor_pattern[16][16] = {
    {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {1, 2, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
};

// Screen double buffer for compositing all canvases
static uint16_t g_current_target = FMRB_CANVAS_SCREEN;  // 0=screen, other=canvas
static sprite_image_id_t g_sprite_image_target = 0;      // 0=none, other=sprite image ID
static volatile bool g_graphics_initialized = false;  // Flag to prevent multiple initializations (volatile for cross-task access)

// Image store for CREATE_IMAGE_FROM_FILE / DRAW_IMAGE / DELETE_IMAGE
#define MAX_IMAGE_STORE 8

typedef struct {
    uint16_t image_id;
    LGFX_Sprite* sprite;    // Decoded sprite (NULL if raw PNG mode)
    uint8_t *png_data;      // Raw PNG data (for direct drawPng)
    uint32_t png_data_len;  // Raw PNG data length
    uint16_t width;
    uint16_t height;
    bool in_use;
} image_store_entry_t;

static image_store_entry_t g_image_store[MAX_IMAGE_STORE];
static uint16_t g_next_image_id = 1;

// Mask store for CREATE_MASK / DELETE_MASK / DRAW_IMAGE_MASKED.
// Each entry holds a 1bpp bitmap allocated in PSRAM (heap on linux).
#define MAX_MASK_STORE 16

typedef struct {
    bool in_use;
    uint16_t mask_id;
    uint16_t canvas_id;  // 0 = unbound; freed on DELETE_CANVAS otherwise
    uint16_t width, height;
    uint8_t *data;       // ceil(width/8) * height bytes, MSB-first per byte
} mask_store_entry_t;

static mask_store_entry_t g_mask_store[MAX_MASK_STORE];
static uint16_t g_next_mask_id = 1;

static mask_store_entry_t* mask_store_find(uint16_t mask_id) {
    if (mask_id == 0) return nullptr;
    for (int i = 0; i < MAX_MASK_STORE; i++) {
        if (g_mask_store[i].in_use && g_mask_store[i].mask_id == mask_id) {
            return &g_mask_store[i];
        }
    }
    return nullptr;
}

static void mask_store_free_entry(mask_store_entry_t *entry) {
    if (!entry) return;
    if (entry->data) {
        free(entry->data);
        entry->data = nullptr;
    }
    entry->in_use = false;
    entry->mask_id = 0;
    entry->canvas_id = 0;
    entry->width = entry->height = 0;
}

// Free all masks bound to the given canvas. Called when the canvas is
// deleted so the mask pool doesn't leak across app sessions.
static int mask_store_free_for_canvas(uint16_t canvas_id) {
    int freed = 0;
    for (int i = 0; i < MAX_MASK_STORE; i++) {
        if (g_mask_store[i].in_use && g_mask_store[i].canvas_id == canvas_id) {
            mask_store_free_entry(&g_mask_store[i]);
            freed++;
        }
    }
    return freed;
}

// Transparent color key for image sprites (RGB332)
// Used as background fill before drawPng; transparent pixels remain this color.
// pushSprite skips pixels matching this color.
// 0xFE = R:7 G:7 B:2 -- near-white, unlikely to appear in actual image content.
#define IMAGE_TRANSPARENT_COLOR 0xFE

// Canvas helper functions
static canvas_state_t* canvas_state_find(uint16_t canvas_id) {
    for (size_t i = 0; i < g_canvas_count; i++) {
        if (g_canvases[i].canvas_id == canvas_id) {
            return &g_canvases[i];
        }
    }
    return nullptr;
}

static canvas_state_t* canvas_state_alloc(uint16_t canvas_id, uint16_t req_width, uint16_t req_height,
                                          int16_t z_order, bool use_transparent, uint8_t transparent_color) {
    if (g_canvas_count >= MAX_CANVAS_COUNT) {
        ESP_LOGE(TAG, "Maximum canvas count reached (%d)", MAX_CANVAS_COUNT);
        return nullptr;
    }

    canvas_state_t* canvas = &g_canvases[g_canvas_count];
    // Note: g_canvas_count is incremented AFTER all initialization is complete
    // to prevent race condition with graphics_task render loop
    canvas->canvas_id = canvas_id;

    // Always allocate at display screen size to avoid reallocation on resize
    canvas->width = g_lgfx->width();
    canvas->height = g_lgfx->height();

    // Set initial active size to requested size
    canvas->active_width = req_width;
    canvas->active_height = req_height;

    // Assign final z_order up front so render task never observes a stale
    // placeholder value during the publish window.
    canvas->z_order = z_order;
    canvas->push_x = 0;
    canvas->push_y = 0;
    canvas->is_visible = false;  // Initially invisible until first present()
    canvas->dirty = false;
    canvas->use_transparent = use_transparent;
    canvas->transparent_color = transparent_color;
    canvas->region_count = 0;
    canvas->clip_x = 0;
    canvas->clip_y = 0;
    canvas->clip_w = 0;   // 0 = no sprite clip (canvas bounds)
    canvas->clip_h = 0;

    // Allocate external memory for draw buffer from mempool
    canvas->draw_buffer_mem = fmrb_mempool_canvas_alloc_buffer();
    if (!canvas->draw_buffer_mem) {
        ESP_LOGE(TAG, "Failed to allocate draw buffer memory for canvas %u", canvas_id);
        return nullptr;
    }

    // Allocate external memory for render buffer from mempool
    canvas->render_buffer_mem = fmrb_mempool_canvas_alloc_buffer();
    if (!canvas->render_buffer_mem) {
        ESP_LOGE(TAG, "Failed to allocate render buffer memory for canvas %u", canvas_id);
        fmrb_mempool_canvas_free_buffer(canvas->draw_buffer_mem);
        canvas->draw_buffer_mem = nullptr;
        return nullptr;
    }

    // Create draw buffer sprite and set external buffer
    canvas->draw_buffer = new LGFX_Sprite(g_lgfx);
    canvas->draw_buffer->setColorDepth(8);  // RGB332
    canvas->draw_buffer->setBuffer(canvas->draw_buffer_mem, req_width, req_height, 8);

    // Create render buffer sprite and set external buffer
    canvas->render_buffer = new LGFX_Sprite(g_lgfx);
    canvas->render_buffer->setColorDepth(8);  // RGB332
    canvas->render_buffer->setBuffer(canvas->render_buffer_mem, req_width, req_height, 8);

    if (use_transparent) {
        // Pre-fill both buffers with the transparent color so uninitialized
        // pixels composite as transparent instead of leaking buffer contents.
        canvas->draw_buffer->fillScreen(transparent_color);
        canvas->render_buffer->fillScreen(transparent_color);
        ESP_LOGI(TAG, "Canvas ID=%u: transparency enabled (color=0x%02X)",
                 canvas_id, transparent_color);
    }

    // Publish the new canvas atomically with respect to the render task.
    // Without the mutex, render could observe g_canvas_count++ before the
    // structure fields are flushed, or sort/composite while we are still
    // writing.
    xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
    g_canvas_count++;
    xSemaphoreGive(g_canvas_mutex);

    ESP_LOGI(TAG, "Canvas allocated: ID=%u, allocated_size=%dx%d, active_size=%dx%d, z_order=%d",
              canvas_id, canvas->width, canvas->height,
              canvas->active_width, canvas->active_height, canvas->z_order);
    return canvas;
}

static void canvas_state_free(canvas_state_t* canvas) {
    if (!canvas) return;

    ESP_LOGI(TAG, "Freeing canvas ID=%u", canvas->canvas_id);

    // Hold the mutex across buffer destruction and array compaction.
    // The render task dereferences render_buffer/draw_buffer and walks the
    // canvas array under the same mutex, so freeing or shifting without it
    // can be observed mid-render.
    xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);

    if (canvas->draw_buffer) {
        delete canvas->draw_buffer;
        canvas->draw_buffer = nullptr;
    }
    if (canvas->render_buffer) {
        delete canvas->render_buffer;
        canvas->render_buffer = nullptr;
    }

    // Free external memory buffers back to mempool
    if (canvas->draw_buffer_mem) {
        fmrb_mempool_canvas_free_buffer(canvas->draw_buffer_mem);
        canvas->draw_buffer_mem = nullptr;
    }
    if (canvas->render_buffer_mem) {
        fmrb_mempool_canvas_free_buffer(canvas->render_buffer_mem);
        canvas->render_buffer_mem = nullptr;
    }

    // Remove from array by shifting remaining elements
    size_t index = canvas - g_canvases;
    if (index < g_canvas_count - 1) {
        memmove(&g_canvases[index], &g_canvases[index + 1],
                (g_canvas_count - index - 1) * sizeof(canvas_state_t));
    }
    g_canvas_count--;

    xSemaphoreGive(g_canvas_mutex);
}

// Compare function for qsort (sort by z_order ascending)
static int canvas_compare_zorder(const void* a, const void* b) {
    const canvas_state_t* ca = (const canvas_state_t*)a;
    const canvas_state_t* cb = (const canvas_state_t*)b;
    return ca->z_order - cb->z_order;
}

static void canvas_sort_by_zorder() {
    if (g_canvas_count > 1) {
        qsort(g_canvases, g_canvas_count, sizeof(canvas_state_t), canvas_compare_zorder);
    }
}

// Composite a single sub-rect of canvas->render_buffer onto dst (8bpp RGB332).
// LovyanGFX's LGFXBase::pushImage overwrites pixelcopy_t::src_bitwidth at entry,
// which breaks sub-rect copies whose source stride differs from the region
// width. To stay independent of that quirk we do the row-by-row copy manually.
// dst stride is taken from dst->width() (= bitwidth for 8bpp).
static void composite_region(LGFX_Sprite* dst, const canvas_state_t* canvas,
                             const fmrb_link_graphics_composite_region_t* r) {
    const uint8_t* src_buf = (const uint8_t*)canvas->render_buffer->getBuffer();
    uint8_t* dst_buf = (uint8_t*)dst->getBuffer();
    if (!src_buf || !dst_buf) return;

    const int32_t src_stride = canvas->render_buffer->width();
    const int32_t dst_stride = dst->width();
    const int32_t dst_w = dst->width();
    const int32_t dst_h = dst->height();
    const int32_t src_w = canvas->render_buffer->width();
    const int32_t src_h = canvas->render_buffer->height();

    int32_t sx = r->src_x;
    int32_t sy = r->src_y;
    int32_t dx = canvas->push_x + r->dst_x;
    int32_t dy = canvas->push_y + r->dst_y;
    int32_t w  = r->w;
    int32_t h  = r->h;

    // Clip against source bounds
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src_w) w = src_w - sx;
    if (sy + h > src_h) h = src_h - sy;
    // Clip against destination bounds
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx + w > dst_w) w = dst_w - dx;
    if (dy + h > dst_h) h = dst_h - dy;
    if (w <= 0 || h <= 0) return;

    if (r->use_transparent) {
        const uint8_t key = canvas->transparent_color;
        for (int32_t y = 0; y < h; y++) {
            const uint8_t* sp = src_buf + (sy + y) * src_stride + sx;
            uint8_t* dp = dst_buf + (dy + y) * dst_stride + dx;
            for (int32_t x = 0; x < w; x++) {
                uint8_t px = sp[x];
                if (px != key) dp[x] = px;
            }
        }
    } else {
        for (int32_t y = 0; y < h; y++) {
            const uint8_t* sp = src_buf + (sy + y) * src_stride + sx;
            uint8_t* dp = dst_buf + (dy + y) * dst_stride + dx;
            memcpy(dp, sp, (size_t)w);
        }
    }
}

// Render all canvases to screen in Z-order
// Composite every visible canvas (and the cursor) into the background
// canvas's render buffer and hand that buffer back. Split out of
// render_frame so EXPORT_FRAME can take the same picture without
// pushing it to the display from another task. Call with the canvas
// mutex held. NULL means there is nothing to show yet.
//
// with_cursor false leaves the pointer out, for a picture that is being
// saved rather than shown -- the device's own export takes the framebuffer
// before the cursor is baked in, and this keeps the two the same. It also
// keeps the export from overwriting the saved background the render path
// restores the cursor from.
static LGFX_Sprite* compose_screen_buffer(bool with_cursor = true) {
    if (g_canvas_count == 0) {
        return NULL;  // No canvases to render
    }

    // Hold the boot screen until the core actually has a frame to show.
    // Canvases are created several seconds before the desktop draws anything,
    // and compositing an untouched canvas paints the display black for that
    // whole stretch. Waiting for the first present() keeps graphics_task's boot
    // text up until the desktop takes over. Wipe once at that hand-off, because
    // the canvas is smaller than the display and would otherwise leave boot
    // text in the margins.
    if (!s_first_present_seen) {
        return NULL;
    }
    if (!s_boot_screen_cleared) {
        g_lgfx->fillScreen(0x00);
        s_boot_screen_cleared = true;
    }

    // Sort canvases by Z-order (low to high)
    canvas_sort_by_zorder();

    // Find background canvas: lowest z_order canvas with a valid render buffer.
    // Prefer visible canvas, but fall back to any canvas with a buffer so that
    // the display shows content even before the first present() is received.
    canvas_state_t* bg_canvas = NULL;
    for (size_t i = 0; i < g_canvas_count; i++) {
        if (g_canvases[i].render_buffer) {
            bg_canvas = &g_canvases[i];
            break;
        }
    }
    if (!bg_canvas) {
        return NULL;  // No canvas with buffer ready yet
    }
    LGFX_Sprite* screen_buffer = bg_canvas->render_buffer;
    const int screen_w = screen_buffer->width();
    const int screen_h = screen_buffer->height();

    // If any visible canvas on top fully covers the screen with opaque content,
    // the bg_canvas pixels will be entirely overwritten anyway. Skip the
    // ~screen_w*screen_h memcpy in that case (e.g. fullscreen apps).
    bool bg_fully_covered = false;
    for (size_t i = 0; i < g_canvas_count; i++) {
        canvas_state_t* canvas = &g_canvases[i];
        if (canvas == bg_canvas) continue;
        if (!canvas->is_visible || !canvas->render_buffer) continue;
        if (canvas->region_count != 0) continue;
        if (canvas->use_transparent) continue;
        if (canvas->push_x != 0 || canvas->push_y != 0) continue;
        if (canvas->active_width < screen_w || canvas->active_height < screen_h) continue;
        bg_fully_covered = true;
        break;
    }

    // Restore background canvas from draw_buffer before compositing.
    // This clears any previously composited window pixels that would
    // otherwise remain as ghost images when windows are moved.
    if (bg_canvas->draw_buffer && !bg_fully_covered) {
        bg_canvas->draw_buffer->pushSprite(screen_buffer, 0, 0);
    }
    taskYIELD();  // Yield after heavy background copy to avoid WDT

    // Composite all other visible canvases onto the background canvas
    for (size_t i = 0; i < g_canvas_count; i++) {
        canvas_state_t* canvas = &g_canvases[i];
        if (canvas == bg_canvas) continue;
        if (canvas->is_visible && canvas->render_buffer) {
            ESP_LOGD(TAG, "Composite canvas ID=%u to screen buffer at (%d,%d), active_size=%dx%d, z_order=%d, regions=%u",
                    canvas->canvas_id, canvas->push_x, canvas->push_y,
                    canvas->active_width, canvas->active_height, canvas->z_order,
                    canvas->region_count);
            canvas->dirty = false;

            if (canvas->region_count > 0) {
                // Sub-rect compositing: only the listed regions are copied,
                // each with its own transparent/opaque mode. The rest of the
                // active area is not touched.
                for (uint8_t r = 0; r < canvas->region_count; r++) {
                    composite_region(screen_buffer, canvas, &canvas->regions[r]);
                }
            } else if (canvas->use_transparent) {
                // Full-area transparent compositing (fallback when no regions set)
                canvas->render_buffer->pushSprite(screen_buffer, canvas->push_x, canvas->push_y, canvas->transparent_color);
            } else {
                // Full-area opaque memcpy fast path
                canvas->render_buffer->pushSprite(screen_buffer, canvas->push_x, canvas->push_y);
            }
            taskYIELD();  // Yield after each canvas composite to avoid WDT
        }
    }

    // Save background under cursor, then draw cursor on screen_buffer
    if (with_cursor && g_cursor_visible && g_cursor_sprite && g_cursor_save) {
        // Save the 16x16 area that cursor will overwrite
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int sx = g_cursor_x + x;
                int sy = g_cursor_y + y;
                if (sx >= 0 && sx < screen_buffer->width() && sy >= 0 && sy < screen_buffer->height()) {
                    g_cursor_save->drawPixel(x, y, screen_buffer->readPixel(sx, sy));
                }
            }
        }
        g_cursor_save_x = g_cursor_x;
        g_cursor_save_y = g_cursor_y;

        // Draw cursor on top of everything
        g_cursor_sprite->pushSprite(screen_buffer, g_cursor_x, g_cursor_y, CURSOR_TRANSPARENT_COLOR);
        g_cursor_drawn = true;
        ESP_LOGD(TAG, "Cursor drawn at (%d, %d)", g_cursor_x, g_cursor_y);
    }

    return screen_buffer;
}

#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
// Write one composited frame out as a 24-bit BMP.
//
// Only the simulator does this: EXPORT_FRAME exists so a deck can be saved as
// pictures, and only here is there a host filesystem to leave them on. BMP
// rather than PNG because an uncompressed bottom-up bitmap is forty lines of
// code and needs no image library; the RGB332 -> RGB888 expansion is the same
// one tools/fmrb_screenshot.py uses, so an exported slide and a screenshot of
// the same slide are byte-identical pictures.
static bool export_frame_bmp(LGFX_Sprite *src, const char *path)
{
    const int w = src->width();
    const int h = src->height();
    if (w <= 0 || h <= 0) return false;

    const int row_bytes = w * 3;
    const int pad = (4 - (row_bytes % 4)) % 4;
    const uint32_t data_size = (uint32_t)(row_bytes + pad) * (uint32_t)h;
    const uint32_t file_size = 54 + data_size;

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "EXPORT_FRAME: cannot open %s", path);
        return false;
    }

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(file_size); hdr[3] = (uint8_t)(file_size >> 8);
    hdr[4] = (uint8_t)(file_size >> 16); hdr[5] = (uint8_t)(file_size >> 24);
    hdr[10] = 54;                       // pixel data offset
    hdr[14] = 40;                       // BITMAPINFOHEADER size
    hdr[18] = (uint8_t)(w); hdr[19] = (uint8_t)(w >> 8);
    hdr[20] = (uint8_t)(w >> 16); hdr[21] = (uint8_t)(w >> 24);
    hdr[22] = (uint8_t)(h); hdr[23] = (uint8_t)(h >> 8);
    hdr[24] = (uint8_t)(h >> 16); hdr[25] = (uint8_t)(h >> 24);
    hdr[26] = 1;                        // planes
    hdr[28] = 24;                       // bits per pixel
    hdr[34] = (uint8_t)(data_size); hdr[35] = (uint8_t)(data_size >> 8);
    hdr[36] = (uint8_t)(data_size >> 16); hdr[37] = (uint8_t)(data_size >> 24);
    fwrite(hdr, 1, sizeof(hdr), f);

    // BMP rows run bottom-up, and each pixel is stored blue first.
    //
    // The source row is read as rgb332 explicitly: readPixel hands back
    // RGB565 whatever the sprite's own depth is, and a round trip through
    // 565 does not reproduce the *255/7 expansion the screenshot tool uses,
    // so the two pictures of one frame would differ everywhere.
    uint8_t *row = (uint8_t *)malloc((size_t)row_bytes + pad);
    uint8_t *raw = (uint8_t *)malloc((size_t)w);
    if (!row || !raw) {
        if (row) free(row);
        if (raw) free(raw);
        fclose(f);
        return false;
    }
    memset(row, 0, (size_t)row_bytes + pad);
    for (int y = h - 1; y >= 0; y--) {
        src->readRect(0, y, w, 1, (lgfx::rgb332_t *)raw);
        for (int x = 0; x < w; x++) {
            uint8_t c = raw[x];
            row[x * 3 + 0] = (uint8_t)((c & 0x03) * 255 / 3);          // blue
            row[x * 3 + 1] = (uint8_t)(((c >> 2) & 0x07) * 255 / 7);   // green
            row[x * 3 + 2] = (uint8_t)(((c >> 5) & 0x07) * 255 / 7);   // red
        }
        fwrite(row, 1, (size_t)row_bytes + pad, f);
    }
    free(raw);
    free(row);
    fclose(f);
    return true;
}

// Make every directory on the way to the file, so an app only has to name
// where it wants the picture. The core's own mkdir cannot reach this
// filesystem: in the simulator the two sides do not share one.
static void export_frame_mkdirs(const char *path)
{
    char buf[256];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        mkdir(buf, 0777);
        buf[i] = '/';
    }
}
#endif

static void graphics_handler_render_frame_internal() {
    LGFX_Sprite* screen_buffer = compose_screen_buffer();
    if (!screen_buffer) {
        return;
    }

    // Push the complete screen buffer (with cursor) to g_lgfx in a single transfer
    screen_buffer->pushSprite(g_lgfx, 0, 0);
    taskYIELD();  // Yield after final screen push to avoid WDT
    ESP_LOGD(TAG, "Screen buffer pushed to display");
}

// Resolve drawing target from canvas_id, with sprite image override
// When g_sprite_image_target is set, all drawing goes to the sprite image.
static LovyanGFX* resolve_draw_target(uint16_t canvas_id) {
    if (g_sprite_image_target != 0) {
        LGFX_Sprite *spr = (LGFX_Sprite*)sprite_manager_get_image_sprite(g_sprite_image_target);
        if (spr) return spr;
        ESP_LOGE(TAG, "Sprite image %u not found", g_sprite_image_target);
        g_sprite_image_target = 0;
    }
    if (canvas_id == FMRB_CANVAS_SCREEN) {
        return g_lgfx;
    }
    canvas_state_t* canvas = canvas_state_find(canvas_id);
    if (canvas) {
        canvas->dirty = true;
        return canvas->draw_buffer;
    }
    ESP_LOGE(TAG, "Canvas %u not found", canvas_id);
    return nullptr;
}

extern "C" int graphics_handler_init(void) {
    // Prevent multiple initializations
    if (g_graphics_initialized) {
        ESP_LOGE(TAG, "Graphics handler already initialized, ignoring request");
        return 0;  // Return success to avoid breaking caller
    }

    // Note: Canvas memory pool is initialized in init_display_callback() with actual display dimensions

    // Get LGFX instance from display interface
    g_lgfx = (LovyanGFX*)DISPLAY_INTERFACE->get_lgfx();
    if (!g_lgfx) {
        ESP_LOGE(TAG, "LGFX instance not created");
        return -1;
    }

    g_lgfx->setAutoDisplay(false);

    // Create mutex for canvas state protection
    g_canvas_mutex = xSemaphoreCreateMutex();
    if (!g_canvas_mutex) {
        ESP_LOGE(TAG, "Failed to create canvas mutex");
        return -1;
    }

    // Initialize cursor sprite (16x16 arrow)
    g_cursor_sprite = new LGFX_Sprite(g_lgfx);
    g_cursor_sprite->setColorDepth(8);  // 8-bit color
    g_cursor_sprite->createSprite(16, 16);
    g_cursor_sprite->clear(CURSOR_TRANSPARENT_COLOR);

    // Initialize cursor background save sprite (16x16)
    g_cursor_save = new LGFX_Sprite(g_lgfx);
    g_cursor_save->setColorDepth(8);
    g_cursor_save->createSprite(16, 16);
    g_cursor_drawn = false;

    // Draw cursor pattern
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint32_t color;
            switch (cursor_pattern[y][x]) {
                case 1: color = 0xFFFFFF; break;  // White outline
                case 2: color = 0x000000; break;  // Black body
                default: color = CURSOR_TRANSPARENT_COLOR; break;  // Transparent
            }
            g_cursor_sprite->drawPixel(x, y, color);
        }
    }

    // Initialize sprite system
    if (fmrb_sprite_pool_init(0) != 0) {
        ESP_LOGW(TAG, "Sprite pool init failed (non-fatal)");
    }
    sprite_manager_init();
    gfx_vm_init();

    g_graphics_initialized = true;  // Mark as initialized
    ESP_LOGI(TAG, "Graphics handler initialized with screen buffer (%dx%d)",
              (int)g_lgfx->width(), (int)g_lgfx->height());
    ESP_LOGI(TAG, "Cursor sprite initialized (16x16) at position (%d, %d)", g_cursor_x, g_cursor_y);
    return 0;
}

extern "C" void graphics_handler_cleanup(void) {
    // Disable rendering first (checked by render_frame)
    g_graphics_initialized = false;
    g_lgfx = nullptr;  // Invalidate pointer to prevent stale access from render loop

    // Clean up sprite system
    sprite_manager_cleanup();
    fmrb_sprite_pool_deinit();
    g_sprite_image_target = 0;

    // Free any uploaded 1bpp masks.
    for (int i = 0; i < MAX_MASK_STORE; i++) {
        if (g_mask_store[i].in_use) {
            mask_store_free_entry(&g_mask_store[i]);
        }
    }

    // Delete all canvases
    while (g_canvas_count > 0) {
        canvas_state_free(&g_canvases[0]);
    }

    // Delete cursor sprites
    if (g_cursor_sprite) {
        delete g_cursor_sprite;
        g_cursor_sprite = nullptr;
    }
    if (g_cursor_save) {
        delete g_cursor_save;
        g_cursor_save = nullptr;
    }
    g_cursor_drawn = false;
    ESP_LOGI(TAG, "Cursor sprites deleted");

    // Delete mutex
    if (g_canvas_mutex) {
        vSemaphoreDelete(g_canvas_mutex);
        g_canvas_mutex = nullptr;
    }

    g_current_target = FMRB_CANVAS_SCREEN;
    g_next_canvas_id = 1;  // Reset canvas ID counter

    ESP_LOGI(TAG, "Graphics handler cleaned up");
}

// SDL_Renderer function removed - not needed in abstracted interface

extern "C" void graphics_handler_render_frame(void) {
    if (!g_lgfx || !g_graphics_initialized) {
        return;
    }
    if (xSemaphoreTake(g_canvas_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        graphics_handler_render_frame_internal();
        xSemaphoreGive(g_canvas_mutex);
    } else {
        ESP_LOGD(TAG, "render_frame: mutex timeout, skipping frame");
    }
}

// Use comm_interface send_ack function
// (No forward declaration needed - using COMM_INTERFACE macro)

extern "C" int graphics_handler_process_command(uint8_t msg_type, uint8_t cmd_type, uint8_t seq, const uint8_t *data, size_t size) {
    if (!g_lgfx) {
        return -1;
    }

    // msg_type: message type (for ACK response)
    // cmd_type: graphics command type (from msgpack sub_cmd field)
    // data: structure data only (no cmd_type prefix)

    switch (cmd_type) {
        case FMRB_LINK_GFX_CLEAR:
        case FMRB_LINK_GFX_FILL_SCREEN:
            if (size >= sizeof(fmrb_link_graphics_clear_t)) {
                const fmrb_link_graphics_clear_t *cmd = (const fmrb_link_graphics_clear_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillScreen(cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_PIXEL:
            if (size >= sizeof(fmrb_link_graphics_pixel_t)) {
                const fmrb_link_graphics_pixel_t *cmd = (const fmrb_link_graphics_pixel_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawPixel(cmd->x, cmd->y, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_LINE:
            if (size >= sizeof(fmrb_link_graphics_line_t)) {
                const fmrb_link_graphics_line_t *cmd = (const fmrb_link_graphics_line_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawLine(cmd->x1, cmd->y1, cmd->x2, cmd->y2, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_RECT:
            if (size >= sizeof(fmrb_link_graphics_rect_t)) {
                const fmrb_link_graphics_rect_t *cmd = (const fmrb_link_graphics_rect_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawRect(cmd->x, cmd->y, cmd->width, cmd->height, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_FILL_RECT:
            if (size >= sizeof(fmrb_link_graphics_rect_t)) {
                const fmrb_link_graphics_rect_t *cmd = (const fmrb_link_graphics_rect_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillRect(cmd->x, cmd->y, cmd->width, cmd->height, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_BLEND_RECT:
            if (size >= sizeof(fmrb_link_graphics_blend_rect_t)) {
                const fmrb_link_graphics_blend_rect_t *cmd = (const fmrb_link_graphics_blend_rect_t*)data;
                LovyanGFX* blend_target = resolve_draw_target(cmd->canvas_id);
                if (!blend_target) return -1;
                LGFX_Sprite* sprite = static_cast<LGFX_Sprite*>(blend_target);
                int16_t x_end = cmd->x + cmd->width;
                int16_t y_end = cmd->y + cmd->height;
                if (cmd->mode == FMRB_BLEND_MODE_XOR) {
                    for (int16_t py = cmd->y; py < y_end; py++) {
                        for (int16_t px = cmd->x; px < x_end; px++) {
                            uint8_t pixel = (uint8_t)sprite->readPixel(px, py);
                            sprite->drawPixel(px, py, pixel ^ cmd->color);
                        }
                    }
                } else {
                    // FMRB_BLEND_MODE_ADD (default)
                    uint8_t add_r = (cmd->color >> 5) & 0x07;
                    uint8_t add_g = (cmd->color >> 2) & 0x07;
                    uint8_t add_b = cmd->color & 0x03;
                    for (int16_t py = cmd->y; py < y_end; py++) {
                        for (int16_t px = cmd->x; px < x_end; px++) {
                            uint8_t pixel = (uint8_t)sprite->readPixel(px, py);
                            uint8_t r = (pixel >> 5) & 0x07;
                            uint8_t g = (pixel >> 2) & 0x07;
                            uint8_t b = pixel & 0x03;
                            r = (r + add_r > 7) ? 7 : r + add_r;
                            g = (g + add_g > 7) ? 7 : g + add_g;
                            b = (b + add_b > 3) ? 3 : b + add_b;
                            sprite->drawPixel(px, py, (r << 5) | (g << 2) | b);
                        }
                    }
                }
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_ROUND_RECT:
            if (size >= sizeof(fmrb_link_graphics_round_rect_t)) {
                const fmrb_link_graphics_round_rect_t *cmd = (const fmrb_link_graphics_round_rect_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawRoundRect(cmd->x, cmd->y, cmd->width, cmd->height, cmd->radius, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_FILL_ROUND_RECT:
            if (size >= sizeof(fmrb_link_graphics_round_rect_t)) {
                const fmrb_link_graphics_round_rect_t *cmd = (const fmrb_link_graphics_round_rect_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillRoundRect(cmd->x, cmd->y, cmd->width, cmd->height, cmd->radius, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_CIRCLE:
            if (size >= sizeof(fmrb_link_graphics_circle_t)) {
                const fmrb_link_graphics_circle_t *cmd = (const fmrb_link_graphics_circle_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawCircle(cmd->x, cmd->y, cmd->radius, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_FILL_CIRCLE:
            if (size >= sizeof(fmrb_link_graphics_circle_t)) {
                const fmrb_link_graphics_circle_t *cmd = (const fmrb_link_graphics_circle_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillCircle(cmd->x, cmd->y, cmd->radius, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_ELLIPSE:
            if (size >= sizeof(fmrb_link_graphics_ellipse_t)) {
                const fmrb_link_graphics_ellipse_t *cmd = (const fmrb_link_graphics_ellipse_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawEllipse(cmd->x, cmd->y, cmd->rx, cmd->ry, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_FILL_ELLIPSE:
            if (size >= sizeof(fmrb_link_graphics_ellipse_t)) {
                const fmrb_link_graphics_ellipse_t *cmd = (const fmrb_link_graphics_ellipse_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillEllipse(cmd->x, cmd->y, cmd->rx, cmd->ry, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_TRIANGLE:
            if (size >= sizeof(fmrb_link_graphics_triangle_t)) {
                const fmrb_link_graphics_triangle_t *cmd = (const fmrb_link_graphics_triangle_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawTriangle(cmd->x0, cmd->y0, cmd->x1, cmd->y1, cmd->x2, cmd->y2, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_FILL_TRIANGLE:
            if (size >= sizeof(fmrb_link_graphics_triangle_t)) {
                const fmrb_link_graphics_triangle_t *cmd = (const fmrb_link_graphics_triangle_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillTriangle(cmd->x0, cmd->y0, cmd->x1, cmd->y1, cmd->x2, cmd->y2, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_ARC:
            if (size >= sizeof(fmrb_link_graphics_arc_t)) {
                const fmrb_link_graphics_arc_t *cmd = (const fmrb_link_graphics_arc_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->drawArc(cmd->x, cmd->y, cmd->r1, cmd->r0, (float)cmd->angle0, (float)cmd->angle1, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_FILL_ARC:
            if (size >= sizeof(fmrb_link_graphics_arc_t)) {
                const fmrb_link_graphics_arc_t *cmd = (const fmrb_link_graphics_arc_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->fillArc(cmd->x, cmd->y, cmd->r1, cmd->r0, (float)cmd->angle0, (float)cmd->angle1, cmd->color);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_SET_TEXT_SIZE:
            if (size >= sizeof(fmrb_link_graphics_text_size_t)) {
                const fmrb_link_graphics_text_size_t *cmd = (const fmrb_link_graphics_text_size_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                target->setTextSize((float)cmd->size);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_SET_FONT:
            if (size >= sizeof(fmrb_link_graphics_set_font_t)) {
                const fmrb_link_graphics_set_font_t *cmd = (const fmrb_link_graphics_set_font_t*)data;
                LovyanGFX* target = resolve_draw_target(cmd->canvas_id);
                if (!target) return -1;
                switch (cmd->family) {
                    case FMRB_LINK_GFX_FONT_FAMILY_DEFAULT:
                        target->setFont(&lgfx::fonts::Font0);
                        break;
                    case FMRB_LINK_GFX_FONT_FAMILY_JA:
                        // size=8 -> misaki (matches the system 8px UI height),
                        // size=12 -> efontJA_12 (readability over compactness).
                        if (cmd->size == 8) {
                            target->setFont(&lgfx::fonts::misaki_8);
                        } else {
                            target->setFont(&lgfx::fonts::efontJA_12);
                        }
                        break;
                    default:
                        ESP_LOGW(TAG, "SET_FONT: unknown family=%u", cmd->family);
                        return -1;
                }
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_STRING:
            // Use structure from fmrb_link_protocol.h (no cmd_type in data)
            if (size < sizeof(fmrb_link_graphics_text_t)) {
                ESP_LOGE(TAG, "String command too small: size=%zu, expected>=%zu", size, sizeof(fmrb_link_graphics_text_t));
                break;
            }
            {
                const fmrb_link_graphics_text_t *text_cmd = (const fmrb_link_graphics_text_t*)data;

                size_t expected_size = sizeof(fmrb_link_graphics_text_t) + text_cmd->text_len;
                if (size < expected_size) {
                    ESP_LOGE(TAG, "String command size mismatch: expected=%zu, actual=%zu, text_len=%u",
                            expected_size, size, text_cmd->text_len);
                    break;
                }

                // Text data follows the structure
                const char *text_data = (const char*)(data + sizeof(fmrb_link_graphics_text_t));
                char text_buf[256];
                size_t len = text_cmd->text_len < 255 ? text_cmd->text_len : 255;
                memcpy(text_buf, text_data, len);
                text_buf[len] = '\0';

                ESP_LOGD(TAG, "DRAW_STRING: canvas_id=%u, x=%d, y=%d, color=0x%02x, bg_color=0x%02x, bg_transparent=%d, text='%s'",
                       text_cmd->canvas_id, (int)text_cmd->x, (int)text_cmd->y, text_cmd->color,
                       text_cmd->bg_color, text_cmd->bg_transparent, text_buf);

                // Get target from command (with sprite override)
                LovyanGFX* target = resolve_draw_target(text_cmd->canvas_id);
                if (!target) return -1;

                // Set text color with optional background
                if (text_cmd->bg_transparent) {
                    // Foreground only (transparent background)
                    target->setTextColor(text_cmd->color);
                } else {
                    // Foreground and background color
                    target->setTextColor(text_cmd->color, text_cmd->bg_color);
                }

                target->setCursor(text_cmd->x, text_cmd->y);

                if (text_cmd->hybrid_mode == 1) {
                    // Hybrid mode: render ASCII runs with Font0 and UTF-8
                    // multi-byte runs with misaki_8. LovyanGFX print() keeps the
                    // cursor between calls so runs stitch together naturally.
                    const lgfx::IFont* saved_font = target->getFont();
                    const uint8_t* p = (const uint8_t*)text_buf;
                    const uint8_t* end = p + len;
                    char run_buf[256];
                    while (p < end) {
                        const uint8_t* run_start = p;
                        bool is_ascii = (*p < 0x80);
                        if (is_ascii) {
                            while (p < end && *p < 0x80) p++;
                            target->setFont(&lgfx::fonts::Font0);
                        } else {
                            while (p < end && *p >= 0x80) p++;
                            target->setFont(&lgfx::fonts::misaki_8);
                        }
                        size_t run_len = p - run_start;
                        if (run_len >= sizeof(run_buf)) run_len = sizeof(run_buf) - 1;
                        memcpy(run_buf, run_start, run_len);
                        run_buf[run_len] = '\0';
                        target->print(run_buf);
                    }
                    // Restore the caller's font selection so subsequent draws
                    // (and the Ruby-side @current_font cache) stay consistent.
                    target->setFont(saved_font);
                } else {
                    target->print(text_buf);
                }
                ESP_LOGD(TAG, "DRAW_STRING: Text drawn");
                return 0;
            }

        // case FMRB_LINK_GFX_PRESENT:
        //     if (size >= sizeof(fmrb_link_graphics_present_t)) {
        //         const fmrb_link_graphics_present_t *cmd = (const fmrb_link_graphics_present_t*)data;
        //         ESP_LOGD(TAG, "PRESENT: canvas_id=%u", cmd->canvas_id);

        //         if (cmd->canvas_id == FMRB_CANVAS_SCREEN) {
        //             // Direct screen update - nothing to do, main loop handles rendering
        //             ESP_LOGD(TAG, "PRESENT: Screen - will be rendered in main loop");
        //         } else {
        //             // Push draw_buffer to render_buffer for the specified canvas
        //             canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
        //             if (!canvas) {
        //                 ESP_LOGE(TAG, "Canvas %u not found for present", cmd->canvas_id);
        //                 return -1;
        //             }

        //             // Copy draw_buffer to render_buffer (double buffering)
        //             ESP_LOGD(TAG, "PRESENT: Copying draw_buffer to render_buffer for canvas %u", cmd->canvas_id);
        //             canvas->draw_buffer->pushSprite(canvas->render_buffer, 0, 0);
        //             canvas->dirty = true;
        //         }

        //         // Note: Rendering and display() are handled by main loop at ~60fps
        //         ESP_LOGD(TAG, "PRESENT: Canvas updated, will be rendered in main loop");
        //         return 0;
        //     }
        //     break;

        // Canvas management commands
        case FMRB_LINK_GFX_CREATE_CANVAS:
            if (size >= sizeof(fmrb_link_graphics_create_canvas_t)) {
                const fmrb_link_graphics_create_canvas_t *cmd = (const fmrb_link_graphics_create_canvas_t*)data;

                // Allocate new canvas ID (ignore cmd->canvas_id from client)
                uint16_t canvas_id = g_next_canvas_id++;
                if (canvas_id == 0xFFFF) {  // FMRB_CANVAS_INVALID
                    canvas_id = g_next_canvas_id++;  // Skip invalid value
                }

                // Allocate canvas state (z_order and transparency settings applied
                // before the canvas becomes visible to the render task)
                canvas_state_t* canvas = canvas_state_alloc(canvas_id, cmd->width, cmd->height,
                                                            cmd->z_order,
                                                            cmd->use_transparent != 0,
                                                            cmd->transparent_color);
                if (!canvas) {
                    ESP_LOGE(TAG, "Failed to allocate canvas %u (%dx%d)",
                            canvas_id, (int)cmd->width, (int)cmd->height);
                    return -1;
                }

                ESP_LOGI(TAG, "Canvas created: ID=%u, %dx%d, z_order=%d", canvas_id, (int)cmd->width, (int)cmd->height, (int)cmd->z_order);

                // Send ACK with canvas_id
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
                socket_server_send_ack(msg_type, seq, (const uint8_t*)&canvas_id, sizeof(canvas_id));
#else
                COMM_INTERFACE->send_ack(msg_type, seq, (const uint8_t*)&canvas_id, sizeof(canvas_id));
#endif
                return 1;  // ACK already sent with canvas_id
            }
            break;

        case FMRB_LINK_GFX_DELETE_CANVAS:
            if (size >= sizeof(fmrb_link_graphics_delete_canvas_t)) {
                const fmrb_link_graphics_delete_canvas_t *cmd = (const fmrb_link_graphics_delete_canvas_t*)data;

                canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
                if (!canvas) {
                    ESP_LOGE(TAG, "Canvas %u not found", cmd->canvas_id);
                    return -1;
                }

                // If deleting current target, switch back to screen
                if (g_current_target == cmd->canvas_id) {
                    g_current_target = FMRB_CANVAS_SCREEN;
                }

                // Auto-cleanup all sprites belonging to this canvas
                sprite_manager_delete_all_for_canvas(cmd->canvas_id);
                g_sprite_image_target = 0;

                // Auto-cleanup all 1bpp masks belonging to this canvas
                int mask_freed = mask_store_free_for_canvas(cmd->canvas_id);
                if (mask_freed > 0) {
                    ESP_LOGI(TAG, "Auto-freed %d masks for canvas %u",
                             mask_freed, cmd->canvas_id);
                }

                // Auto-cleanup all GfxBlock programs belonging to this canvas
                gfx_vm_delete_progs_by_canvas(cmd->canvas_id);

                canvas_state_free(canvas);
                ESP_LOGI(TAG, "Canvas deleted: ID=%u", cmd->canvas_id);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_SET_WINDOW_ORDER:
            if (size >= sizeof(fmrb_link_graphics_set_window_order_t)) {
                const fmrb_link_graphics_set_window_order_t *cmd = (const fmrb_link_graphics_set_window_order_t*)data;

                // Hold the mutex across find + update so the render task's
                // qsort cannot swap canvas array entries between locating the
                // target canvas and writing its new z_order, which would
                // otherwise corrupt a different canvas's z_order permanently.
                xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
                canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
                if (!canvas) {
                    xSemaphoreGive(g_canvas_mutex);
                    ESP_LOGE(TAG, "Canvas %u not found for SET_WINDOW_ORDER", cmd->canvas_id);
                    return -1;
                }
                canvas->z_order = cmd->z_order;
                xSemaphoreGive(g_canvas_mutex);
                ESP_LOGI(TAG, "Canvas %u z_order updated to %d", cmd->canvas_id, cmd->z_order);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_SET_CANVAS_VISIBLE:
            if (size >= sizeof(fmrb_link_graphics_set_canvas_visible_t)) {
                const fmrb_link_graphics_set_canvas_visible_t *cmd = (const fmrb_link_graphics_set_canvas_visible_t*)data;

                canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
                if (!canvas) {
                    ESP_LOGE(TAG, "Canvas %u not found for SET_CANVAS_VISIBLE", cmd->canvas_id);
                    return -1;
                }

                xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
                canvas->is_visible = (cmd->visible != 0);
                xSemaphoreGive(g_canvas_mutex);
                ESP_LOGI(TAG, "Canvas %u visibility set to %d", cmd->canvas_id, cmd->visible);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_GET_PIXEL:
            if (size >= sizeof(fmrb_link_graphics_get_pixel_t)) {
                const fmrb_link_graphics_get_pixel_t *cmd = (const fmrb_link_graphics_get_pixel_t*)data;
                fmrb_link_graphics_pixel_value_t resp = { .color = 0, .status = 0xFF };

                // Use readPixelValue on the sprite to get the raw RGB332 byte.
                // readPixel goes through the base LovyanGFX color-conversion
                // path and would return RGB565 here, whose low byte does not
                // match the stored RGB332 value.
                LGFX_Sprite* sprite_target = nullptr;
                int32_t w = 0, h = 0;
                if (cmd->canvas_id == FMRB_CANVAS_SCREEN) {
                    // Screen readback is not supported for now — would need
                    // a different path because g_lgfx is not an LGFX_Sprite.
                    resp.status = 0xFF;
                } else {
                    canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
                    if (canvas && canvas->draw_buffer) {
                        sprite_target = canvas->draw_buffer;
                        w = canvas->active_width;
                        h = canvas->active_height;
                    }
                }

                if (!sprite_target && cmd->canvas_id != FMRB_CANVAS_SCREEN) {
                    ESP_LOGW(TAG, "GET_PIXEL: canvas %u not found", cmd->canvas_id);
                    resp.status = 0xFF;
                } else if (sprite_target) {
                    if (cmd->x < 0 || cmd->y < 0 || cmd->x >= w || cmd->y >= h) {
                        resp.status = 1;   // out of range
                    } else {
                        resp.color = (uint8_t)sprite_target->readPixelValue(cmd->x, cmd->y);
                        resp.status = 0;
                    }
                }

#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
                socket_server_send_ack(msg_type, seq, (const uint8_t*)&resp, sizeof(resp));
#else
                COMM_INTERFACE->send_ack(msg_type, seq, (const uint8_t*)&resp, sizeof(resp));
#endif
                return 1;  // ACK already sent
            }
            break;

        case FMRB_LINK_GFX_SET_COMPOSITE_REGIONS: {
            // Variable-length payload: 4-byte header + count * 14-byte region.
            const size_t header_size = offsetof(fmrb_link_graphics_set_composite_regions_t, regions);
            if (size < header_size) {
                ESP_LOGE(TAG, "SET_COMPOSITE_REGIONS: payload too small (%zu)", size);
                return -1;
            }
            const fmrb_link_graphics_set_composite_regions_t *cmd =
                (const fmrb_link_graphics_set_composite_regions_t*)data;

            uint8_t count = cmd->count;
            if (count > FMRB_LINK_MAX_COMPOSITE_REGIONS) {
                ESP_LOGW(TAG, "SET_COMPOSITE_REGIONS: count %u exceeds max %u, clipping",
                         count, (unsigned)FMRB_LINK_MAX_COMPOSITE_REGIONS);
                count = FMRB_LINK_MAX_COMPOSITE_REGIONS;
            }
            size_t expected = header_size + (size_t)count * sizeof(fmrb_link_graphics_composite_region_t);
            if (size < expected) {
                ESP_LOGE(TAG, "SET_COMPOSITE_REGIONS: payload %zu < expected %zu (count=%u)",
                         size, expected, count);
                return -1;
            }

            canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
            if (!canvas) {
                ESP_LOGE(TAG, "Canvas %u not found for SET_COMPOSITE_REGIONS", cmd->canvas_id);
                return -1;
            }

            xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
            if (count > 0) {
                memcpy(canvas->regions, cmd->regions,
                       (size_t)count * sizeof(fmrb_link_graphics_composite_region_t));
            }
            canvas->region_count = count;
            xSemaphoreGive(g_canvas_mutex);
            ESP_LOGI(TAG, "Canvas %u composite regions set: count=%u", cmd->canvas_id, count);
            return 0;
        }

        case FMRB_LINK_GFX_SET_SPRITE_CLIP: {
            if (size < sizeof(fmrb_link_graphics_set_sprite_clip_t)) {
                ESP_LOGE(TAG, "SET_SPRITE_CLIP: payload too small (%zu)", size);
                return -1;
            }
            const fmrb_link_graphics_set_sprite_clip_t *cmd =
                (const fmrb_link_graphics_set_sprite_clip_t*)data;

            canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
            if (!canvas) {
                ESP_LOGE(TAG, "Canvas %u not found for SET_SPRITE_CLIP", cmd->canvas_id);
                return -1;
            }

            xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
            if (cmd->w == 0 || cmd->h == 0 ||
                cmd->x >= canvas->active_width || cmd->y >= canvas->active_height) {
                canvas->clip_x = canvas->clip_y = 0;
                canvas->clip_w = canvas->clip_h = 0;
            } else {
                uint16_t max_w = canvas->active_width - cmd->x;
                uint16_t max_h = canvas->active_height - cmd->y;
                canvas->clip_x = cmd->x;
                canvas->clip_y = cmd->y;
                canvas->clip_w = (cmd->w < max_w) ? cmd->w : max_w;
                canvas->clip_h = (cmd->h < max_h) ? cmd->h : max_h;
            }
            xSemaphoreGive(g_canvas_mutex);
            ESP_LOGI(TAG, "Canvas %u sprite clip set: (%u,%u) %ux%u",
                     cmd->canvas_id, canvas->clip_x, canvas->clip_y,
                     canvas->clip_w, canvas->clip_h);
            return 0;
        }

        case FMRB_LINK_GFX_CREATE_MASK: {
            // BEGIN: reserve a zero-filled mask buffer of size width*height
            // bits. Data is streamed in subsequently via MASK_DATA chunks.
            fmrb_link_graphics_mask_created_t resp = { .mask_id = 0 };

            do {
                if (size < sizeof(fmrb_link_graphics_create_mask_t)) {
                    ESP_LOGE(TAG, "CREATE_MASK: payload too small (%zu)", size);
                    break;
                }
                const fmrb_link_graphics_create_mask_t *cmd =
                    (const fmrb_link_graphics_create_mask_t *)data;
                if (cmd->width == 0 || cmd->height == 0) {
                    ESP_LOGE(TAG, "CREATE_MASK: invalid dimensions %ux%u",
                             cmd->width, cmd->height);
                    break;
                }
                uint32_t row_bytes = (uint32_t)((cmd->width + 7) / 8);
                uint32_t mask_bytes = row_bytes * cmd->height;
                int slot = -1;
                for (int i = 0; i < MAX_MASK_STORE; i++) {
                    if (!g_mask_store[i].in_use) { slot = i; break; }
                }
                if (slot < 0) {
                    ESP_LOGE(TAG, "CREATE_MASK: mask store full");
                    break;
                }
                uint8_t *buf;
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
                buf = (uint8_t *)calloc(1, mask_bytes);
#else
                buf = (uint8_t *)heap_caps_calloc(1, mask_bytes, MALLOC_CAP_SPIRAM);
#endif
                if (!buf) {
                    ESP_LOGE(TAG, "CREATE_MASK: alloc failed (%u bytes)", (unsigned)mask_bytes);
                    break;
                }
                uint16_t new_id = g_next_mask_id++;
                if (new_id == 0) new_id = g_next_mask_id++;  // skip 0 (reserved as error)
                g_mask_store[slot].in_use = true;
                g_mask_store[slot].mask_id = new_id;
                g_mask_store[slot].canvas_id = cmd->canvas_id;
                g_mask_store[slot].width = cmd->width;
                g_mask_store[slot].height = cmd->height;
                g_mask_store[slot].data = buf;
                resp.mask_id = new_id;
                ESP_LOGD(TAG, "CREATE_MASK: id=%u canvas=%u reserved %ux%u (%u bytes)",
                         new_id, cmd->canvas_id,
                         cmd->width, cmd->height, (unsigned)mask_bytes);
            } while (0);

#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            socket_server_send_ack(msg_type, seq, (const uint8_t*)&resp, sizeof(resp));
#else
            COMM_INTERFACE->send_ack(msg_type, seq, (const uint8_t*)&resp, sizeof(resp));
#endif
            return 1;
        }

        case FMRB_LINK_GFX_MASK_DATA:
            if (size >= sizeof(fmrb_link_graphics_mask_data_t)) {
                const fmrb_link_graphics_mask_data_t *cmd =
                    (const fmrb_link_graphics_mask_data_t *)data;
                if (sizeof(*cmd) + cmd->chunk_len > size) {
                    ESP_LOGE(TAG, "MASK_DATA: chunk_len %u extends past payload (%zu)",
                             cmd->chunk_len, size);
                    return -1;
                }
                mask_store_entry_t *m = mask_store_find(cmd->mask_id);
                if (!m) {
                    ESP_LOGE(TAG, "MASK_DATA: mask %u not found", cmd->mask_id);
                    return -1;
                }
                uint32_t total = (uint32_t)((m->width + 7) / 8) * m->height;
                if ((uint64_t)cmd->offset + cmd->chunk_len > total) {
                    ESP_LOGE(TAG, "MASK_DATA: chunk %u..%u exceeds mask size %u",
                             (unsigned)cmd->offset,
                             (unsigned)(cmd->offset + cmd->chunk_len),
                             (unsigned)total);
                    return -1;
                }
                memcpy(m->data + cmd->offset, data + sizeof(*cmd), cmd->chunk_len);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DELETE_MASK:
            if (size >= sizeof(fmrb_link_graphics_delete_mask_t)) {
                const fmrb_link_graphics_delete_mask_t *cmd =
                    (const fmrb_link_graphics_delete_mask_t *)data;
                mask_store_entry_t *m = mask_store_find(cmd->mask_id);
                if (m) {
                    mask_store_free_entry(m);
                    ESP_LOGD(TAG, "DELETE_MASK: id=%u", cmd->mask_id);
                } else {
                    ESP_LOGW(TAG, "DELETE_MASK: id=%u not found", cmd->mask_id);
                }
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_IMAGE_MASKED:
            if (size >= sizeof(fmrb_link_graphics_draw_image_masked_t)) {
                const fmrb_link_graphics_draw_image_masked_t *cmd =
                    (const fmrb_link_graphics_draw_image_masked_t *)data;

                LGFX_Sprite *src = (LGFX_Sprite *)sprite_manager_get_image_sprite(cmd->image_id);
                if (!src) {
                    ESP_LOGE(TAG, "DRAW_IMAGE_MASKED: image %u not found", cmd->image_id);
                    return -1;
                }
                mask_store_entry_t *mask = mask_store_find(cmd->mask_id);
                if (!mask) {
                    ESP_LOGE(TAG, "DRAW_IMAGE_MASKED: mask %u not found", cmd->mask_id);
                    return -1;
                }
                LGFX_Sprite *dst = nullptr;
                if (cmd->canvas_id == FMRB_CANVAS_SCREEN) {
                    ESP_LOGE(TAG, "DRAW_IMAGE_MASKED: screen target not supported");
                    return -1;
                } else {
                    canvas_state_t *canvas = canvas_state_find(cmd->canvas_id);
                    if (!canvas || !canvas->draw_buffer) {
                        ESP_LOGE(TAG, "DRAW_IMAGE_MASKED: canvas %u not found", cmd->canvas_id);
                        return -1;
                    }
                    dst = canvas->draw_buffer;
                    canvas->dirty = true;
                }

                int mw = mask->width;
                int mh = mask->height;
                int sw = src->width();
                int sh = src->height();
                int row_bytes = (mw + 7) / 8;
                const uint8_t *mdata = mask->data;

                for (int yy = 0; yy < mh; yy++) {
                    // Skip rows entirely outside the source sprite.
                    if (yy >= sh) break;
                    const uint8_t *row = mdata + yy * row_bytes;
                    for (int xx = 0; xx < mw; xx++) {
                        if (xx >= sw) break;
                        uint8_t bit = (row[xx >> 3] >> (7 - (xx & 7))) & 1;
                        if (!bit) continue;
                        uint8_t pixel = (uint8_t)src->readPixelValue(xx, yy);
                        dst->drawPixel(cmd->x + xx, cmd->y + yy, pixel);
                    }
                }
                return 0;
            }
            break;

        case FMRB_LINK_GFX_DRAW_TILE:
            if (size >= sizeof(fmrb_link_graphics_draw_tile_t)) {
                const fmrb_link_graphics_draw_tile_t *cmd =
                    (const fmrb_link_graphics_draw_tile_t *)data;

                LGFX_Sprite *src = (LGFX_Sprite *)sprite_manager_get_image_sprite(cmd->image_id);
                if (!src) {
                    ESP_LOGE(TAG, "DRAW_TILE: image %u not found", cmd->image_id);
                    return -1;
                }
                LGFX_Sprite *dst = nullptr;
                if (cmd->canvas_id == FMRB_CANVAS_SCREEN) {
                    ESP_LOGE(TAG, "DRAW_TILE: screen target not supported");
                    return -1;
                } else {
                    canvas_state_t *canvas = canvas_state_find(cmd->canvas_id);
                    if (!canvas || !canvas->draw_buffer) {
                        ESP_LOGE(TAG, "DRAW_TILE: canvas %u not found", cmd->canvas_id);
                        return -1;
                    }
                    dst = canvas->draw_buffer;
                    canvas->dirty = true;
                }

                uint8_t transparent_color = 0;
                bool has_transparent =
                    sprite_manager_get_image_transparent(cmd->image_id,
                                                         &transparent_color) == 1;

                int sw = src->width();
                int sh = src->height();
                int src_x = cmd->src_x;
                int src_y = cmd->src_y;
                int w = cmd->w;
                int h = cmd->h;
                int dst_x = cmd->dst_x;
                int dst_y = cmd->dst_y;

                for (int yy = 0; yy < h; yy++) {
                    int sy = src_y + yy;
                    if (sy < 0 || sy >= sh) continue;
                    for (int xx = 0; xx < w; xx++) {
                        int sx = src_x + xx;
                        if (sx < 0 || sx >= sw) continue;
                        uint8_t pixel = (uint8_t)src->readPixelValue(sx, sy);
                        if (has_transparent && pixel == transparent_color) continue;
                        dst->drawPixel(dst_x + xx, dst_y + yy, pixel);
                    }
                }
                return 0;
            }
            break;

        case FMRB_LINK_GFX_UPDATE_WINDOW:
            if (size >= sizeof(fmrb_link_graphics_update_window_t)) {
                const fmrb_link_graphics_update_window_t *cmd = (const fmrb_link_graphics_update_window_t*)data;

                canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
                if (!canvas) {
                    ESP_LOGE(TAG, "Canvas %u not found for UPDATE_WINDOW", cmd->canvas_id);
                    return -1;
                }

                ESP_LOGI(TAG, "UPDATE_WINDOW: canvas_id=%u, pos=(%d,%d), active_size=%dx%d",
                          cmd->canvas_id, (int)cmd->x, (int)cmd->y, (int)cmd->width, (int)cmd->height);

                // Lock to prevent render_frame from reading inconsistent state
                xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);

                // Update position
                canvas->push_x = cmd->x;
                canvas->push_y = cmd->y;

                // Update active size by calling setBuffer with new dimensions
                // This reuses the same external memory buffer with new width/height
                canvas->active_width = (uint16_t)cmd->width;
                canvas->active_height = (uint16_t)cmd->height;

                // Reconfigure sprites with new dimensions (reusing same memory buffers)
                canvas->draw_buffer->setBuffer(canvas->draw_buffer_mem,
                                              canvas->active_width, canvas->active_height, 8);
                canvas->render_buffer->setBuffer(canvas->render_buffer_mem,
                                                canvas->active_width, canvas->active_height, 8);

                // Composite regions are sized for the old dimensions and may
                // now reach past the new buffer. Clear them so the next frame
                // falls back to full-area compositing; the consumer should
                // resend regions after resize if needed.
                canvas->region_count = 0;

                // Same for the sprite clip: it was sized for the old active
                // area. The app resends one for the new user area from its
                // resize handler.
                canvas->clip_x = canvas->clip_y = 0;
                canvas->clip_w = canvas->clip_h = 0;

                canvas->dirty = true;

                xSemaphoreGive(g_canvas_mutex);

                ESP_LOGI(TAG, "Canvas %u resized to %dx%d using setBuffer (allocated: %dx%d)",
                          cmd->canvas_id, canvas->active_width, canvas->active_height,
                          canvas->width, canvas->height);

                return 0;
            }
            break;

        case FMRB_LINK_GFX_SET_TARGET:
            if (size >= sizeof(fmrb_link_graphics_set_target_t)) {
                const fmrb_link_graphics_set_target_t *cmd = (const fmrb_link_graphics_set_target_t*)data;

                // Validate target
                if (cmd->target_id != FMRB_CANVAS_SCREEN) {
                    if (!canvas_state_find(cmd->target_id)) {
                        ESP_LOGE(TAG, "Canvas %u not found for set_target", cmd->target_id);
                        return -1;
                    }
                }

                g_current_target = cmd->target_id;
                ESP_LOGD(TAG, "Drawing target set: ID=%u %s", cmd->target_id,
                       cmd->target_id == FMRB_CANVAS_SCREEN ? "(screen)" : "(canvas)");
                return 0;
            }
            break;

        case FMRB_LINK_GFX_PUSH_CANVAS:
            if (size >= sizeof(fmrb_link_graphics_push_canvas_t)) {
                const fmrb_link_graphics_push_canvas_t *cmd = (const fmrb_link_graphics_push_canvas_t*)data;

                // Find source canvas
                canvas_state_t* src_canvas = canvas_state_find(cmd->canvas_id);
                if (!src_canvas) {
                    ESP_LOGE(TAG, "Canvas %u not found for push", cmd->canvas_id);
                    return -1;
                }

                // Determine destination (screen or canvas)
                LovyanGFX* dst;
                const char* dst_name;
                int push_x, push_y;

                if (cmd->dest_canvas_id == FMRB_CANVAS_RENDER) {
                    // Push to own render_buffer at (0,0), save position for later screen rendering
                    // Lock to prevent render_frame from reading while we update
                    xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
                    dst = src_canvas->render_buffer;
                    dst_name = "render_canvas";
                    src_canvas->push_x = cmd->x;
                    src_canvas->push_y = cmd->y;
                    src_canvas->is_visible = true;  // Make visible on first present()
                    s_first_present_seen = true;
                    s_present_count++;
                    push_x = 0;
                    push_y = 0;
                } else if(cmd->dest_canvas_id == 0) {
                    // Push directly to screen at specified position
                    dst = g_lgfx;
                    dst_name = "screen";
                    push_x = cmd->x;
                    push_y = cmd->y;
                } else {
                    ESP_LOGE(TAG, "Destination canvas %u is not supported yet...", cmd->dest_canvas_id);
                    // Store push position in canvas state (for render_frame)
                    // TODO: how to handle child canvas??
                    // src_canvas->push_x = cmd->x;
                    // src_canvas->push_y = cmd->y;
                    return -1;
                }


                // Push draw_buffer to destination
                LGFX_Sprite* src_sprite = src_canvas->draw_buffer;
                ESP_LOGD(TAG, "PUSH_CANVAS: src=%p (active=%dx%d), dst=%p (%s), push_at=(%d,%d), save_pos=(%d,%d)",
                       src_sprite, src_canvas->active_width, src_canvas->active_height, dst, dst_name,
                       push_x, push_y, (int)cmd->x, (int)cmd->y);

                // Since setBuffer configures sprite to active size, pushSprite transfers only active region
                if (cmd->use_transparency) {
                    src_sprite->pushSprite(dst, push_x, push_y, cmd->transparent_color);
                    ESP_LOGD(TAG, "Canvas pushed with transparency: ID=%u to %s at (%d,%d), transp=0x%02x",
                           cmd->canvas_id, dst_name, push_x, push_y, cmd->transparent_color);
                } else {
                    src_sprite->pushSprite(dst, push_x, push_y);
                    ESP_LOGD(TAG, "Canvas pushed: ID=%u to %s at (%d,%d)", cmd->canvas_id, dst_name, push_x, push_y);
                }

                // Composite sprites onto render buffer after draw_buffer copy.
                // The app-set clip (SET_SPRITE_CLIP) keeps them inside its
                // user area instead of over the frame it just drew. It is
                // applied here and dropped again because setBuffer() on resize
                // would silently reset a clip left on the sprite.
                if (cmd->dest_canvas_id == FMRB_CANVAS_RENDER && src_canvas->render_buffer) {
                    bool clipped = (src_canvas->clip_w > 0 && src_canvas->clip_h > 0);
                    if (clipped) {
                        src_canvas->render_buffer->setClipRect(
                            src_canvas->clip_x, src_canvas->clip_y,
                            src_canvas->clip_w, src_canvas->clip_h);
                    }
                    sprite_manager_composite(cmd->canvas_id, src_canvas->render_buffer);
                    if (clipped) src_canvas->render_buffer->clearClipRect();
                }

                // Release mutex if we locked for render_buffer path
                if (cmd->dest_canvas_id == FMRB_CANVAS_RENDER) {
                    xSemaphoreGive(g_canvas_mutex);
                }

                return 0;
            }
            break;

        case FMRB_LINK_GFX_EXPORT_FRAME: {
            if (size < sizeof(fmrb_link_graphics_export_frame_t)) break;
            const fmrb_link_graphics_export_frame_t *cmd =
                (const fmrb_link_graphics_export_frame_t*)data;
            const char *path_data = (const char *)(data + sizeof(*cmd));
            uint16_t path_len = cmd->path_len;
            if (sizeof(*cmd) + path_len > size) {
                ESP_LOGE(TAG, "EXPORT_FRAME: path extends beyond payload");
                return -1;
            }
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            {
                const char *p = path_data;
                int plen = (int)path_len;
                if (plen > 0 && p[0] == '/') { p++; plen--; }
                char full_path[256];
                snprintf(full_path, sizeof(full_path), "flash/%.*s", plen, p);
                export_frame_mkdirs(full_path);

                // Composite here rather than wait for the render task: the
                // command sits right behind the present it belongs to, and
                // that present has not reached the screen yet. Pushing to
                // the display from this task is what we must not do, which
                // is why only the compositing half is called.
                xSemaphoreTake(g_canvas_mutex, portMAX_DELAY);
                LGFX_Sprite *fb = compose_screen_buffer(false);
                bool ok = fb ? export_frame_bmp(fb, full_path) : false;
                xSemaphoreGive(g_canvas_mutex);
                if (ok) {
                    ESP_LOGI(TAG, "EXPORT_FRAME: wrote %s", full_path);
                } else {
                    ESP_LOGE(TAG, "EXPORT_FRAME: failed for %s", full_path);
                    return -1;
                }
            }
#else
            // Retro (WROVER) has no encoder and no filesystem worth writing
            // pictures to. Say so once per call and carry on.
            ESP_LOGW(TAG, "EXPORT_FRAME: NOT_SUPPORTED (%.*s)",
                     (int)path_len, path_data);
#endif
            return 0;
        }

        case FMRB_LINK_GFX_CURSOR_SET_POSITION:
            if (size >= sizeof(fmrb_link_graphics_cursor_position_t)) {
                const fmrb_link_graphics_cursor_position_t *cmd = (const fmrb_link_graphics_cursor_position_t*)data;
                g_cursor_x = cmd->x;
                g_cursor_y = cmd->y;
                ESP_LOGD(TAG, "Cursor position updated: (%d, %d)", g_cursor_x, g_cursor_y);
                return 0;
            }
            break;

        case FMRB_LINK_GFX_CURSOR_SET_VISIBLE:
            if (size >= sizeof(fmrb_link_graphics_cursor_visible_t)) {
                const fmrb_link_graphics_cursor_visible_t *cmd = (const fmrb_link_graphics_cursor_visible_t*)data;
                g_cursor_visible = cmd->visible;
                ESP_LOGD(TAG, "Cursor visibility updated: %s", g_cursor_visible ? "visible" : "hidden");
                return 0;
            }
            break;

        case FMRB_LINK_GFX_CREATE_IMAGE_FROM_FILE: {
            if (size < sizeof(fmrb_link_graphics_create_image_from_file_t)) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: payload too small");
                return -1;
            }
            const fmrb_link_graphics_create_image_from_file_t *cmd =
                (const fmrb_link_graphics_create_image_from_file_t *)data;
            const char *path_data = (const char *)(data + sizeof(*cmd));
            uint16_t path_len = cmd->path_len;

            if (sizeof(*cmd) + path_len > size) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: path extends beyond payload");
                return -1;
            }

            // Build full path (skip leading '/' to avoid double-slash)
            const char *p = path_data;
            int plen = (int)path_len;
            if (plen > 0 && p[0] == '/') { p++; plen--; }

            char full_path[256];
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            snprintf(full_path, sizeof(full_path), "flash/%.*s", plen, p);
#else
            snprintf(full_path, sizeof(full_path), "/flash/%.*s", plen, p);
#endif

            // Find free slot
            int slot = -1;
            for (int i = 0; i < MAX_IMAGE_STORE; i++) {
                if (!g_image_store[i].in_use) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: image store full");
                return -1;
            }

            // Read PNG file
            FILE *fp = fopen(full_path, "rb");
            if (!fp) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: failed to open %s", full_path);
                return -1;
            }

            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            if (file_size <= 0 || file_size > 200000) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: invalid file size %ld", file_size);
                fclose(fp);
                return -1;
            }

            uint8_t *png_data;
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            png_data = (uint8_t *)malloc(file_size);
#else
            png_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
#endif
            if (!png_data) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: PSRAM alloc failed for %ld bytes", file_size);
                fclose(fp);
                return -1;
            }

            size_t bytes_read = fread(png_data, 1, file_size, fp);
            fclose(fp);

            if ((long)bytes_read != file_size) {
                ESP_LOGE(TAG, "CREATE_IMAGE_FROM_FILE: read error");
                free(png_data);
                return -1;
            }

            // Get PNG dimensions from header (bytes 16-23: width and height as 4-byte big-endian)
            uint16_t png_w = g_lgfx->width();
            uint16_t png_h = g_lgfx->height();
            if (file_size >= 24 && png_data[0] == 0x89 && png_data[1] == 0x50) {
                png_w = (png_data[16] << 24) | (png_data[17] << 16) | (png_data[18] << 8) | png_data[19];
                png_h = (png_data[20] << 24) | (png_data[21] << 16) | (png_data[22] << 8) | png_data[23];
                // Clamp to screen size
                if (png_w > (uint16_t)g_lgfx->width()) png_w = g_lgfx->width();
                if (png_h > (uint16_t)g_lgfx->height()) png_h = g_lgfx->height();
            }

            // Store raw PNG data for direct drawPng at draw time.
            // This ensures correct color rendering because drawPng handles
            // color conversion and alpha blending directly on the target canvas.
            uint16_t new_id = g_next_image_id++;
            g_image_store[slot].image_id = new_id;
            g_image_store[slot].sprite = nullptr;
            g_image_store[slot].png_data = png_data;  // Transfer ownership
            g_image_store[slot].png_data_len = (uint32_t)file_size;
            g_image_store[slot].width = png_w;
            g_image_store[slot].height = png_h;
            g_image_store[slot].in_use = true;

            ESP_LOGI(TAG, "CREATE_IMAGE_FROM_FILE: id=%u, path=%s, %ux%u",
                    new_id, full_path, png_w, png_h);

            // Send ACK with image_id
            {
                fmrb_link_graphics_image_created_t resp = {
                    .image_id = new_id,
                    .width = png_w,
                    .height = png_h
                };
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
                socket_server_send_ack(msg_type, seq, (const uint8_t *)&resp, sizeof(resp));
#else
                COMM_INTERFACE->send_ack(msg_type, seq, (const uint8_t *)&resp, sizeof(resp));
#endif
            }
            return 1;  // ACK already sent
        }

        case FMRB_LINK_GFX_DRAW_IMAGE: {
            if (size < sizeof(fmrb_link_graphics_draw_image_t)) {
                ESP_LOGE(TAG, "DRAW_IMAGE: payload too small");
                return -1;
            }
            const fmrb_link_graphics_draw_image_t *cmd =
                (const fmrb_link_graphics_draw_image_t *)data;

            // Find image
            image_store_entry_t *entry = NULL;
            for (int i = 0; i < MAX_IMAGE_STORE; i++) {
                if (g_image_store[i].in_use && g_image_store[i].image_id == cmd->image_id) {
                    entry = &g_image_store[i];
                    break;
                }
            }
            if (!entry) {
                ESP_LOGE(TAG, "DRAW_IMAGE: image_id=%u not found", cmd->image_id);
                return -1;
            }

            // Decode fixed-point scale (x256) to float
            float scale_x = (cmd->scale_x_fp8 != 0) ? cmd->scale_x_fp8 / 256.0f : 1.0f;
            float scale_y = (cmd->scale_y_fp8 != 0) ? cmd->scale_y_fp8 / 256.0f : scale_x;

            // Draw PNG directly onto target (correct color/alpha handling)
            if (entry->png_data && entry->png_data_len > 0) {
                if (cmd->canvas_id == FMRB_CANVAS_SCREEN) {
                    g_lgfx->drawPng(entry->png_data, entry->png_data_len,
                                    cmd->x, cmd->y, 0, 0, 0, 0, scale_x, scale_y);
                    ESP_LOGD(TAG, "DRAW_IMAGE: id=%u at (%d,%d) scale=%.2fx%.2f on screen",
                            cmd->image_id, cmd->x, cmd->y, scale_x, scale_y);
                } else {
                    canvas_state_t *canvas = canvas_state_find(cmd->canvas_id);
                    if (canvas && canvas->draw_buffer) {
                        canvas->draw_buffer->drawPng(entry->png_data, entry->png_data_len,
                                                     cmd->x, cmd->y, 0, 0, 0, 0, scale_x, scale_y);
                        canvas->dirty = true;
                        ESP_LOGD(TAG, "DRAW_IMAGE: id=%u at (%d,%d) scale=%.2fx%.2f on canvas %u",
                                cmd->image_id, cmd->x, cmd->y, scale_x, scale_y, cmd->canvas_id);
                    } else {
                        ESP_LOGE(TAG, "DRAW_IMAGE: canvas %u not found", cmd->canvas_id);
                        return -1;
                    }
                }
            } else {
                ESP_LOGE(TAG, "DRAW_IMAGE: image_id=%u has no data", cmd->image_id);
                return -1;
            }
            return 0;
        }

        case FMRB_LINK_GFX_DELETE_IMAGE: {
            if (size < sizeof(fmrb_link_graphics_delete_image_t)) {
                ESP_LOGE(TAG, "DELETE_IMAGE: payload too small");
                return -1;
            }
            const fmrb_link_graphics_delete_image_t *cmd =
                (const fmrb_link_graphics_delete_image_t *)data;

            for (int i = 0; i < MAX_IMAGE_STORE; i++) {
                if (g_image_store[i].in_use && g_image_store[i].image_id == cmd->image_id) {
                    if (g_image_store[i].sprite) {
                        g_image_store[i].sprite->deleteSprite();
                        delete g_image_store[i].sprite;
                    }
                    if (g_image_store[i].png_data) {
                        free(g_image_store[i].png_data);
                    }
                    g_image_store[i].in_use = false;
                    g_image_store[i].sprite = nullptr;
                    g_image_store[i].png_data = nullptr;
                    g_image_store[i].png_data_len = 0;
                    ESP_LOGI(TAG, "DELETE_IMAGE: id=%u deleted", cmd->image_id);
                    return 0;
                }
            }
            ESP_LOGW(TAG, "DELETE_IMAGE: id=%u not found", cmd->image_id);
            return 0;
        }

        case FMRB_LINK_GFX_SET_OUTPUT_LEVEL: {
            if (size < 1) return -1;
            uint8_t level = data[0];
#ifndef CONFIG_IDF_TARGET_LINUX
            if (g_lgfx) {
                auto panel = (lgfx::Panel_CVBS*)((lgfx::LGFX_Device*)g_lgfx)->getPanel();
                if (panel) {
                    panel->setOutputLevel(level);
                    ESP_LOGI(TAG, "SET_OUTPUT_LEVEL: %u", level);
                }
            }
#else
            ESP_LOGI(TAG, "SET_OUTPUT_LEVEL: %u (no-op on Linux)", level);
#endif
            return 0;
        }

        case FMRB_LINK_GFX_SET_CHROMA_LEVEL: {
            if (size < 1) return -1;
            uint8_t level = data[0];
#ifndef CONFIG_IDF_TARGET_LINUX
            if (g_lgfx) {
                auto panel = (lgfx::Panel_CVBS*)((lgfx::LGFX_Device*)g_lgfx)->getPanel();
                if (panel) {
                    panel->setChromaLevel(level);
                    ESP_LOGI(TAG, "SET_CHROMA_LEVEL: %u", level);
                }
            }
#else
            ESP_LOGI(TAG, "SET_CHROMA_LEVEL: %u (no-op on Linux)", level);
#endif
            return 0;
        }

        // --- Sprite commands ---

        case FMRB_LINK_GFX_CREATE_SPRITE_IMAGE: {
            if (size < sizeof(fmrb_link_graphics_create_sprite_image_t)) break;
            const fmrb_link_graphics_create_sprite_image_t *cmd =
                (const fmrb_link_graphics_create_sprite_image_t*)data;
            sprite_image_id_t id = sprite_manager_create_image(
                cmd->canvas_id, cmd->width, cmd->height,
                cmd->transparent_color, cmd->use_transparent != 0);
            ESP_LOGI(TAG, "CREATE_SPRITE_IMAGE: canvas=%u, %ux%u -> id=%u",
                     cmd->canvas_id, cmd->width, cmd->height, id);
            // Send ACK with image_id (same pattern as CREATE_CANVAS)
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            socket_server_send_ack(msg_type, seq, (const uint8_t*)&id, sizeof(id));
#else
            COMM_INTERFACE->send_ack(msg_type, seq, (const uint8_t*)&id, sizeof(id));
#endif
            return 1;  // ACK already sent
        }

        case FMRB_LINK_GFX_DELETE_SPRITE_IMAGE: {
            if (size < sizeof(fmrb_link_graphics_delete_sprite_image_t)) break;
            const fmrb_link_graphics_delete_sprite_image_t *cmd =
                (const fmrb_link_graphics_delete_sprite_image_t*)data;
            sprite_manager_delete_image(cmd->image_id);
            return 0;
        }

        case FMRB_LINK_GFX_SET_SPRITE_IMAGE_TARGET: {
            if (size < sizeof(fmrb_link_graphics_set_sprite_image_target_t)) break;
            const fmrb_link_graphics_set_sprite_image_target_t *cmd =
                (const fmrb_link_graphics_set_sprite_image_target_t*)data;
            g_sprite_image_target = cmd->image_id;  // 0 = reset to canvas
            ESP_LOGD(TAG, "Sprite image target: %u", cmd->image_id);
            return 0;
        }

        case FMRB_LINK_GFX_LOAD_SPRITE_IMAGE_BMP: {
            if (size < sizeof(fmrb_link_graphics_load_sprite_image_bmp_t)) break;
            const fmrb_link_graphics_load_sprite_image_bmp_t *cmd =
                (const fmrb_link_graphics_load_sprite_image_bmp_t*)data;
            const char *path_data = (const char *)(data + sizeof(*cmd));
            uint16_t path_len = cmd->path_len;

            if (sizeof(*cmd) + path_len > size) {
                ESP_LOGE(TAG, "LOAD_SPRITE_BMP: path extends beyond payload");
                return -1;
            }

            // Build full path
            const char *p = path_data;
            int plen = (int)path_len;
            if (plen > 0 && p[0] == '/') { p++; plen--; }

            char full_path[256];
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            snprintf(full_path, sizeof(full_path), "flash/%.*s", plen, p);
#else
            snprintf(full_path, sizeof(full_path), "/flash/%.*s", plen, p);
#endif

            // Get sprite image buffer
            LGFX_Sprite *spr = (LGFX_Sprite*)sprite_manager_get_image_sprite(cmd->image_id);
            if (!spr) {
                ESP_LOGE(TAG, "LOAD_SPRITE_BMP: image %u not found", cmd->image_id);
                return -1;
            }

            uint16_t img_w, img_h;
            sprite_manager_get_image_size(cmd->image_id, &img_w, &img_h);

            // Read BMP file
            FILE *fp = fopen(full_path, "rb");
            if (!fp) {
                ESP_LOGE(TAG, "LOAD_SPRITE_BMP: cannot open %s", full_path);
                return -1;
            }

            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            if (file_size <= 0 || file_size > 65536) {
                ESP_LOGE(TAG, "LOAD_SPRITE_BMP: invalid size %ld", file_size);
                fclose(fp);
                return -1;
            }

            uint8_t *bmp_buf = (uint8_t *)malloc(file_size);
            if (!bmp_buf) {
                ESP_LOGE(TAG, "LOAD_SPRITE_BMP: alloc failed");
                fclose(fp);
                return -1;
            }

            size_t bytes_read = fread(bmp_buf, 1, file_size, fp);
            fclose(fp);

            if ((long)bytes_read != file_size) {
                free(bmp_buf);
                return -1;
            }

            // Parse BMP
            fmrb_bmp332_t bmp;
            if (fmrb_bmp332_parse(bmp_buf, (size_t)file_size, &bmp) != 0) {
                ESP_LOGE(TAG, "LOAD_SPRITE_BMP: parse failed for %s", full_path);
                free(bmp_buf);
                return -1;
            }

            // Copy pixels to sprite buffer (clamp to sprite size)
            uint16_t copy_w = (bmp.width < img_w) ? bmp.width : img_w;
            uint16_t copy_h = (bmp.height < img_h) ? bmp.height : img_h;
            for (uint16_t y = 0; y < copy_h; y++) {
                for (uint16_t x = 0; x < copy_w; x++) {
                    spr->drawPixel(x, y, bmp.pixels[y * bmp.width + x]);
                }
            }

            free(bmp_buf);
            ESP_LOGI(TAG, "LOAD_SPRITE_BMP: loaded %s (%ux%u) into image %u",
                     full_path, bmp.width, bmp.height, cmd->image_id);
            return 0;
        }

        case FMRB_LINK_GFX_CREATE_SPRITE_INSTANCE: {
            if (size < sizeof(fmrb_link_graphics_create_sprite_instance_t)) break;
            const fmrb_link_graphics_create_sprite_instance_t *cmd =
                (const fmrb_link_graphics_create_sprite_instance_t*)data;
            // Copy image_ids to avoid unaligned access on packed struct
            uint16_t image_ids_copy[FMRB_SPRITE_MAX_FRAMES];
            memcpy(image_ids_copy, cmd->image_ids, sizeof(uint16_t) * cmd->frame_count);
            sprite_instance_id_t id = sprite_manager_create_instance(
                cmd->canvas_id, image_ids_copy, cmd->frame_count,
                cmd->x, cmd->y, cmd->z_order);
            ESP_LOGD(TAG, "CREATE_SPRITE_INSTANCE: canvas=%u, frames=%u -> id=%u",
                     cmd->canvas_id, cmd->frame_count, id);
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            socket_server_send_ack(msg_type, seq, (const uint8_t*)&id, sizeof(id));
#else
            COMM_INTERFACE->send_ack(msg_type, seq, (const uint8_t*)&id, sizeof(id));
#endif
            return 1;  // ACK already sent
        }

        case FMRB_LINK_GFX_DELETE_SPRITE_INSTANCE: {
            if (size < sizeof(fmrb_link_graphics_delete_sprite_instance_t)) break;
            const fmrb_link_graphics_delete_sprite_instance_t *cmd =
                (const fmrb_link_graphics_delete_sprite_instance_t*)data;
            sprite_manager_delete_instance(cmd->instance_id);
            return 0;
        }

        case FMRB_LINK_GFX_SPRITE_INSTANCE_MOVE: {
            if (size < sizeof(fmrb_link_graphics_sprite_instance_move_t)) break;
            const fmrb_link_graphics_sprite_instance_move_t *cmd =
                (const fmrb_link_graphics_sprite_instance_move_t*)data;
            sprite_manager_move_instance(cmd->instance_id, cmd->x, cmd->y);
            return 0;
        }

        case FMRB_LINK_GFX_SPRITE_INSTANCE_SET_VISIBLE: {
            if (size < sizeof(fmrb_link_graphics_sprite_instance_set_visible_t)) break;
            const fmrb_link_graphics_sprite_instance_set_visible_t *cmd =
                (const fmrb_link_graphics_sprite_instance_set_visible_t*)data;
            sprite_manager_set_instance_visible(cmd->instance_id, cmd->visible != 0);
            return 0;
        }

        case FMRB_LINK_GFX_SPRITE_INSTANCE_SET_FRAME: {
            if (size < sizeof(fmrb_link_graphics_sprite_instance_set_frame_t)) break;
            const fmrb_link_graphics_sprite_instance_set_frame_t *cmd =
                (const fmrb_link_graphics_sprite_instance_set_frame_t*)data;
            sprite_manager_set_instance_frame(cmd->instance_id, cmd->frame_index);
            return 0;
        }

        case FMRB_LINK_GFX_DELETE_ALL_SPRITES: {
            if (size < sizeof(fmrb_link_graphics_delete_all_sprites_t)) break;
            const fmrb_link_graphics_delete_all_sprites_t *cmd =
                (const fmrb_link_graphics_delete_all_sprites_t*)data;
            sprite_manager_delete_all_for_canvas(cmd->canvas_id);
            return 0;
        }

        // ---------- GfxBlock VM ----------
        case FMRB_LINK_GFX_DEFINE_PROG: {
            if (size < sizeof(fmrb_link_graphics_define_prog_t)) break;
            const fmrb_link_graphics_define_prog_t *cmd =
                (const fmrb_link_graphics_define_prog_t*)data;
            size_t expected = sizeof(*cmd) + cmd->bytecode_len + cmd->strtable_len;
            if (size < expected) {
                ESP_LOGE(TAG, "DEFINE_PROG: truncated payload (got=%zu expected=%zu)",
                         size, expected);
                return -1;
            }
            const uint8_t *bytecode = data + sizeof(*cmd);
            const uint8_t *strtable = bytecode + cmd->bytecode_len;

            uint8_t prog_id = gfx_vm_define_prog(cmd->canvas_id,
                                                 bytecode, cmd->bytecode_len,
                                                 strtable, cmd->strtable_len);
            // Sync reply: single byte prog_id
#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)
            socket_server_send_ack(msg_type, seq, &prog_id, sizeof(prog_id));
#else
            COMM_INTERFACE->send_ack(msg_type, seq, &prog_id, sizeof(prog_id));
#endif
            return 1;  // ACK already sent
        }

        case FMRB_LINK_GFX_EXEC_PROG: {
            if (size < sizeof(fmrb_link_graphics_exec_prog_t)) break;
            const fmrb_link_graphics_exec_prog_t *cmd =
                (const fmrb_link_graphics_exec_prog_t*)data;
            size_t expected = sizeof(*cmd) + (size_t)cmd->reg_count * 3;
            if (size < expected) {
                ESP_LOGE(TAG, "EXEC_PROG: truncated payload (got=%zu expected=%zu)",
                         size, expected);
                return -1;
            }
            const uint8_t *reg_updates = data + sizeof(*cmd);

            canvas_state_t* canvas = canvas_state_find(cmd->canvas_id);
            if (!canvas || !canvas->draw_buffer) {
                ESP_LOGE(TAG, "EXEC_PROG: canvas %u not found", cmd->canvas_id);
                return -1;
            }
            gfx_vm_exec_prog(cmd->canvas_id, cmd->prog_id,
                             reg_updates, cmd->reg_count,
                             (void *)canvas->draw_buffer);
            canvas->dirty = true;
            return 0;
        }

        case FMRB_LINK_GFX_DELETE_PROG: {
            if (size < sizeof(fmrb_link_graphics_delete_prog_t)) break;
            const fmrb_link_graphics_delete_prog_t *cmd =
                (const fmrb_link_graphics_delete_prog_t*)data;
            gfx_vm_delete_prog(cmd->prog_id);
            return 0;
        }

        default:
            ESP_LOGE(TAG, "Unknown graphics command: 0x%02x", cmd_type);
            return -1;
    }

    ESP_LOGE(TAG, "Invalid command size for type 0x%02x (size=%zu)", cmd_type, size);
    return -1;
}
