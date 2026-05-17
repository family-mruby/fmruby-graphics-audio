#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fmrb_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// Limits
#define SPRITE_MAX_IMAGES     64
#define SPRITE_MAX_INSTANCES  128

// Opaque handle types
typedef uint16_t sprite_image_id_t;
typedef uint16_t sprite_instance_id_t;

/**
 * @brief Initialize sprite manager
 * Must be called after sprite pool init.
 * @return 0 on success, -1 on failure
 */
int sprite_manager_init(void);

/**
 * @brief Cleanup sprite manager
 */
void sprite_manager_cleanup(void);

// --- SpriteImage API ---

/**
 * @brief Create a sprite image buffer
 * @param canvas_id Parent canvas (owner)
 * @param width Image width
 * @param height Image height
 * @param transparent_color Color key for transparency
 * @param use_transparent Enable color-key transparency
 * @return image_id (>0) on success, 0 on failure
 */
sprite_image_id_t sprite_manager_create_image(uint16_t canvas_id,
    uint16_t width, uint16_t height,
    uint8_t transparent_color, bool use_transparent);

/**
 * @brief Delete a sprite image
 * Also deletes any instances referencing this image.
 * @param image_id Image to delete
 */
void sprite_manager_delete_image(sprite_image_id_t image_id);

/**
 * @brief Get the LGFX_Sprite for drawing to a sprite image
 * @param image_id Target image
 * @return Pointer to LGFX_Sprite, or NULL if not found
 */
void* sprite_manager_get_image_sprite(sprite_image_id_t image_id);

/**
 * @brief Get image dimensions
 * @param image_id Target image
 * @param width Output width
 * @param height Output height
 * @return 0 on success, -1 if not found
 */
int sprite_manager_get_image_size(sprite_image_id_t image_id,
    uint16_t *width, uint16_t *height);

/**
 * @brief Query whether an image uses color-key transparency.
 * @param image_id Target image
 * @param out_color Receives the transparent color when use_transparent is true
 *                  (untouched otherwise). May be NULL.
 * @return 1 if use_transparent is set, 0 if not set, -1 if image not found.
 */
int sprite_manager_get_image_transparent(sprite_image_id_t image_id,
    uint8_t *out_color);

// --- SpriteInstance API ---

/**
 * @brief Create a sprite instance (placement)
 * @param canvas_id Parent canvas
 * @param image_ids Array of image IDs for animation frames
 * @param frame_count Number of frames (1..FMRB_SPRITE_MAX_FRAMES)
 * @param x Window-local X position
 * @param y Window-local Y position
 * @param z_order Z-order within window
 * @return instance_id (>0) on success, 0 on failure
 */
sprite_instance_id_t sprite_manager_create_instance(uint16_t canvas_id,
    const uint16_t *image_ids, uint8_t frame_count,
    int16_t x, int16_t y, int16_t z_order);

/**
 * @brief Delete a sprite instance
 * @param instance_id Instance to delete
 */
void sprite_manager_delete_instance(sprite_instance_id_t instance_id);

/**
 * @brief Move a sprite instance
 * @param instance_id Target instance
 * @param x New X position
 * @param y New Y position
 */
void sprite_manager_move_instance(sprite_instance_id_t instance_id,
    int16_t x, int16_t y);

/**
 * @brief Set instance visibility
 * @param instance_id Target instance
 * @param visible true=visible, false=hidden
 */
void sprite_manager_set_instance_visible(sprite_instance_id_t instance_id,
    bool visible);

/**
 * @brief Set current animation frame
 * @param instance_id Target instance
 * @param frame_index Frame index (0..frame_count-1)
 */
void sprite_manager_set_instance_frame(sprite_instance_id_t instance_id,
    uint8_t frame_index);

// --- Bulk operations ---

/**
 * @brief Delete all sprites (images + instances) belonging to a canvas
 * @param canvas_id Canvas to clean up
 */
void sprite_manager_delete_all_for_canvas(uint16_t canvas_id);

// --- Rendering ---

/**
 * @brief Composite all visible sprites onto a canvas render buffer
 * Called during present() before Window-to-Window composite.
 * @param canvas_id Target canvas
 * @param render_buffer LGFX_Sprite to composite onto
 */
void sprite_manager_composite(uint16_t canvas_id, void *render_buffer);

#ifdef __cplusplus
}
#endif
