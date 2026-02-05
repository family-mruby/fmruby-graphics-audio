#include "socket_server.h"
#include "fmrb_link_msgpack.h"
#include "message_queue.h"
#include "fmrb_link_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"

static const char *TAG = "socket_server";

// Message queue for decoded messages
static message_queue_t g_message_queue;

static int server_fd = -1;
static int client_fd = -1;
static int server_running = 0;

#define SOCKET_PATH "/tmp/fmrb_socket"
#define BUFFER_SIZE 4096

static int create_socket_server(void) {
    struct sockaddr_un addr;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        ESP_LOGE(TAG, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    // Remove existing socket file
    unlink(SOCKET_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        ESP_LOGE(TAG, "Failed to bind socket: %s", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    if (listen(server_fd, 1) == -1) {
        ESP_LOGE(TAG, "Failed to listen on socket: %s", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    // Set non-blocking mode
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "Socket server listening on %s", SOCKET_PATH);
    return 0;
}

static int accept_connection(void) {
    if (client_fd != -1) {
        return 0; // Already connected
    }

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Failed to accept connection: %s", strerror(errno));
        }
        return -1;
    }

    // Set non-blocking mode for client socket
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "Client connected");
    return 0;
}

static int process_cobs_frame(const uint8_t *encoded_data, size_t encoded_len) {
    uint8_t type, seq, sub_cmd;
    uint8_t payload_buffer[MSG_QUEUE_MAX_PAYLOAD];
    size_t payload_len;

    // Decode frame using common msgpack module
    int result = fmrb_link_decode_frame(encoded_data, encoded_len,
                                       &type, &seq, &sub_cmd,
                                       payload_buffer, sizeof(payload_buffer),
                                       &payload_len);
    if (result != 0) {
        ESP_LOGE(TAG, "Frame decode failed");
        return -1;
    }

    ESP_LOGD(TAG, "RX msgpack: type=%d seq=%d sub_cmd=0x%02x payload_len=%zu",
               type, seq, sub_cmd, payload_len);

    // Enqueue the decoded message using common queue module
    result = message_queue_enqueue(&g_message_queue, type, seq, sub_cmd,
                                   payload_buffer, payload_len);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to enqueue message");
        return -1;
    }

    return 0;
}

// Legacy process_message() removed - now using msgpack + COBS protocol via process_cobs_frame()

static int read_message(void) {
    static uint8_t buffer[BUFFER_SIZE];
    static size_t buffer_pos = 0;

    // Read data into buffer
    ssize_t bytes_read = read(client_fd, buffer + buffer_pos, BUFFER_SIZE - buffer_pos);
    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            ESP_LOGI(TAG, "Client disconnected");
            close(client_fd);
            client_fd = -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Read error: %s", strerror(errno));
            close(client_fd);
            client_fd = -1;
        }
        return -1;
    }

    buffer_pos += bytes_read;

    // Process complete COBS frames (terminated by 0x00)
    int messages_processed = 0;
    size_t scan_pos = 0;

    while (scan_pos < buffer_pos) {
        // Look for frame terminator (0x00)
        size_t frame_end = scan_pos;
        while (frame_end < buffer_pos && buffer[frame_end] != 0x00) {
            frame_end++;
        }

        if (frame_end >= buffer_pos) {
            // No complete frame yet
            break;
        }

        // Found a complete frame: [scan_pos .. frame_end-1] + 0x00 at frame_end
        size_t frame_len = frame_end - scan_pos;

        if (frame_len > 0) {
            // Process COBS frame (without the 0x00 terminator)
            if (process_cobs_frame(buffer + scan_pos, frame_len) == 0) {
                messages_processed++;
            }
        }

        // Move to next frame (skip the 0x00 terminator)
        scan_pos = frame_end + 1;
    }

    // Remove processed data from buffer
    if (scan_pos > 0) {
        size_t remaining = buffer_pos - scan_pos;
        if (remaining > 0) {
            memmove(buffer, buffer + scan_pos, remaining);
        }
        buffer_pos = remaining;
    }

    // Check for buffer overflow
    if (buffer_pos >= BUFFER_SIZE - 1) {
        ESP_LOGE(TAG, "Buffer overflow, resetting");
        buffer_pos = 0;
    }

    return messages_processed;
}

// Send ACK response with optional payload
int socket_server_send_ack(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len) {
    if (client_fd == -1) {
        ESP_LOGE(TAG, "Cannot send ACK: no client connected");
        return -1;
    }

    uint8_t encoded_buffer[BUFFER_SIZE];
    size_t encoded_len;

    // Encode ACK using common msgpack module
    int result = fmrb_link_encode_ack(type, seq, response_data, response_len,
                                     encoded_buffer, sizeof(encoded_buffer),
                                     &encoded_len);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to encode ACK");
        return -1;
    }

    // Send to client
    ssize_t written = write(client_fd, encoded_buffer, encoded_len);
    if (written != (ssize_t)encoded_len) {
        ESP_LOGE(TAG, "Failed to write ACK response: %zd/%zu (client_fd=%d, errno=%d: %s)",
                   written, encoded_len, client_fd, errno, strerror(errno));
        return -1;
    }

    ESP_LOGD(TAG, "ACK sent: type=%u seq=%u response_len=%u", type, seq, response_len);
    return 0;
}

int socket_server_start(void) {
    if (server_running) {
        return 0;
    }

    if (create_socket_server() != 0) {
        return -1;
    }

    server_running = 1;
    return 0;
}

void socket_server_stop(void) {
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
    ESP_LOGI(TAG, "Socket server stopped");
}

int socket_server_process(void) {
    if (!server_running) {
        return 0;
    }

    // Try to accept new connections
    accept_connection();

    // Process messages from connected client
    if (client_fd != -1) {
        return read_message();
    }

    return 0;
}

int socket_server_is_running(void) {
    return server_running;
}

// comm_interface implementation for Linux/socket
#include "comm_interface.h"

static int comm_socket_init(void) {
    // Initialize message queue
    message_queue_init(&g_message_queue);
    return socket_server_start();
}

static int comm_socket_process(void) {
    return socket_server_process();
}

static int comm_socket_send(const uint8_t *data, size_t len) {
    // Socket server is receive-only in current implementation
    (void)data;
    (void)len;
    return 0;
}

static int comm_socket_receive(uint8_t *buf, size_t buf_size) {
    // Legacy method - not used in new architecture
    (void)buf;
    (void)buf_size;
    return 0;
}

static int comm_socket_receive_message(uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
                                        const uint8_t **payload, size_t *payload_len) {
    // Dequeue message using common queue module
    int result = message_queue_dequeue(&g_message_queue, type, seq, sub_cmd,
                                       payload, payload_len);

    if (result > 0) {
        ESP_LOGD(TAG, "Dequeued message: type=%u seq=%u sub_cmd=0x%02x len=%zu (queue=%d/%d)",
                   *type, *seq, *sub_cmd, *payload_len,
                   message_queue_count(&g_message_queue), MSG_QUEUE_MAX_MESSAGES);
    }

    return result;
}

static void comm_socket_cleanup(void) {
    socket_server_stop();
    // Clear message queue
    message_queue_init(&g_message_queue);
}

static const comm_interface_t socket_comm_impl = {
    .init = comm_socket_init,
    .send = comm_socket_send,
    .receive = comm_socket_receive,
    .process = comm_socket_process,
    .receive_message = comm_socket_receive_message,
    .send_ack = socket_server_send_ack,
    .is_running = socket_server_is_running,
    .cleanup = comm_socket_cleanup
};

const comm_interface_t* comm_get_interface(void) {
    return &socket_comm_impl;
}