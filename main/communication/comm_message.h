#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum payload size for communication messages.
 * Matches the SPI frame size (256 bytes) on ESP32.
 */
#define COMM_MSG_MAX_PAYLOAD 256

/**
 * Message structure exchanged via FreeRTOS MessageBuffer
 * between comm layer (producer) and message_handler_task (consumer).
 */
typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t sub_cmd;
    uint16_t payload_len;
    uint8_t payload[COMM_MSG_MAX_PAYLOAD];
} message_data_t;

#ifdef __cplusplus
}
#endif
