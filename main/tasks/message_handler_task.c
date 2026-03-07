#include "message_handler_task.h"
#include "fmrb_link_protocol.h"
#include "graphics_handler.h"
#include "audio_handler.h"
#include "comm_interface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include <string.h>
#include <stddef.h>

static const char *TAG = "msg_handler_task";
static volatile int task_running = 0;

// Forward declaration - implemented in main.cpp
extern int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth);

// Message structure exchanged between comm_task and message_handler_task
typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t sub_cmd;
    uint16_t payload_len;
    uint8_t payload[1024];  // MSG_QUEUE_MAX_PAYLOAD
} message_data_t;

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

                // Send version response via ACK
                int result = comm->send_ack(type, seq, &local_version, sizeof(local_version));

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

                int result = init_display_callback(init_cmd->width, init_cmd->height, init_cmd->color_depth);

                // Send ACK to prevent retransmission
                if (result == 0) {
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
    // Return convention: 0=success (ACK not sent), >0=success (ACK already sent), <0=error
    int result = graphics_handler_process_command(type, sub_cmd, seq, payload, payload_len);

    if (result == 0) {
        // Command succeeded, ACK not yet sent
        comm->send_ack(type, seq, NULL, 0);
    }
    // result > 0: ACK already sent by handler (e.g. CREATE_CANVAS with canvas_id)

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
    
    while (task_running) {
        // Wait for a message from the comm_task
        // pdMS_TO_TICKS(100) = 100ms timeout to allow task_running check
        bytes_recv = xMessageBufferReceive(msg_buffer, &msg, sizeof(message_data_t), pdMS_TO_TICKS(100));
        
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
        }
        // If timeout (bytes_recv == 0), loop continues to check task_running
    }
    
    ESP_LOGI(TAG, "Message handler task stopped");
    vTaskDelete(NULL);
}

void message_handler_task_stop(void) {
    task_running = 0;
}
