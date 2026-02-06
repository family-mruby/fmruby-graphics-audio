#include "fmrb_mempool.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_heap_caps.h"
#endif

static const char *TAG = "fmrb_mempool";

//---------------------------
// Generic memory pool implementation
//---------------------------

int mempool_init(mempool_t *pool, size_t block_size, size_t num_blocks, bool use_psram) {
    if (!pool || block_size == 0 || num_blocks == 0) {
        ESP_LOGE(TAG, "Invalid pool parameters");
        return -1;
    }

    // Allocate block descriptors
    pool->blocks = (mempool_block_t*)malloc(sizeof(mempool_block_t) * num_blocks);
    if (!pool->blocks) {
        ESP_LOGE(TAG, "Failed to allocate block descriptors");
        return -1;
    }

    // Calculate total memory needed
    size_t total_size = block_size * num_blocks;

    // Allocate memory for all blocks
#ifdef CONFIG_IDF_TARGET_ESP32
    if (use_psram) {
        pool->base_ptr = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
        if (pool->base_ptr) {
            ESP_LOGI(TAG, "Allocated %zu bytes from PSRAM for %zu blocks", total_size, num_blocks);
        } else {
            ESP_LOGW(TAG, "PSRAM allocation failed, falling back to internal RAM");
            pool->base_ptr = malloc(total_size);
        }
    } else {
        pool->base_ptr = malloc(total_size);
        ESP_LOGI(TAG, "Allocated %zu bytes from internal RAM for %zu blocks", total_size, num_blocks);
    }
#else
    pool->base_ptr = malloc(total_size);
    ESP_LOGI(TAG, "Allocated %zu bytes from heap for %zu blocks", total_size, num_blocks);
#endif

    if (!pool->base_ptr) {
        ESP_LOGE(TAG, "Failed to allocate memory pool (%zu bytes)", total_size);
        free(pool->blocks);
        return -1;
    }

    // Initialize block descriptors and build free list
    pool->num_blocks = num_blocks;
    pool->block_size = block_size;
    pool->free_list = &pool->blocks[0];

    for (size_t i = 0; i < num_blocks; i++) {
        pool->blocks[i].ptr = (uint8_t*)pool->base_ptr + (i * block_size);
        pool->blocks[i].size = block_size;
        pool->blocks[i].in_use = false;

        // Link to next block (last block points to NULL)
        if (i < num_blocks - 1) {
            pool->blocks[i].next = &pool->blocks[i + 1];
        } else {
            pool->blocks[i].next = NULL;
        }
    }

    ESP_LOGI(TAG, "Memory pool initialized: %zu blocks of %zu bytes (total: %zu bytes)",
             num_blocks, block_size, total_size);

    return 0;
}

void mempool_deinit(mempool_t *pool) {
    if (!pool) return;

    if (pool->base_ptr) {
        free(pool->base_ptr);
        pool->base_ptr = NULL;
    }

    if (pool->blocks) {
        free(pool->blocks);
        pool->blocks = NULL;
    }

    pool->free_list = NULL;
    pool->num_blocks = 0;
    pool->block_size = 0;

    ESP_LOGI(TAG, "Memory pool deinitialized");
}

void* mempool_alloc(mempool_t *pool) {
    if (!pool || !pool->free_list) {
        ESP_LOGE(TAG, "Pool exhausted or not initialized");
        return NULL;
    }

    // Take first block from free list
    mempool_block_t *block = pool->free_list;
    pool->free_list = block->next;

    block->in_use = true;
    block->next = NULL;

    return block->ptr;
}

int mempool_free(mempool_t *pool, void *ptr) {
    if (!pool || !ptr) {
        return -1;
    }

    // Find the block corresponding to this pointer
    mempool_block_t *block = NULL;
    for (size_t i = 0; i < pool->num_blocks; i++) {
        if (pool->blocks[i].ptr == ptr) {
            block = &pool->blocks[i];
            break;
        }
    }

    if (!block) {
        ESP_LOGE(TAG, "Pointer %p not found in pool", ptr);
        return -1;
    }

    if (!block->in_use) {
        ESP_LOGW(TAG, "Double free detected for pointer %p", ptr);
        return -1;
    }

    // Mark as free and add to free list
    block->in_use = false;
    block->next = pool->free_list;
    pool->free_list = block;

    return 0;
}

void mempool_stats(const mempool_t *pool, size_t *used_blocks, size_t *free_blocks) {
    if (!pool) {
        if (used_blocks) *used_blocks = 0;
        if (free_blocks) *free_blocks = 0;
        return;
    }

    size_t used = 0;
    size_t free = 0;

    for (size_t i = 0; i < pool->num_blocks; i++) {
        if (pool->blocks[i].in_use) {
            used++;
        } else {
            free++;
        }
    }

    if (used_blocks) *used_blocks = used;
    if (free_blocks) *free_blocks = free;
}

//---------------------------
// Canvas-specific implementation
//---------------------------

static mempool_t g_canvas_pool;
static bool g_canvas_pool_initialized = false;
static size_t g_canvas_buffer_size = 0;  // Dynamic buffer size based on display

int fmrb_mempool_canvas_init(uint16_t width, uint16_t height, uint8_t color_depth) {
    if (g_canvas_pool_initialized) {
        ESP_LOGW(TAG, "Canvas pool already initialized");
        return 0;
    }

    // Calculate bytes per pixel from color depth
    // Currently only 8-bit (RGB332) is supported
    size_t bytes_per_pixel = (color_depth + 7) / 8;  // Round up to nearest byte

    // Calculate buffer size dynamically based on display dimensions
    g_canvas_buffer_size = (size_t)width * height * bytes_per_pixel;

    // Initialize pool with canvas buffer configuration
    // Use PSRAM on ESP32 if available
    bool use_psram = true;
#ifndef CONFIG_IDF_TARGET_ESP32
    use_psram = false;
#endif

    int ret = mempool_init(&g_canvas_pool, g_canvas_buffer_size, FMRB_CANVAS_MAX_BUFFERS, use_psram);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to initialize canvas memory pool");
        return -1;
    }

    g_canvas_pool_initialized = true;

    ESP_LOGI(TAG, "Canvas memory pool initialized:");
    ESP_LOGI(TAG, "  - Display: %dx%d, %d-bit color", width, height, color_depth);
    ESP_LOGI(TAG, "  - Buffer size: %zu bytes (%zu bytes/pixel)",
             g_canvas_buffer_size, bytes_per_pixel);
    ESP_LOGI(TAG, "  - Max buffers: %d (for %d canvases)",
             FMRB_CANVAS_MAX_BUFFERS, FMRB_CANVAS_MAX_CANVASES);
    ESP_LOGI(TAG, "  - Total pool: %zu bytes (%.2f MB)",
             g_canvas_buffer_size * FMRB_CANVAS_MAX_BUFFERS,
             (float)(g_canvas_buffer_size * FMRB_CANVAS_MAX_BUFFERS) / (1024.0f * 1024.0f));

    return 0;
}

void fmrb_mempool_canvas_deinit(void) {
    if (!g_canvas_pool_initialized) {
        return;
    }

    mempool_deinit(&g_canvas_pool);
    g_canvas_pool_initialized = false;

    ESP_LOGI(TAG, "Canvas memory pool deinitialized");
}

void* fmrb_mempool_canvas_alloc_buffer(void) {
    if (!g_canvas_pool_initialized) {
        ESP_LOGE(TAG, "Canvas pool not initialized");
        return NULL;
    }

    void *buffer = mempool_alloc(&g_canvas_pool);
    if (!buffer) {
        size_t used, free;
        fmrb_mempool_canvas_get_stats(&used, &free);
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (used: %zu, free: %zu)", used, free);
        return NULL;
    }

    // Clear buffer to avoid garbage data
    memset(buffer, 0, g_canvas_buffer_size);

    return buffer;
}

int fmrb_mempool_canvas_free_buffer(void *buffer) {
    if (!g_canvas_pool_initialized) {
        ESP_LOGE(TAG, "Canvas pool not initialized");
        return -1;
    }

    return mempool_free(&g_canvas_pool, buffer);
}

void fmrb_mempool_canvas_get_stats(size_t *used_buffers, size_t *free_buffers) {
    if (!g_canvas_pool_initialized) {
        if (used_buffers) *used_buffers = 0;
        if (free_buffers) *free_buffers = 0;
        return;
    }

    mempool_stats(&g_canvas_pool, used_buffers, free_buffers);
}
