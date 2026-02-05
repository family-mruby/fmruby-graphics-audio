#include "fmrb_link_msgpack.h"
#include "fmrb_link_cobs.h"
#include "fmrb_link_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <msgpack.h>

int fmrb_link_decode_frame(const uint8_t *encoded_data, size_t encoded_len,
                           uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
                           uint8_t *payload_out, size_t payload_out_size,
                           size_t *payload_len) {
    if (!encoded_data || !type || !seq || !sub_cmd || !payload_len) {
        return -1;
    }

    // Allocate buffer for decoded data (COBS + CRC32)
    uint8_t *decoded_buffer = (uint8_t*)malloc(encoded_len);
    if (!decoded_buffer) {
        fprintf(stderr, "[MSGPACK] Failed to allocate decode buffer\n");
        return -1;
    }

    // COBS decode
    ssize_t decoded_len = fmrb_link_cobs_decode(encoded_data, encoded_len, decoded_buffer);
    if (decoded_len < (ssize_t)sizeof(uint32_t)) {
        fprintf(stderr, "[MSGPACK] COBS decode failed or frame too small\n");
        free(decoded_buffer);
        return -1;
    }

    // Separate msgpack data and CRC32
    size_t msgpack_len = decoded_len - sizeof(uint32_t);
    uint8_t *msgpack_data = decoded_buffer;
    uint32_t received_crc;
    memcpy(&received_crc, decoded_buffer + msgpack_len, sizeof(uint32_t));

    // Verify CRC32
    uint32_t calculated_crc = fmrb_link_crc32_update(0, msgpack_data, msgpack_len);
    if (received_crc != calculated_crc) {
        fprintf(stderr, "[MSGPACK] CRC32 mismatch: expected=0x%08x, actual=0x%08x\n",
                calculated_crc, received_crc);
        free(decoded_buffer);
        return -1;
    }

    // Unpack msgpack array: [type, seq, sub_cmd, payload]
    msgpack_unpacked msg;
    msgpack_unpacked_init(&msg);
    msgpack_unpack_return ret = msgpack_unpack_next(&msg, (const char*)msgpack_data, msgpack_len, NULL);

    if (ret != MSGPACK_UNPACK_SUCCESS) {
        fprintf(stderr, "[MSGPACK] msgpack unpack failed\n");
        msgpack_unpacked_destroy(&msg);
        free(decoded_buffer);
        return -1;
    }

    msgpack_object root = msg.data;
    if (root.type != MSGPACK_OBJECT_ARRAY || root.via.array.size != 4) {
        fprintf(stderr, "[MSGPACK] Invalid msgpack format: not array or size != 4\n");
        msgpack_unpacked_destroy(&msg);
        free(decoded_buffer);
        return -1;
    }

    // Extract fields
    *type = root.via.array.ptr[0].via.u64;
    *seq = root.via.array.ptr[1].via.u64;
    *sub_cmd = root.via.array.ptr[2].via.u64;

    // Extract payload
    *payload_len = 0;
    if (root.via.array.ptr[3].type == MSGPACK_OBJECT_BIN) {
        const uint8_t *payload_src = (const uint8_t*)root.via.array.ptr[3].via.bin.ptr;
        size_t src_len = root.via.array.ptr[3].via.bin.size;

        if (payload_out && src_len > 0) {
            if (src_len > payload_out_size) {
                fprintf(stderr, "[MSGPACK] Payload too large: %zu > %zu\n", src_len, payload_out_size);
                msgpack_unpacked_destroy(&msg);
                free(decoded_buffer);
                return -1;
            }
            memcpy(payload_out, payload_src, src_len);
            *payload_len = src_len;
        }
    }

    msgpack_unpacked_destroy(&msg);
    free(decoded_buffer);
    return 0;
}

int fmrb_link_encode_ack(uint8_t type, uint8_t seq,
                         const uint8_t *response_data, uint16_t response_len,
                         uint8_t *encoded_out, size_t encoded_out_size,
                         size_t *encoded_len) {
    if (!encoded_out || !encoded_len) {
        return -1;
    }

    // Build msgpack response: [type, seq, sub_cmd=0xF0 (ACK), payload]
    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    // Pack as array: [type, seq, 0xF0 (ACK), response_data]
    msgpack_pack_array(&pk, 4);
    msgpack_pack_uint8(&pk, type);
    msgpack_pack_uint8(&pk, seq);
    msgpack_pack_uint8(&pk, 0xF0);  // ACK sub-command

    // Pack response data as binary
    if (response_data && response_len > 0) {
        msgpack_pack_bin(&pk, response_len);
        msgpack_pack_bin_body(&pk, response_data, response_len);
    } else {
        msgpack_pack_nil(&pk);
    }

    // Add CRC32 to msgpack message
    uint32_t crc = fmrb_link_crc32_update(0, (const uint8_t*)sbuf.data, sbuf.size);
    size_t msg_with_crc_len = sbuf.size + sizeof(uint32_t);
    uint8_t *msg_with_crc = (uint8_t*)malloc(msg_with_crc_len);
    if (!msg_with_crc) {
        msgpack_sbuffer_destroy(&sbuf);
        fprintf(stderr, "[MSGPACK] Failed to allocate buffer for CRC\n");
        return -1;
    }

    memcpy(msg_with_crc, sbuf.data, sbuf.size);
    memcpy(msg_with_crc + sbuf.size, &crc, sizeof(uint32_t));
    msgpack_sbuffer_destroy(&sbuf);

    // COBS encode the msgpack + CRC32
    size_t cobs_len = fmrb_link_cobs_encode(msg_with_crc, msg_with_crc_len, encoded_out);
    free(msg_with_crc);

    if (cobs_len == 0 || cobs_len >= encoded_out_size) {
        fprintf(stderr, "[MSGPACK] COBS encode failed or buffer too small\n");
        return -1;
    }

    // Add 0x00 terminator
    encoded_out[cobs_len] = 0x00;
    *encoded_len = cobs_len + 1;

    return 0;
}
