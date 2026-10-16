/**
 * @file display_shm.cpp
 * @brief Display interface using POSIX shared memory for Linux headless builds.
 *        Sends framebuffer to a separate SDL2 display process.
 */
#include "display_interface.h"
#include <LovyanGFX.hpp>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "shm_display.h"
#include "display_shm.h"

static const char *TAG = "display_shm";

// Headless LGFX using a plain Sprite as the screen buffer. The sprite draws
// into g_screen_mem, which is large enough for the base screen and for the
// high-resolution screen, so a screen size change is a setBuffer on the same
// sprite: graphics_handler holds this pointer as its g_lgfx and keeps it.
static lgfx::LGFX_Sprite* g_screen_sprite = nullptr;
static uint8_t* g_screen_mem = nullptr;
static size_t g_screen_mem_size = 0;
static fmrb_shm_t* g_shm = nullptr;
static int g_shm_fd = -1;
static sem_t* g_sem_frame = SEM_FAILED;
static uint16_t g_width = 0;
static uint16_t g_height = 0;
static uint16_t g_base_width = 0;
static uint16_t g_base_height = 0;

static int shm_display_init(uint16_t width, uint16_t height, uint8_t color_depth,
                            uint8_t margin_x, uint8_t margin_y) {
    (void)margin_x;
    (void)margin_y;
    ESP_LOGI(TAG, "Initializing SHM display: %dx%d, %d-bit color", width, height, color_depth);

    if (width > FMRB_SHM_MAX_WIDTH || height > FMRB_SHM_MAX_HEIGHT) {
        ESP_LOGE(TAG, "Display size %dx%d exceeds SHM max %dx%d",
                 width, height, FMRB_SHM_MAX_WIDTH, FMRB_SHM_MAX_HEIGHT);
        return -1;
    }

    // Open shared memory (create if not exists)
    // Retry on EINTR (FreeRTOS SIGALRM can interrupt syscalls)
    do {
        g_shm_fd = shm_open(FMRB_SHM_NAME, O_CREAT | O_RDWR, 0666);
    } while (g_shm_fd < 0 && errno == EINTR);
    if (g_shm_fd < 0) {
        ESP_LOGE(TAG, "shm_open failed: %s", strerror(errno));
        return -1;
    }

    if (ftruncate(g_shm_fd, sizeof(fmrb_shm_t)) < 0) {
        ESP_LOGE(TAG, "ftruncate failed: %s", strerror(errno));
        close(g_shm_fd);
        shm_unlink(FMRB_SHM_NAME);
        return -1;
    }

    g_shm = (fmrb_shm_t*)mmap(NULL, sizeof(fmrb_shm_t),
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        ESP_LOGE(TAG, "mmap failed: %s", strerror(errno));
        close(g_shm_fd);
        shm_unlink(FMRB_SHM_NAME);
        return -1;
    }

    // Initialize shared memory (memset first to clear stale data)
    memset(g_shm, 0, sizeof(fmrb_shm_t));
    g_shm->display_width = width;
    g_shm->display_height = height;
    g_shm->color_depth = color_depth;
    g_shm->scaling_x = 2;
    g_shm->scaling_y = 2;
    g_shm->layout_version = FMRB_SHM_LAYOUT_VERSION;
    g_shm->shm_size = (uint32_t)sizeof(fmrb_shm_t);
    for (int i = 0; i < 2; i++) {
        g_shm->frame_width[i] = width;
        g_shm->frame_height[i] = height;
    }
    // Set magic LAST to signal SDL2 side that SHM is freshly initialized
    g_shm->ready_magic = FMRB_SHM_READY_MAGIC;

    // Create named semaphore for frame synchronization
    sem_unlink(FMRB_SEM_FRAME_NAME);  // Remove stale semaphore
    do {
        g_sem_frame = sem_open(FMRB_SEM_FRAME_NAME, O_CREAT | O_EXCL, 0666, 0);
    } while (g_sem_frame == SEM_FAILED && errno == EINTR);
    if (g_sem_frame == SEM_FAILED) {
        ESP_LOGE(TAG, "sem_open failed: %s", strerror(errno));
        munmap(g_shm, sizeof(fmrb_shm_t));
        close(g_shm_fd);
        shm_unlink(FMRB_SHM_NAME);
        return -1;
    }

    // Create headless LGFX_Sprite as the screen buffer, over memory sized for
    // the larger of the base and the high-resolution screen.
    {
        size_t base_px = (size_t)width * height;
        size_t hires_px = (size_t)FMRB_SHM_HIRES_WIDTH * FMRB_SHM_HIRES_HEIGHT;
        g_screen_mem_size = (base_px > hires_px ? base_px : hires_px) * ((color_depth + 7) / 8);
    }
    g_screen_mem = (uint8_t*)calloc(1, g_screen_mem_size);
    g_screen_sprite = new lgfx::LGFX_Sprite(nullptr);
    g_screen_sprite->setColorDepth(color_depth);
    if (g_screen_mem) {
        g_screen_sprite->setBuffer(g_screen_mem, width, height, color_depth);
    }
    if (!g_screen_mem || !g_screen_sprite->getBuffer()) {
        ESP_LOGE(TAG, "Failed to create screen sprite %dx%d", width, height);
        delete g_screen_sprite;
        g_screen_sprite = nullptr;
        free(g_screen_mem);
        g_screen_mem = nullptr;
        sem_close(g_sem_frame);
        sem_unlink(FMRB_SEM_FRAME_NAME);
        munmap(g_shm, sizeof(fmrb_shm_t));
        close(g_shm_fd);
        shm_unlink(FMRB_SHM_NAME);
        return -1;
    }
    g_screen_sprite->fillScreen(0);

    g_width = width;
    g_height = height;
    g_base_width = width;
    g_base_height = height;

    // Wait for SDL2 display process to connect
    // Use vTaskDelay instead of usleep (FreeRTOS SIGALRM interrupts usleep)
    ESP_LOGI(TAG, "Waiting for SDL2 display process...");
    int wait_count = 0;
    while (!g_shm->display_initialized) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
        if (wait_count % 50 == 0) {
            // An sdl2-display image built from an older shm_display.h waits
            // for the old magic and never answers this one.
            ESP_LOGW(TAG, "Still waiting for SDL2 display process (%d seconds)... "
                     "If it never connects, its image may predate SHM layout %d: "
                     "docker compose build sdl2-display",
                     wait_count / 10, FMRB_SHM_LAYOUT_VERSION);
        }
    }
    ESP_LOGI(TAG, "SDL2 display process connected");

    ESP_LOGI(TAG, "SHM display initialized: %dx%d, %d-bit (SHM layout %d, %u bytes)",
             width, height, color_depth, FMRB_SHM_LAYOUT_VERSION, (unsigned)sizeof(fmrb_shm_t));
    return 0;
}

int shm_display_set_screen_size(uint16_t width, uint16_t height) {
    if (!g_screen_sprite || !g_screen_mem) return -1;
    if (width == 0 || height == 0) {
        width = g_base_width;
        height = g_base_height;
    }
    if ((size_t)width * height > g_screen_mem_size ||
        (size_t)width * height > FMRB_SHM_FRAMEBUF_SIZE) {
        ESP_LOGE(TAG, "screen size %ux%u does not fit the screen buffer", width, height);
        return -1;
    }
    if (width == g_width && height == g_height) return 0;
    // Called from the render task only, between two frames: present() runs
    // on the same task, so no frame is copied while the size changes.
    g_screen_sprite->setBuffer(g_screen_mem, width, height, g_screen_sprite->getColorDepth());
    g_screen_sprite->fillScreen(0);
    g_width = width;
    g_height = height;
    ESP_LOGI(TAG, "screen size %ux%u (base %ux%u)", width, height, g_base_width, g_base_height);
    return 0;
}

void shm_display_get_base_size(uint16_t *width, uint16_t *height) {
    if (width) *width = g_base_width;
    if (height) *height = g_base_height;
}

static void* shm_display_get_lgfx(void) {
    return (void*)g_screen_sprite;
}

static int shm_display_process_events(void) {
    if (g_shm && g_shm->shutdown_requested) {
        return 1;  // Quit requested
    }
    return 0;
}

static void shm_display_present(void) {
    if (!g_shm || !g_screen_sprite || g_sem_frame == SEM_FAILED) {
        return;
    }

    // Get the sprite's pixel buffer
    const uint8_t* pixels = (const uint8_t*)g_screen_sprite->getBuffer();
    if (!pixels) return;

    // Write to the current write buffer
    uint32_t buf_idx = g_shm->write_index & 1;
    size_t frame_size = (size_t)g_width * g_height;
    memcpy(g_shm->framebuf[buf_idx], pixels, frame_size);
    g_shm->frame_width[buf_idx] = g_width;
    g_shm->frame_height[buf_idx] = g_height;

    // Swap buffers (after the size: the reader takes the size from the slot)
    g_shm->write_index = buf_idx ^ 1;

    // Signal the SDL2 process that a new frame is ready
    sem_post(g_sem_frame);
}

static void shm_display_cleanup(void) {
    ESP_LOGI(TAG, "Cleaning up SHM display");

    if (g_shm) {
        g_shm->shutdown_requested = 1;
    }

    if (g_screen_sprite) {
        g_screen_sprite->deleteSprite();
        delete g_screen_sprite;
        g_screen_sprite = nullptr;
    }
    free(g_screen_mem);
    g_screen_mem = nullptr;
    g_screen_mem_size = 0;

    if (g_sem_frame != SEM_FAILED) {
        sem_close(g_sem_frame);
        sem_unlink(FMRB_SEM_FRAME_NAME);
        g_sem_frame = SEM_FAILED;
    }

    if (g_shm) {
        munmap(g_shm, sizeof(fmrb_shm_t));
        g_shm = nullptr;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        shm_unlink(FMRB_SHM_NAME);
        g_shm_fd = -1;
    }
}

static int shm_display_get_scaling(uint_fast8_t* sx, uint_fast8_t* sy) {
    if (g_shm) {
        if (sx) *sx = g_shm->scaling_x;
        if (sy) *sy = g_shm->scaling_y;
        return 0;
    }
    return -1;
}

static const display_interface_t shm_display_interface = {
    .init = shm_display_init,
    .get_lgfx = shm_display_get_lgfx,
    .process_events = shm_display_process_events,
    .display = shm_display_present,
    .cleanup = shm_display_cleanup,
    .get_scaling = shm_display_get_scaling,
};

const display_interface_t* display_get_interface(void) {
    return &shm_display_interface;
}
