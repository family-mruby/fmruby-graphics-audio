/**
 * @file input_handler_ipc.c
 * @brief Input handler receiving HID events from SDL2 display process via Unix socket.
 *        Replaces SDL2-based input_handler.c for headless Linux builds.
 */
#include "input_handler.h"
#include "input_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include "esp_log.h"
#include "shm_display.h"
#include "fmrb_hid_event.h"

static const char *TAG = "input_handler_ipc";

static bool g_initialized = false;
static int g_server_fd = -1;
static int g_client_fd = -1;
static int g_last_mouse_x = 0;
static int g_last_mouse_y = 0;
static volatile bool g_quit_requested = false;

int input_handler_init(void) {
    if (g_initialized) {
        return 0;
    }

    /* Remove existing socket file */
    unlink(FMRB_INPUT_SOCKET_PATH);

    /* Create server socket */
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FMRB_INPUT_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    if (listen(g_server_fd, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        unlink(FMRB_INPUT_SOCKET_PATH);
        return -1;
    }

    /* Set non-blocking */
    int flags = fcntl(g_server_fd, F_GETFL, 0);
    fcntl(g_server_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "Input handler (IPC) initialized on %s", FMRB_INPUT_SOCKET_PATH);
    g_initialized = true;
    return 0;
}

/* Try to accept a client connection (non-blocking) */
static void try_accept(void) {
    if (g_client_fd >= 0 || g_server_fd < 0) return;

    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    g_client_fd = accept(g_server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (g_client_fd >= 0) {
        /* Set client non-blocking */
        int flags = fcntl(g_client_fd, F_GETFL, 0);
        fcntl(g_client_fd, F_SETFL, flags | O_NONBLOCK);
        ESP_LOGI(TAG, "SDL2 display process connected");
    }
}

/* Read and process one HID event from the client socket */
static int process_one_event(void) {
    if (g_client_fd < 0) return 0;

    /* Read packet header: [type(1)][len(2)] */
    uint8_t header[3];
    ssize_t n = recv(g_client_fd, header, 3, MSG_PEEK);
    if (n == 0) {
        /* Client disconnected */
        ESP_LOGI(TAG, "SDL2 display process disconnected");
        close(g_client_fd);
        g_client_fd = -1;
        return 0;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; /* No data available */
        }
        if (errno == EINTR) return 0;
        ESP_LOGE(TAG, "recv error: %s", strerror(errno));
        close(g_client_fd);
        g_client_fd = -1;
        return -1;
    }
    if (n < 3) return 0; /* Partial header, wait for more */

    uint8_t type = header[0];
    uint16_t data_len = header[1] | (header[2] << 8);

    /* Check if full packet is available */
    uint8_t buf[256];
    uint16_t total = 3 + data_len;
    if (total > sizeof(buf)) {
        /* Packet too large, discard */
        ESP_LOGE(TAG, "Packet too large: %u bytes", total);
        recv(g_client_fd, buf, sizeof(buf), 0);
        return 0;
    }

    n = recv(g_client_fd, buf, total, MSG_PEEK);
    if (n < total) return 0; /* Wait for full packet */

    /* Consume the packet */
    recv(g_client_fd, buf, total, 0);

    const uint8_t *data = buf + 3;

    /* Process based on event type */
    switch (type) {
        case HID_EVENT_MOUSE_MOTION:
            if (data_len >= sizeof(hid_mouse_motion_event_t)) {
                const hid_mouse_motion_event_t *evt = (const hid_mouse_motion_event_t*)data;
                g_last_mouse_x = evt->x;
                g_last_mouse_y = evt->y;
            }
            /* Forward to fmruby-core */
            input_socket_send_event(type, data, data_len);
            break;

        case HID_EVENT_MOUSE_BUTTON:
        case HID_EVENT_MOUSE_WHEEL:
        case HID_EVENT_KEY_DOWN:
        case HID_EVENT_KEY_UP:
            /* Forward to fmruby-core */
            input_socket_send_event(type, data, data_len);
            break;

        case FMRB_CTRL_SHUTDOWN:
            g_quit_requested = true;
            break;

        default:
            ESP_LOGW(TAG, "Unknown event type: 0x%02x", type);
            break;
    }

    return 1; /* Processed one event */
}

int input_handler_process_events(void) {
    if (!g_initialized) return -1;

    if (g_quit_requested) return 1;

    try_accept();

    /* Process all available events */
    while (process_one_event() > 0) {
        /* Continue processing */
    }

    return 0;
}

void input_handler_cleanup(void) {
    if (!g_initialized) return;

    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
        unlink(FMRB_INPUT_SOCKET_PATH);
    }

    g_initialized = false;
    ESP_LOGI(TAG, "Input handler (IPC) cleaned up");
}

int input_handler_get_mouse_position(int* x, int* y) {
    if (!g_initialized || !x || !y) return -1;
    *x = g_last_mouse_x;
    *y = g_last_mouse_y;
    return 0;
}
