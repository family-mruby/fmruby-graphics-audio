#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize file transfer handler (mount LittleFS if needed)
int file_transfer_handler_init(void);

// Process a file transfer command
// Returns: 0=success (caller sends ACK), >0=success (ACK sent by handler), <0=error
int file_transfer_handler_process(uint8_t type, uint8_t sub_cmd, uint8_t seq,
                                  const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
