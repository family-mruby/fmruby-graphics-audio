#include "fmrb_sprite_pool.h"
#include "fmrb_mempool.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "sprite_pool";

// Allocation tracking entry
typedef struct {
    void *ptr;
    size_t size;
    uint16_t canvas_id;
    bool in_use;
} sprite_alloc_entry_t;

// Pool state
static mempool_tlsf_t s_pool;
static sprite_alloc_entry_t s_allocs[FMRB_SPRITE_MAX_ALLOCS];
static bool s_initialized = false;

static sprite_alloc_entry_t* find_free_entry(void) {
    for (int i = 0; i < FMRB_SPRITE_MAX_ALLOCS; i++) {
        if (!s_allocs[i].in_use) {
            return &s_allocs[i];
        }
    }
    return NULL;
}

static sprite_alloc_entry_t* find_entry_by_ptr(void *ptr) {
    for (int i = 0; i < FMRB_SPRITE_MAX_ALLOCS; i++) {
        if (s_allocs[i].in_use && s_allocs[i].ptr == ptr) {
            return &s_allocs[i];
        }
    }
    return NULL;
}

int fmrb_sprite_pool_init(size_t pool_size) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return 0;
    }

    if (pool_size == 0) {
        pool_size = FMRB_SPRITE_POOL_DEFAULT_SIZE;
    }

    bool use_psram = true;
#ifndef CONFIG_IDF_TARGET_ESP32
    use_psram = false;
#endif

    if (mempool_tlsf_init(&s_pool, pool_size, use_psram) != 0) {
        ESP_LOGE(TAG, "Failed to init TLSF pool");
        return -1;
    }

    memset(s_allocs, 0, sizeof(s_allocs));
    s_initialized = true;

    ESP_LOGI(TAG, "Sprite pool initialized: %zu bytes (%.1f KB)",
             pool_size, (float)pool_size / 1024.0f);
    return 0;
}

void fmrb_sprite_pool_deinit(void) {
    if (!s_initialized) return;

    mempool_tlsf_deinit(&s_pool);
    memset(s_allocs, 0, sizeof(s_allocs));
    s_initialized = false;

    ESP_LOGI(TAG, "Sprite pool deinitialized");
}

void* fmrb_sprite_pool_alloc(size_t size, uint16_t canvas_id) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "Pool not initialized");
        return NULL;
    }

    sprite_alloc_entry_t *entry = find_free_entry();
    if (!entry) {
        ESP_LOGE(TAG, "Max allocations reached (%d)", FMRB_SPRITE_MAX_ALLOCS);
        return NULL;
    }

    void *ptr = mempool_tlsf_alloc(&s_pool, size);
    if (!ptr) {
        return NULL;
    }

    memset(ptr, 0, size);

    entry->ptr = ptr;
    entry->size = size;
    entry->canvas_id = canvas_id;
    entry->in_use = true;

    return ptr;
}

void fmrb_sprite_pool_free(void *ptr) {
    if (!s_initialized || !ptr) return;

    sprite_alloc_entry_t *entry = find_entry_by_ptr(ptr);
    if (!entry) {
        ESP_LOGW(TAG, "Pointer %p not tracked (may be double-free)", ptr);
        return;
    }

    mempool_tlsf_free(&s_pool, ptr);
    entry->in_use = false;
    entry->ptr = NULL;
}

int fmrb_sprite_pool_free_by_canvas(uint16_t canvas_id) {
    if (!s_initialized) return 0;

    int freed = 0;
    for (int i = 0; i < FMRB_SPRITE_MAX_ALLOCS; i++) {
        if (s_allocs[i].in_use && s_allocs[i].canvas_id == canvas_id) {
            mempool_tlsf_free(&s_pool, s_allocs[i].ptr);
            s_allocs[i].in_use = false;
            s_allocs[i].ptr = NULL;
            freed++;
        }
    }

    if (freed > 0) {
        ESP_LOGI(TAG, "Freed %d allocations for canvas %u", freed, canvas_id);
    }
    return freed;
}

void fmrb_sprite_pool_get_stats(size_t *total_bytes, size_t *used_bytes,
                                 size_t *free_bytes, size_t *alloc_count) {
    if (!s_initialized) {
        if (total_bytes) *total_bytes = 0;
        if (used_bytes) *used_bytes = 0;
        if (free_bytes) *free_bytes = 0;
        if (alloc_count) *alloc_count = 0;
        return;
    }

    mempool_tlsf_stats(&s_pool, total_bytes, used_bytes, free_bytes);

    if (alloc_count) {
        size_t count = 0;
        for (int i = 0; i < FMRB_SPRITE_MAX_ALLOCS; i++) {
            if (s_allocs[i].in_use) count++;
        }
        *alloc_count = count;
    }
}
