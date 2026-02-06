#pragma once

#include <stdint.h>
#include <stddef.h>
#include "fmrb_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize graphics handler
 * @return 0 on success, -1 on error
 */
int graphics_handler_init(void);

/**
 * @brief Cleanup graphics handler
 */
void graphics_handler_cleanup(void);

/**
 * @brief Process graphics command
 * @param msg_type Message type (for ACK response)
 * @param cmd_type Graphics command type (from msgpack sub_cmd)
 * @param seq Sequence number
 * @param data Command data (structure only, no cmd_type prefix)
 * @param size Data size
 * @return 0 on success, -1 on error
 */
int graphics_handler_process_command(uint8_t msg_type, uint8_t cmd_type, uint8_t seq, const uint8_t *data, size_t size);

// SDL renderer functions removed - not needed in abstracted interface

/**
 * @brief Render all canvases to screen in Z-order
 * This function composites all visible canvases to the screen based on their Z-order.
 * Should be called periodically (e.g., every frame in main loop).
 */
void graphics_handler_render_frame(void);

#ifdef __cplusplus
}
#endif