#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sprite pool configuration
#define FMRB_SPRITE_POOL_DEFAULT_SIZE  (256 * 1024)  // 256KB
#define FMRB_SPRITE_MAX_ALLOCS        128            // Max tracked allocations

/**
 * @brief Initialize sprite memory pool
 * Allocates a dedicated PSRAM region managed by TLSF (via multi_heap).
 * @param pool_size Total pool size in bytes (0 = use default 256KB)
 * @return 0 on success, -1 on failure
 */
int fmrb_sprite_pool_init(size_t pool_size);

/**
 * @brief Deinitialize sprite pool and free all resources
 */
void fmrb_sprite_pool_deinit(void);

/**
 * @brief Allocate memory from sprite pool
 * @param size Requested size in bytes
 * @param canvas_id Owner canvas ID (for bulk cleanup)
 * @return Pointer to allocated memory, or NULL on failure
 */
void* fmrb_sprite_pool_alloc(size_t size, uint16_t canvas_id);

/**
 * @brief Free memory back to sprite pool
 * @param ptr Pointer to free
 */
void fmrb_sprite_pool_free(void *ptr);

/**
 * @brief Free all allocations belonging to a canvas
 * Used when a Window is destroyed.
 * @param canvas_id Canvas ID whose allocations to free
 * @return Number of allocations freed
 */
int fmrb_sprite_pool_free_by_canvas(uint16_t canvas_id);

/**
 * @brief Get pool usage statistics
 * @param total_bytes Output: total pool size
 * @param used_bytes Output: bytes currently allocated
 * @param free_bytes Output: bytes available
 * @param alloc_count Output: number of active allocations
 */
void fmrb_sprite_pool_get_stats(size_t *total_bytes, size_t *used_bytes,
                                 size_t *free_bytes, size_t *alloc_count);

#ifdef __cplusplus
}
#endif
