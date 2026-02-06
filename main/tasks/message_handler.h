#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Process a received message
 * @param type Message type
 * @param seq Sequence number
 * @param sub_cmd Sub-command
 * @param payload Payload data
 * @param payload_len Payload length
 * @return 0 on success, -1 on error
 */
int message_handler_process(uint8_t type, uint8_t seq, uint8_t sub_cmd,
                            const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
