#include "message_handler.h"
#include "fmrb_link_protocol.h"
#include "graphics_handler.h"
#include "audio_handler.h"
#include "comm_interface.h"
#include <stdio.h>
#include <string.h>

// Forward declaration - implemented in main.cpp
extern int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth);

/**
 * Handle CONTROL messages
 */
static int handle_control_message(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                                  const uint8_t *payload, size_t payload_len) {
    const comm_interface_t *comm = comm_get_interface();
    if (!comm) {
        fprintf(stderr, "[MSG_HANDLER] No comm interface available\n");
        return -1;
    }

    switch (sub_cmd) {
        case FMRB_LINK_CONTROL_VERSION:
            if (payload_len >= 1) {
                uint8_t remote_version = payload[0];
                uint8_t local_version = FMRB_LINK_PROTOCOL_VERSION;

                printf("[MSG_HANDLER] VERSION check: remote=%d, local=%d, seq=%u\n",
                       remote_version, local_version, seq);

                // Send version response via ACK
                int result = comm->send_ack(type, seq, &local_version, sizeof(local_version));

                if (result == 0) {
                    printf("[MSG_HANDLER] VERSION ACK sent successfully\n");
                } else {
                    fprintf(stderr, "[MSG_HANDLER] VERSION ACK send failed: %d\n", result);
                }

                if (remote_version != local_version) {
                    fprintf(stderr, "[MSG_HANDLER] WARNING: Protocol version mismatch! remote=%d, local=%d\n",
                            remote_version, local_version);
                }
                return result;
            }
            break;

        case FMRB_LINK_CONTROL_INIT_DISPLAY:
            if (payload_len >= sizeof(fmrb_control_init_display_t)) {
                const fmrb_control_init_display_t *init_cmd = (const fmrb_control_init_display_t*)payload;
                printf("[MSG_HANDLER] INIT_DISPLAY: %dx%d, %d-bit\n",
                       init_cmd->width, init_cmd->height, init_cmd->color_depth);

                int result = init_display_callback(init_cmd->width, init_cmd->height, init_cmd->color_depth);

                // Send ACK to prevent retransmission
                if (result == 0) {
                    comm->send_ack(type, seq, NULL, 0);
                }
                return result;
            }
            break;

        default:
            fprintf(stderr, "[MSG_HANDLER] Unknown control command: 0x%02x\n", sub_cmd);
            return -1;
    }

    return -1;
}

/**
 * Handle GRAPHICS messages
 */
static int handle_graphics_message(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                                   const uint8_t *payload, size_t payload_len) {
    const comm_interface_t *comm = comm_get_interface();
    if (!comm) {
        fprintf(stderr, "[MSG_HANDLER] No comm interface available\n");
        return -1;
    }

    // Pass to graphics handler
    int result = graphics_handler_process_command(type, sub_cmd, seq, payload, payload_len);

    // Send ACK to prevent retransmission
    if (result == 0) {
        comm->send_ack(type, seq, NULL, 0);
    }

    return result;
}

/**
 * Handle AUDIO messages
 */
static int handle_audio_message(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                               const uint8_t *payload, size_t payload_len) {
    (void)type;
    (void)seq;
    (void)sub_cmd;

    // Pass to audio handler
    return audio_handler_process_command(payload, payload_len);
}

/**
 * Main message processing dispatcher
 */
int message_handler_process(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                           const uint8_t *payload, size_t payload_len) {
    // Strip ACK_REQUIRED flag for type matching
    uint8_t base_type = type & 0x7F;

    switch (base_type) {
        case FMRB_LINK_TYPE_CONTROL:
            return handle_control_message(type, seq, sub_cmd, payload, payload_len);

        case FMRB_LINK_TYPE_GRAPHICS:
            return handle_graphics_message(type, seq, sub_cmd, payload, payload_len);

        case FMRB_LINK_TYPE_AUDIO:
            return handle_audio_message(type, seq, sub_cmd, payload, payload_len);

        default:
            fprintf(stderr, "[MSG_HANDLER] Unknown message type: %u\n", type);
            return -1;
    }
}
