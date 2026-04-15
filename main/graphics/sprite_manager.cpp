#include <cstring>
#include <algorithm>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

extern "C" {
#include "sprite_manager.h"
#include "../mempool/fmrb_sprite_pool.h"
#include "esp_log.h"
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_heap_caps.h"
#endif
}

static const char *TAG = "sprite_mgr";

// Internal sprite image state
typedef struct {
    sprite_image_id_t id;
    uint16_t canvas_id;
    LGFX_Sprite *sprite;
    void *buffer_mem;           // Pixel buffer from sprite pool
    uint16_t width, height;
    uint8_t transparent_color;
    bool use_transparent;
    bool in_use;
} sprite_image_state_t;

// Internal sprite instance state
typedef struct {
    sprite_instance_id_t id;
    uint16_t canvas_id;
    uint16_t image_ids[FMRB_SPRITE_MAX_FRAMES];
    uint8_t frame_count;
    uint8_t current_frame;
    int16_t x, y;
    int16_t z_order;
    bool visible;
    bool in_use;
} sprite_instance_state_t;

static sprite_image_state_t s_images[SPRITE_MAX_IMAGES];
static sprite_instance_state_t s_instances[SPRITE_MAX_INSTANCES];
static uint16_t s_next_image_id = 1;
static uint16_t s_next_instance_id = 1;
static bool s_initialized = false;

// Temporary array for Z-order sorting during composite
static sprite_instance_state_t* s_sorted[SPRITE_MAX_INSTANCES];

// Helper: find image slot by ID
static sprite_image_state_t* find_image(sprite_image_id_t id) {
    for (int i = 0; i < SPRITE_MAX_IMAGES; i++) {
        if (s_images[i].in_use && s_images[i].id == id) {
            return &s_images[i];
        }
    }
    return nullptr;
}

// Helper: find instance slot by ID
static sprite_instance_state_t* find_instance(sprite_instance_id_t id) {
    for (int i = 0; i < SPRITE_MAX_INSTANCES; i++) {
        if (s_instances[i].in_use && s_instances[i].id == id) {
            return &s_instances[i];
        }
    }
    return nullptr;
}

// Helper: find free image slot
static sprite_image_state_t* find_free_image_slot(void) {
    for (int i = 0; i < SPRITE_MAX_IMAGES; i++) {
        if (!s_images[i].in_use) {
            return &s_images[i];
        }
    }
    return nullptr;
}

// Helper: find free instance slot
static sprite_instance_state_t* find_free_instance_slot(void) {
    for (int i = 0; i < SPRITE_MAX_INSTANCES; i++) {
        if (!s_instances[i].in_use) {
            return &s_instances[i];
        }
    }
    return nullptr;
}

int sprite_manager_init(void) {
    if (s_initialized) return 0;

    memset(s_images, 0, sizeof(s_images));
    memset(s_instances, 0, sizeof(s_instances));
    s_next_image_id = 1;
    s_next_instance_id = 1;
    s_initialized = true;

    ESP_LOGI(TAG, "Sprite manager initialized (max images=%d, max instances=%d)",
             SPRITE_MAX_IMAGES, SPRITE_MAX_INSTANCES);
    return 0;
}

void sprite_manager_cleanup(void) {
    if (!s_initialized) return;

    // Delete all images (which frees pool memory and LGFX_Sprite objects)
    for (int i = 0; i < SPRITE_MAX_IMAGES; i++) {
        if (s_images[i].in_use) {
            if (s_images[i].sprite) {
                delete s_images[i].sprite;
            }
            if (s_images[i].buffer_mem) {
                fmrb_sprite_pool_free(s_images[i].buffer_mem);
            }
        }
    }

    memset(s_images, 0, sizeof(s_images));
    memset(s_instances, 0, sizeof(s_instances));
    s_initialized = false;

    ESP_LOGI(TAG, "Sprite manager cleaned up");
}

sprite_image_id_t sprite_manager_create_image(uint16_t canvas_id,
    uint16_t width, uint16_t height,
    uint8_t transparent_color, bool use_transparent)
{
    if (!s_initialized) return 0;

    sprite_image_state_t *slot = find_free_image_slot();
    if (!slot) {
        ESP_LOGE(TAG, "No free image slots");
        return 0;
    }

    // Allocate pixel buffer from sprite pool (RGB332 = 1 byte/pixel)
    size_t buf_size = (size_t)width * height;
    void *buf = fmrb_sprite_pool_alloc(buf_size, canvas_id);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to alloc sprite buffer %ux%u (%zu bytes)",
                 width, height, buf_size);
        return 0;
    }

    // Create LGFX_Sprite object
    LGFX_Sprite *spr = new (std::nothrow) LGFX_Sprite();
    if (!spr) {
        ESP_LOGE(TAG, "Failed to create LGFX_Sprite");
        fmrb_sprite_pool_free(buf);
        return 0;
    }

    spr->setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
    spr->setBuffer(buf, width, height, lgfx::color_depth_t::rgb332_1Byte);

    uint16_t id = s_next_image_id++;
    if (s_next_image_id == 0) s_next_image_id = 1;  // Avoid 0

    slot->id = id;
    slot->canvas_id = canvas_id;
    slot->sprite = spr;
    slot->buffer_mem = buf;
    slot->width = width;
    slot->height = height;
    slot->transparent_color = transparent_color;
    slot->use_transparent = use_transparent;
    slot->in_use = true;

    ESP_LOGI(TAG, "Created sprite image: id=%u, %ux%u, canvas=%u",
             id, width, height, canvas_id);
    return id;
}

void sprite_manager_delete_image(sprite_image_id_t image_id) {
    sprite_image_state_t *img = find_image(image_id);
    if (!img) return;

    // Delete instances referencing this image
    for (int i = 0; i < SPRITE_MAX_INSTANCES; i++) {
        if (!s_instances[i].in_use) continue;
        for (int f = 0; f < s_instances[i].frame_count; f++) {
            if (s_instances[i].image_ids[f] == image_id) {
                s_instances[i].in_use = false;
                ESP_LOGI(TAG, "Deleted instance %u (referenced deleted image %u)",
                         s_instances[i].id, image_id);
                break;
            }
        }
    }

    if (img->sprite) {
        delete img->sprite;
    }
    if (img->buffer_mem) {
        fmrb_sprite_pool_free(img->buffer_mem);
    }

    img->in_use = false;
    ESP_LOGI(TAG, "Deleted sprite image: id=%u", image_id);
}

void* sprite_manager_get_image_sprite(sprite_image_id_t image_id) {
    sprite_image_state_t *img = find_image(image_id);
    if (!img) return nullptr;
    return img->sprite;
}

int sprite_manager_get_image_size(sprite_image_id_t image_id,
    uint16_t *width, uint16_t *height)
{
    sprite_image_state_t *img = find_image(image_id);
    if (!img) return -1;
    if (width) *width = img->width;
    if (height) *height = img->height;
    return 0;
}

sprite_instance_id_t sprite_manager_create_instance(uint16_t canvas_id,
    const uint16_t *image_ids, uint8_t frame_count,
    int16_t x, int16_t y, int16_t z_order)
{
    if (!s_initialized) return 0;

    if (frame_count == 0 || frame_count > FMRB_SPRITE_MAX_FRAMES) {
        ESP_LOGE(TAG, "Invalid frame_count: %u", frame_count);
        return 0;
    }

    // Validate all image IDs
    for (int i = 0; i < frame_count; i++) {
        if (!find_image(image_ids[i])) {
            ESP_LOGE(TAG, "Image ID %u not found (frame %d)", image_ids[i], i);
            return 0;
        }
    }

    sprite_instance_state_t *slot = find_free_instance_slot();
    if (!slot) {
        ESP_LOGE(TAG, "No free instance slots");
        return 0;
    }

    uint16_t id = s_next_instance_id++;
    if (s_next_instance_id == 0) s_next_instance_id = 1;

    slot->id = id;
    slot->canvas_id = canvas_id;
    memcpy(slot->image_ids, image_ids, sizeof(uint16_t) * frame_count);
    slot->frame_count = frame_count;
    slot->current_frame = 0;
    slot->x = x;
    slot->y = y;
    slot->z_order = z_order;
    slot->visible = true;
    slot->in_use = true;

    ESP_LOGI(TAG, "Created sprite instance: id=%u, canvas=%u, frames=%u, pos=(%d,%d), z=%d",
             id, canvas_id, frame_count, x, y, z_order);
    return id;
}

void sprite_manager_delete_instance(sprite_instance_id_t instance_id) {
    sprite_instance_state_t *inst = find_instance(instance_id);
    if (!inst) return;
    inst->in_use = false;
    ESP_LOGI(TAG, "Deleted sprite instance: id=%u", instance_id);
}

void sprite_manager_move_instance(sprite_instance_id_t instance_id,
    int16_t x, int16_t y)
{
    sprite_instance_state_t *inst = find_instance(instance_id);
    if (!inst) return;
    inst->x = x;
    inst->y = y;
}

void sprite_manager_set_instance_visible(sprite_instance_id_t instance_id,
    bool visible)
{
    sprite_instance_state_t *inst = find_instance(instance_id);
    if (!inst) return;
    inst->visible = visible;
}

void sprite_manager_set_instance_frame(sprite_instance_id_t instance_id,
    uint8_t frame_index)
{
    sprite_instance_state_t *inst = find_instance(instance_id);
    if (!inst) return;
    if (frame_index < inst->frame_count) {
        inst->current_frame = frame_index;
    }
}

void sprite_manager_delete_all_for_canvas(uint16_t canvas_id) {
    int del_images = 0, del_instances = 0;

    // Delete instances first
    for (int i = 0; i < SPRITE_MAX_INSTANCES; i++) {
        if (s_instances[i].in_use && s_instances[i].canvas_id == canvas_id) {
            s_instances[i].in_use = false;
            del_instances++;
        }
    }

    // Delete images (frees pool memory)
    for (int i = 0; i < SPRITE_MAX_IMAGES; i++) {
        if (s_images[i].in_use && s_images[i].canvas_id == canvas_id) {
            if (s_images[i].sprite) {
                delete s_images[i].sprite;
                s_images[i].sprite = nullptr;
            }
            if (s_images[i].buffer_mem) {
                fmrb_sprite_pool_free(s_images[i].buffer_mem);
                s_images[i].buffer_mem = nullptr;
            }
            s_images[i].in_use = false;
            del_images++;
        }
    }

    // Also free tracked pool allocations for this canvas
    fmrb_sprite_pool_free_by_canvas(canvas_id);

    if (del_images > 0 || del_instances > 0) {
        ESP_LOGI(TAG, "Deleted all sprites for canvas %u: %d images, %d instances",
                 canvas_id, del_images, del_instances);
    }
}

// Z-order comparison for sorting
static int z_order_compare(const void *a, const void *b) {
    const sprite_instance_state_t *ia = *(const sprite_instance_state_t **)a;
    const sprite_instance_state_t *ib = *(const sprite_instance_state_t **)b;
    return ia->z_order - ib->z_order;
}

void sprite_manager_composite(uint16_t canvas_id, void *render_buffer_ptr) {
    if (!s_initialized || !render_buffer_ptr) return;

    LGFX_Sprite *render_buf = static_cast<LGFX_Sprite*>(render_buffer_ptr);

    // Collect visible instances for this canvas
    int count = 0;
    for (int i = 0; i < SPRITE_MAX_INSTANCES; i++) {
        if (s_instances[i].in_use &&
            s_instances[i].visible &&
            s_instances[i].canvas_id == canvas_id)
        {
            s_sorted[count++] = &s_instances[i];
        }
    }

    if (count == 0) return;

    // Sort by Z-order (ascending: lower Z drawn first)
    qsort(s_sorted, count, sizeof(sprite_instance_state_t*), z_order_compare);

    // Composite each sprite onto render buffer
    for (int i = 0; i < count; i++) {
        sprite_instance_state_t *inst = s_sorted[i];

        // Get the current frame's image
        uint16_t img_id = inst->image_ids[inst->current_frame];
        sprite_image_state_t *img = find_image(img_id);
        if (!img || !img->sprite) continue;

        // Push sprite onto render buffer at instance position
        if (img->use_transparent) {
            // Use color-key transparency
            lgfx::rgb332_t trans_color;
            trans_color.raw = img->transparent_color;
            img->sprite->pushSprite(render_buf, inst->x, inst->y, trans_color);
        } else {
            img->sprite->pushSprite(render_buf, inst->x, inst->y);
        }
    }
}
