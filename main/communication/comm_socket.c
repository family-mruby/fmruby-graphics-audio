#include "comm_interface.h"

#ifdef CONFIG_IDF_TARGET_LINUX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <msgpack.h>

// Forward declarations from host/common
extern size_t fmrb_link_cobs_encode(const uint8_t *input, size_t input_len, uint8_t *output);
extern ssize_t fmrb_link_cobs_decode(const uint8_t *input, size_t input_len, uint8_t *output);
extern uint32_t fmrb_link_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

// Forward declarations for handlers
extern int graphics_handler_process_command(uint8_t type, uint8_t sub_cmd, uint8_t seq,
                                            const uint8_t *payload, size_t payload_len);
extern int audio_handler_process_command(const uint8_t *cmd_buffer, size_t cmd_len);
extern int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth);

// Protocol definitions (from fmrb_link_protocol.h)
#define FMRB_LINK_PROTOCOL_VERSION 1
#define FMRB_LINK_TYPE_CONTROL    0x01
#define FMRB_LINK_TYPE_GRAPHICS   0x02
#define FMRB_LINK_TYPE_AUDIO      0x03
#define FMRB_LINK_CONTROL_VERSION       0x01
#define FMRB_LINK_CONTROL_INIT_DISPLAY  0x02

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t color_depth;
} __attribute__((packed)) fmrb_control_init_display_t;

// Socket server state
static int server_fd = -1;
static int client_fd = -1;
static int server_running = 0;

#define SOCKET_PATH "/tmp/fmrb_socket"
#define BUFFER_SIZE 4096

// Log levels
typedef enum {
    SOCK_LOG_NONE = 0,
    SOCK_LOG_ERROR = 1,
    SOCK_LOG_INFO = 2,
    SOCK_LOG_DEBUG = 3,
} sock_log_level_t;

static sock_log_level_t g_sock_log_level = SOCK_LOG_ERROR;

#define SOCK_LOG_E(fmt, ...) do { if (g_sock_log_level >= SOCK_LOG_ERROR) { fprintf(stderr, "[SOCK_ERR] " fmt "\n", ##__VA_ARGS__); } } while(0)
#define SOCK_LOG_I(fmt, ...) do { if (g_sock_log_level >= SOCK_LOG_INFO) { printf("[SOCK_INFO] " fmt "\n", ##__VA_ARGS__); } } while(0)
#define SOCK_LOG_D(fmt, ...) do { if (g_sock_log_level >= SOCK_LOG_DEBUG) { printf("[SOCK_DBG] " fmt "\n", ##__VA_ARGS__); } } while(0)

static int create_socket_server(void) {
    struct sockaddr_un addr;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    // Remove existing socket file
    unlink(SOCKET_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "Failed to bind socket: %s\n", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    if (listen(server_fd, 1) == -1) {
        fprintf(stderr, "Failed to listen on socket: %s\n", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    // Set non-blocking mode
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Socket server listening on %s\n", SOCKET_PATH);
    return 0;
}

static int accept_connection(void) {
    if (client_fd != -1) {
        return 0; // Already connected
    }

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "Failed to accept connection: %s\n", strerror(errno));
        }
        return -1;
    }

    // Set non-blocking mode for client socket
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Client connected\n");
    return 0;
}

static int socket_send_ack(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len) {
    if (client_fd == -1) {
        fprintf(stderr, "Cannot send ACK: no client connected\n");
        return -1;
    }

    // Build msgpack response: [type, seq, sub_cmd=0xF0 (ACK), payload]
    msgpack_sbuffer sbuf;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer pk;
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    msgpack_pack_array(&pk, 4);
    msgpack_pack_uint8(&pk, type);
    msgpack_pack_uint8(&pk, seq);
    msgpack_pack_uint8(&pk, 0xF0);  // ACK sub-command

    if (response_data && response_len > 0) {
        msgpack_pack_bin(&pk, response_len);
        msgpack_pack_bin_body(&pk, response_data, response_len);
    } else {
        msgpack_pack_nil(&pk);
    }

    // Add CRC32
    uint32_t crc = fmrb_link_crc32_update(0, (const uint8_t*)sbuf.data, sbuf.size);
    size_t msg_with_crc_len = sbuf.size + sizeof(uint32_t);
    uint8_t *msg_with_crc = (uint8_t*)malloc(msg_with_crc_len);
    if (!msg_with_crc) {
        msgpack_sbuffer_destroy(&sbuf);
        return -1;
    }

    memcpy(msg_with_crc, sbuf.data, sbuf.size);
    memcpy(msg_with_crc + sbuf.size, &crc, sizeof(uint32_t));
    msgpack_sbuffer_destroy(&sbuf);

    // COBS encode
    uint8_t encoded_buffer[BUFFER_SIZE];
    size_t encoded_len = fmrb_link_cobs_encode(msg_with_crc, msg_with_crc_len, encoded_buffer);
    free(msg_with_crc);

    if (encoded_len == 0) {
        return -1;
    }

    // Add terminator
    encoded_buffer[encoded_len] = 0x00;
    encoded_len++;

    // Send
    ssize_t written = write(client_fd, encoded_buffer, encoded_len);
    if (written != (ssize_t)encoded_len) {
        fprintf(stderr, "Failed to write ACK response\n");
        return -1;
    }

    SOCK_LOG_D("ACK sent: type=%u seq=%u response_len=%u", type, seq, response_len);
    return 0;
}

static int process_cobs_frame(const uint8_t *encoded_data, size_t encoded_len) {
    uint8_t *decoded_buffer = (uint8_t*)malloc(encoded_len);
    if (!decoded_buffer) {
        return -1;
    }

    // COBS decode
    ssize_t decoded_len = fmrb_link_cobs_decode(encoded_data, encoded_len, decoded_buffer);
    if (decoded_len < (ssize_t)sizeof(uint32_t)) {
        free(decoded_buffer);
        return -1;
    }

    // Separate msgpack and CRC32
    size_t msgpack_len = decoded_len - sizeof(uint32_t);
    uint8_t *msgpack_data = decoded_buffer;
    uint32_t received_crc;
    memcpy(&received_crc, decoded_buffer + msgpack_len, sizeof(uint32_t));

    // Verify CRC32
    uint32_t calculated_crc = fmrb_link_crc32_update(0, msgpack_data, msgpack_len);
    if (received_crc != calculated_crc) {
        fprintf(stderr, "CRC32 mismatch\n");
        free(decoded_buffer);
        return -1;
    }

    // Unpack msgpack: [type, seq, sub_cmd, payload]
    msgpack_unpacked msg;
    msgpack_unpacked_init(&msg);
    msgpack_unpack_return ret = msgpack_unpack_next(&msg, (const char*)msgpack_data, msgpack_len, NULL);

    if (ret != MSGPACK_UNPACK_SUCCESS) {
        msgpack_unpacked_destroy(&msg);
        free(decoded_buffer);
        return -1;
    }

    msgpack_object root = msg.data;
    if (root.type != MSGPACK_OBJECT_ARRAY || root.via.array.size != 4) {
        msgpack_unpacked_destroy(&msg);
        free(decoded_buffer);
        return -1;
    }

    // Extract fields
    uint8_t type = root.via.array.ptr[0].via.u64;
    uint8_t seq = root.via.array.ptr[1].via.u64;
    uint8_t sub_cmd = root.via.array.ptr[2].via.u64;

    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (root.via.array.ptr[3].type == MSGPACK_OBJECT_BIN) {
        payload = (const uint8_t*)root.via.array.ptr[3].via.bin.ptr;
        payload_len = root.via.array.ptr[3].via.bin.size;
    }

    // Process based on type
    int result = 0;
    switch (type & 0x7F) {
        case FMRB_LINK_TYPE_CONTROL:
            if (sub_cmd == FMRB_LINK_CONTROL_VERSION && payload_len >= 1) {
                uint8_t remote_version = payload[0];
                uint8_t local_version = FMRB_LINK_PROTOCOL_VERSION;
                printf("VERSION check: remote=%d, local=%d\n", remote_version, local_version);
                result = socket_send_ack(type, seq, &local_version, sizeof(local_version));
            } else if (sub_cmd == FMRB_LINK_CONTROL_INIT_DISPLAY && payload_len >= sizeof(fmrb_control_init_display_t)) {
                const fmrb_control_init_display_t *init_cmd = (const fmrb_control_init_display_t*)payload;
                printf("INIT_DISPLAY: %dx%d, %d-bit\n", init_cmd->width, init_cmd->height, init_cmd->color_depth);
                result = init_display_callback(init_cmd->width, init_cmd->height, init_cmd->color_depth);
                if (result == 0) {
                    socket_send_ack(type, seq, NULL, 0);
                }
            }
            break;

        case FMRB_LINK_TYPE_GRAPHICS:
            result = graphics_handler_process_command(type, sub_cmd, seq, payload, payload_len);
            if (result == 0) {
                socket_send_ack(type, seq, NULL, 0);
            }
            break;

        case FMRB_LINK_TYPE_AUDIO:
            result = audio_handler_process_command(payload, payload_len);
            break;

        default:
            fprintf(stderr, "Unknown frame type: %u\n", type);
            result = -1;
            break;
    }

    msgpack_unpacked_destroy(&msg);
    free(decoded_buffer);
    return result;
}

static int read_message(void) {
    static uint8_t buffer[BUFFER_SIZE];
    static size_t buffer_pos = 0;

    ssize_t bytes_read = read(client_fd, buffer + buffer_pos, BUFFER_SIZE - buffer_pos);
    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            printf("Client disconnected\n");
            close(client_fd);
            client_fd = -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "Read error: %s\n", strerror(errno));
            close(client_fd);
            client_fd = -1;
        }
        return -1;
    }

    buffer_pos += bytes_read;

    // Process complete COBS frames
    int messages_processed = 0;
    size_t scan_pos = 0;

    while (scan_pos < buffer_pos) {
        size_t frame_end = scan_pos;
        while (frame_end < buffer_pos && buffer[frame_end] != 0x00) {
            frame_end++;
        }

        if (frame_end >= buffer_pos) {
            break;
        }

        size_t frame_len = frame_end - scan_pos;
        if (frame_len > 0) {
            if (process_cobs_frame(buffer + scan_pos, frame_len) == 0) {
                messages_processed++;
            }
        }

        scan_pos = frame_end + 1;
    }

    // Remove processed data
    if (scan_pos > 0) {
        size_t remaining = buffer_pos - scan_pos;
        if (remaining > 0) {
            memmove(buffer, buffer + scan_pos, remaining);
        }
        buffer_pos = remaining;
    }

    if (buffer_pos >= BUFFER_SIZE - 1) {
        fprintf(stderr, "Buffer overflow, resetting\n");
        buffer_pos = 0;
    }

    return messages_processed;
}

// Implementation of comm_interface_t methods

static int socket_init(void) {
    if (server_running) {
        return 0;
    }

    if (create_socket_server() != 0) {
        return -1;
    }

    server_running = 1;
    return 0;
}

static int socket_send(const uint8_t *data, size_t len) {
    if (client_fd == -1) {
        return -1;
    }
    return write(client_fd, data, len);
}

static int socket_receive(uint8_t *buf, size_t buf_size) {
    if (client_fd == -1) {
        return 0;
    }
    return read(client_fd, buf, buf_size);
}

static int socket_process(void) {
    if (!server_running) {
        return 0;
    }

    accept_connection();

    if (client_fd != -1) {
        return read_message();
    }

    return 0;
}

static int socket_is_running(void) {
    return server_running;
}

static void socket_cleanup(void) {
    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }

    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
        unlink(SOCKET_PATH);
    }

    server_running = 0;
    printf("Socket server stopped\n");
}

static const comm_interface_t socket_comm = {
    .init = socket_init,
    .send = socket_send,
    .receive = socket_receive,
    .process = socket_process,
    .send_ack = socket_send_ack,
    .is_running = socket_is_running,
    .cleanup = socket_cleanup,
};

const comm_interface_t* comm_get_interface(void) {
    return &socket_comm;
}

#endif // CONFIG_IDF_TARGET_LINUX
