#ifndef FMRB_LINK_MSGPACK_H
#define FMRB_LINK_MSGPACK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a COBS+CRC32+msgpack frame
 * @param encoded_data COBS encoded frame data (without 0x00 terminator)
 * @param encoded_len Length of encoded data
 * @param type Output: message type
 * @param seq Output: sequence number
 * @param sub_cmd Output: sub-command
 * @param payload_out Output: payload buffer (caller must provide buffer)
 * @param payload_out_size Size of payload_out buffer
 * @param payload_len Output: actual payload length
 * @return 0 on success, -1 on error
 */
int fmrb_link_decode_frame(const uint8_t *encoded_data, size_t encoded_len,
                           uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
                           uint8_t *payload_out, size_t payload_out_size,
                           size_t *payload_len);

/**
 * Encode ACK response as msgpack+CRC32+COBS
 * @param type Message type
 * @param seq Sequence number
 * @param response_data Optional response payload
 * @param response_len Length of response payload
 * @param encoded_out Output buffer for encoded data (with 0x00 terminator)
 * @param encoded_out_size Size of output buffer
 * @param encoded_len Output: actual encoded length (including 0x00)
 * @return 0 on success, -1 on error
 */
int fmrb_link_encode_ack(uint8_t type, uint8_t seq,
                         const uint8_t *response_data, uint16_t response_len,
                         uint8_t *encoded_out, size_t encoded_out_size,
                         size_t *encoded_len);

#ifdef __cplusplus
}
#endif

#endif // FMRB_LINK_MSGPACK_H
