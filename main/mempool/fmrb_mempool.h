#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Canvas buffer configuration
#define FMRB_CANVAS_BUFFER_BPP    1  // RGB332 format
#define FMRB_CANVAS_MAX_CANVASES  16
#define FMRB_CANVAS_MAX_BUFFERS   (FMRB_CANVAS_MAX_CANVASES * 2)  // 2 buffers per canvas (draw + render)

// Generic memory pool structures
typedef struct mempool_block {
    void *ptr;                    // Pointer to memory block
    size_t size;                  // Size of the block
    bool in_use;                  // Allocation status
    struct mempool_block *next;   // Next block in free list
} mempool_block_t;

typedef struct {
    mempool_block_t *blocks;      // Array of blocks
    size_t num_blocks;            // Total number of blocks
    size_t block_size;            // Size of each block
    mempool_block_t *free_list;   // Head of free list
    void *base_ptr;               // Base pointer to allocated memory
} mempool_t;

/**
 * @brief Initialize generic memory pool
 * @param pool Pool structure
 * @param block_size Size of each block
 * @param num_blocks Number of blocks
 * @param use_psram Use PSRAM if available (ESP32 only)
 * @return 0 on success, -1 on failure
 */
int mempool_init(mempool_t *pool, size_t block_size, size_t num_blocks, bool use_psram);

/**
 * @brief Deinitialize memory pool and free all resources
 * @param pool Pool structure
 */
void mempool_deinit(mempool_t *pool);

/**
 * @brief Allocate a block from the pool
 * @param pool Pool structure
 * @return Pointer to allocated block, or NULL if pool is full
 */
void* mempool_alloc(mempool_t *pool);

/**
 * @brief Free a block back to the pool
 * @param pool Pool structure
 * @param ptr Pointer to block to free
 * @return 0 on success, -1 if pointer not found in pool
 */
int mempool_free(mempool_t *pool, void *ptr);

/**
 * @brief Get pool statistics
 * @param pool Pool structure
 * @param used_blocks Output: number of blocks in use
 * @param free_blocks Output: number of free blocks
 */
void mempool_stats(const mempool_t *pool, size_t *used_blocks, size_t *free_blocks);

//---------------------------
// Canvas-specific API
//---------------------------

/**
 * @brief Initialize canvas memory pool
 * Pre-allocates memory for all canvas buffers based on display size
 * Uses ESP32 PSRAM if available, otherwise regular heap
 * @param width Display width in pixels
 * @param height Display height in pixels
 * @param color_depth Color depth in bits (8 for RGB332, reserved for future expansion)
 * @return 0 on success, -1 on failure
 */
int fmrb_mempool_canvas_init(uint16_t width, uint16_t height, uint8_t color_depth);

/**
 * @brief Deinitialize canvas memory pool
 * Frees all pre-allocated memory
 */
void fmrb_mempool_canvas_deinit(void);

/**
 * @brief Allocate a canvas buffer from the pool
 * @return Pointer to buffer (153,600 bytes), or NULL if pool is exhausted
 */
void* fmrb_mempool_canvas_alloc_buffer(void);

/**
 * @brief Free a canvas buffer back to the pool
 * @param buffer Pointer to buffer to free
 * @return 0 on success, -1 if buffer not found in pool
 */
int fmrb_mempool_canvas_free_buffer(void *buffer);

/**
 * @brief Get canvas pool usage statistics
 * @param used_buffers Output: number of buffers in use
 * @param free_buffers Output: number of free buffers
 */
void fmrb_mempool_canvas_get_stats(size_t *used_buffers, size_t *free_buffers);

#ifdef __cplusplus
}
#endif
