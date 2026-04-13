#include "message_handler_task.h"
#include "fmrb_link_protocol.h"
#include "graphics_handler.h"
#include "audio_handler.h"
#include "file_transfer_handler.h"
#include "comm_interface.h"
#include "comm_message.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include <string.h>
#include <stddef.h>

static const char *TAG = "msg_handler_task";
static volatile int task_running = 0;

// Forward declaration - implemented in main.cpp
extern int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth,
                                 uint8_t margin_x, uint8_t margin_y);

/**
 * Handle CONTROL messages
 */
static int handle_control_message(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                                  const uint8_t *payload, size_t payload_len) {
    const comm_interface_t *comm = comm_get_interface();
    if (!comm) {
        ESP_LOGE(TAG, "No comm interface available");
        return -1;
    }

    switch (sub_cmd) {
        case FMRB_LINK_CONTROL_VERSION:
            if (payload_len >= 1) {
                uint8_t remote_version = payload[0];
                uint8_t local_version = FMRB_LINK_PROTOCOL_VERSION;

                ESP_LOGI(TAG, "VERSION check: remote=%d, local=%d, seq=%u",
                       remote_version, local_version, seq);

                // Send version response via ACK (CONTROL always has ACK_REQUIRED)
                int result = (type & FMRB_LINK_FLAG_ACK_REQUIRED)
                    ? comm->send_ack(type, seq, &local_version, sizeof(local_version))
                    : 0;

                if (result == 0) {
                    ESP_LOGI(TAG, "VERSION ACK sent successfully");
                } else {
                    ESP_LOGE(TAG, "VERSION ACK send failed: %d", result);
                }

                if (remote_version != local_version) {
                    ESP_LOGE(TAG, "WARNING: Protocol version mismatch! remote=%d, local=%d",
                            remote_version, local_version);
                }
                return result;
            }
            break;

        case FMRB_LINK_CONTROL_INIT_DISPLAY:
            if (payload_len >= sizeof(fmrb_control_init_display_t)) {
                const fmrb_control_init_display_t *init_cmd = (const fmrb_control_init_display_t*)payload;
                ESP_LOGI(TAG, "INIT_DISPLAY: %dx%d, %d-bit",
                       init_cmd->width, init_cmd->height, init_cmd->color_depth);

                int result = init_display_callback(init_cmd->width, init_cmd->height, init_cmd->color_depth,
                                                  init_cmd->margin_x, init_cmd->margin_y);

                if (result == 0 && (type & FMRB_LINK_FLAG_ACK_REQUIRED)) {
                    comm->send_ack(type, seq, NULL, 0);
                }
                return result;
            }
            break;

        default:
            ESP_LOGE(TAG, "Unknown control command: 0x%02x", sub_cmd);
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
        ESP_LOGE(TAG, "No comm interface available");
        return -1;
    }

    // Pass to graphics handler
    int result = graphics_handler_process_command(type, sub_cmd, seq, payload, payload_len);

    // Only send per-message ACK when ACK_REQUIRED flag is set (e.g. CREATE_CANVAS).
    // Batch drawing commands are sent without this flag; their frame-level
    // ACK (STS_APP_OK) is handled by comm_spi_slave.
    if (type & FMRB_LINK_FLAG_ACK_REQUIRED) {
        if (result == 0) {
            comm->send_ack(type, seq, NULL, 0);
        }
        // result > 0: ACK already sent by handler (e.g. CREATE_CANVAS with canvas_id)
    }

    return (result >= 0) ? 0 : result;
}

/**
 * Handle FILE_TRANSFER messages
 */
static int handle_file_transfer_message(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                                        const uint8_t *payload, size_t payload_len) {
    const comm_interface_t *comm = comm_get_interface();
    if (!comm) {
        ESP_LOGE(TAG, "No comm interface available");
        return -1;
    }

    int result = file_transfer_handler_process(type, sub_cmd, seq, payload, payload_len);

    if (type & FMRB_LINK_FLAG_ACK_REQUIRED) {
        if (result == 0) {
            comm->send_ack(type, seq, NULL, 0);
        }
        // result > 0: ACK already sent by handler (e.g. STATUS with response data)
    }

    return (result >= 0) ? 0 : result;
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
 * Process a received message
 */
static int process_message(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                           const uint8_t *payload, size_t payload_len) {
    // Strip flag bits (ACK_REQUIRED=0x20, CHUNKED=0x40) for type matching
    uint8_t base_type = type & ~(FMRB_LINK_FLAG_ACK_REQUIRED | FMRB_LINK_FLAG_CHUNKED);

    switch (base_type) {
        case FMRB_LINK_TYPE_CONTROL:
            return handle_control_message(type, seq, sub_cmd, payload, payload_len);

        case FMRB_LINK_TYPE_GRAPHICS:
            return handle_graphics_message(type, seq, sub_cmd, payload, payload_len);

        case FMRB_LINK_TYPE_AUDIO:
            return handle_audio_message(type, seq, sub_cmd, payload, payload_len);

        case FMRB_LINK_TYPE_FILE_TRANSFER:
            return handle_file_transfer_message(type, seq, sub_cmd, payload, payload_len);

        default:
            ESP_LOGE(TAG, "Unknown message type: %u", type);
            return -1;
    }
}

void message_handler_task(void *pvParameters) {
    MessageBufferHandle_t msg_buffer = (MessageBufferHandle_t)pvParameters;
    
    if (!msg_buffer) {
        ESP_LOGE(TAG, "Invalid MessageBuffer handle");
        vTaskDelete(NULL);
        return;
    }
    
    task_running = 1;
    ESP_LOGI(TAG, "Message handler task started on core %d", (int)xPortGetCoreID());
    
    message_data_t msg;
    size_t bytes_recv;
    
    uint32_t consecutive_msgs = 0;

    while (task_running) {
        // Wait for a message from the comm_task
        // pdMS_TO_TICKS(100) = 100ms timeout to allow task_running check
        bytes_recv = xMessageBufferReceive(msg_buffer, &msg, sizeof(message_data_t), pdMS_TO_TICKS(1000));

        if (bytes_recv > 0) {
            // Process the message
            ESP_LOGD(TAG, "Received message: type=%u seq=%u sub_cmd=0x%02x len=%u",
                   msg.type, msg.seq, msg.sub_cmd, msg.payload_len);

            int result = process_message(msg.type, msg.seq, msg.sub_cmd,
                                        msg.payload, msg.payload_len);

            if (result < 0) {
                ESP_LOGW(TAG, "Handler failed: type=%u seq=%u sub_cmd=0x%02x (result=%d)",
                       msg.type, msg.seq, msg.sub_cmd, result);
            }

            // Yield periodically to prevent WDT when processing burst traffic
            consecutive_msgs++;
            if (consecutive_msgs >= 16) {
                consecutive_msgs = 0;
                vTaskDelay(1);
            }
        } else {
            consecutive_msgs = 0;
        }
        // If timeout (bytes_recv == 0), loop continues to check task_running
    }
    
    ESP_LOGI(TAG, "Message handler task stopped");
    vTaskDelete(NULL);
}

void message_handler_task_stop(void) {
    task_running = 0;
}
