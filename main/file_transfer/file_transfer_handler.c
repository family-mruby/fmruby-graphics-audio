#include "file_transfer_handler.h"
#include "fmrb_link_protocol.h"
#include "comm_interface.h"
#include "esp_log.h"

#if !defined(CONFIG_IDF_TARGET_LINUX)
#include "esp_littlefs.h"
#endif

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "file_transfer";

// Base path for file storage
// ESP32: absolute VFS mount point "/flash"
// Linux: relative to CWD (/project in Docker) -> "flash" resolves to /project/flash/
#if defined(CONFIG_IDF_TARGET_LINUX)
#define FILE_TRANSFER_BASE_PATH "flash"
#else
#define FILE_TRANSFER_BASE_PATH "/flash"
#endif
#define FILE_TRANSFER_MAX_PATH  256

// File transfer receive state
typedef struct {
    bool active;
    char path[FILE_TRANSFER_MAX_PATH];
    FILE *fp;
    uint32_t total_size;
    uint32_t received;
} file_receive_state_t;

static file_receive_state_t g_recv = {0};
static bool g_fs_mounted = false;

// Mount LittleFS if not already mounted
static int ensure_fs_mounted(void)
{
    if (g_fs_mounted) {
        return 0;
    }

#if defined(CONFIG_IDF_TARGET_LINUX)
    // On Linux, use local filesystem directory
    mkdir(FILE_TRANSFER_BASE_PATH, 0755);
    g_fs_mounted = true;
    ESP_LOGI(TAG, "Using local directory: %s", FILE_TRANSFER_BASE_PATH);
    return 0;
#else
    // On ESP32, LittleFS is mounted by app_main before tasks start
    g_fs_mounted = true;
    return 0;
#endif
}

// Build full path from relative path
static int build_full_path(char *out, size_t out_size,
                           const char *rel_path, uint16_t path_len)
{
    if (path_len == 0 || path_len >= 120) {
        return -1;
    }

    // Skip leading '/' to avoid double-slash (e.g. "/flash//boot/...")
    const char *p = rel_path;
    uint16_t len = path_len;
    if (len > 0 && p[0] == '/') {
        p++;
        len--;
    }

    int written = snprintf(out, out_size, "%s/%.*s",
                           FILE_TRANSFER_BASE_PATH, (int)len, p);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

// Create parent directories recursively
static void ensure_parent_dirs(const char *path)
{
    char tmp[FILE_TRANSFER_MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    // Find last '/' and create all parent dirs
    for (char *p = tmp + strlen(FILE_TRANSFER_BASE_PATH) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

// Handle BEGIN command
static int handle_begin(uint8_t type, uint8_t seq,
                        const uint8_t *payload, size_t payload_len)
{
    if (payload_len < sizeof(fmrb_link_file_transfer_begin_t)) {
        ESP_LOGE(TAG, "BEGIN: payload too small");
        return -1;
    }

    const fmrb_link_file_transfer_begin_t *cmd =
        (const fmrb_link_file_transfer_begin_t *)payload;

    const char *rel_path = (const char *)(payload + sizeof(fmrb_link_file_transfer_begin_t));
    uint16_t path_len = cmd->path_len;

    if (sizeof(fmrb_link_file_transfer_begin_t) + path_len > payload_len) {
        ESP_LOGE(TAG, "BEGIN: path extends beyond payload");
        return -1;
    }

    if (ensure_fs_mounted() != 0) {
        return -1;
    }

    // Abort any previous active transfer
    if (g_recv.active && g_recv.fp) {
        fclose(g_recv.fp);
        g_recv.fp = NULL;
        g_recv.active = false;
        ESP_LOGW(TAG, "Aborting previous active transfer");
    }

    // Build full path
    if (build_full_path(g_recv.path, sizeof(g_recv.path), rel_path, path_len) != 0) {
        ESP_LOGE(TAG, "BEGIN: invalid path");
        return -1;
    }

    ensure_parent_dirs(g_recv.path);

    g_recv.fp = fopen(g_recv.path, "wb");
    if (!g_recv.fp) {
        ESP_LOGE(TAG, "BEGIN: failed to open %s for writing: %s", g_recv.path, strerror(errno));
        return -1;
    }

    g_recv.total_size = cmd->total_size;
    g_recv.received = 0;
    g_recv.active = true;

    ESP_LOGI(TAG, "BEGIN: path=%s, total_size=%u", g_recv.path, (unsigned)g_recv.total_size);
    return 0;
}

// Handle DATA command
static int handle_data(uint8_t type, uint8_t seq,
                       const uint8_t *payload, size_t payload_len)
{
    if (!g_recv.active || !g_recv.fp) {
        ESP_LOGE(TAG, "DATA: no active transfer");
        return -1;
    }

    if (payload_len < sizeof(fmrb_link_file_transfer_data_t)) {
        ESP_LOGE(TAG, "DATA: payload too small");
        return -1;
    }

    const fmrb_link_file_transfer_data_t *cmd =
        (const fmrb_link_file_transfer_data_t *)payload;

    const uint8_t *chunk_data = payload + sizeof(fmrb_link_file_transfer_data_t);
    uint16_t chunk_len = cmd->chunk_len;

    if (sizeof(fmrb_link_file_transfer_data_t) + chunk_len > payload_len) {
        ESP_LOGE(TAG, "DATA: chunk extends beyond payload");
        return -1;
    }

    if (cmd->offset != g_recv.received) {
        ESP_LOGW(TAG, "DATA: offset mismatch, expected=%u, got=%u",
                (unsigned)g_recv.received, (unsigned)cmd->offset);
        // Seek to the correct position
        fseek(g_recv.fp, cmd->offset, SEEK_SET);
        g_recv.received = cmd->offset;
    }

    size_t written = fwrite(chunk_data, 1, chunk_len, g_recv.fp);
    if (written != chunk_len) {
        ESP_LOGE(TAG, "DATA: write failed, expected=%u, written=%u",
                (unsigned)chunk_len, (unsigned)written);
        fclose(g_recv.fp);
        g_recv.fp = NULL;
        g_recv.active = false;
        return -1;
    }

    g_recv.received += chunk_len;

    ESP_LOGD(TAG, "DATA: offset=%u, chunk_len=%u, total_received=%u/%u",
            (unsigned)cmd->offset, (unsigned)chunk_len,
            (unsigned)g_recv.received, (unsigned)g_recv.total_size);
    return 0;
}

// Handle END command
static int handle_end(uint8_t type, uint8_t seq,
                      const uint8_t *payload, size_t payload_len)
{
    if (!g_recv.active || !g_recv.fp) {
        ESP_LOGE(TAG, "END: no active transfer");
        return -1;
    }

    if (payload_len < sizeof(fmrb_link_file_transfer_end_t)) {
        ESP_LOGE(TAG, "END: payload too small");
        return -1;
    }

    const fmrb_link_file_transfer_end_t *cmd =
        (const fmrb_link_file_transfer_end_t *)payload;

    fclose(g_recv.fp);
    g_recv.fp = NULL;
    g_recv.active = false;

    if (g_recv.received != cmd->total_size) {
        ESP_LOGE(TAG, "END: size mismatch, received=%u, expected=%u",
                (unsigned)g_recv.received, (unsigned)cmd->total_size);
        remove(g_recv.path);
        return -1;
    }

    // TODO: verify CRC32 checksum if cmd->checksum != 0

    ESP_LOGI(TAG, "END: transfer complete, %u bytes written to %s",
            (unsigned)g_recv.received, g_recv.path);
    return 0;
}

// Handle STATUS command
static int handle_status(uint8_t type, uint8_t seq,
                         const uint8_t *payload, size_t payload_len)
{
    if (payload_len < sizeof(fmrb_link_file_transfer_status_t)) {
        ESP_LOGE(TAG, "STATUS: payload too small");
        return -1;
    }

    const fmrb_link_file_transfer_status_t *cmd =
        (const fmrb_link_file_transfer_status_t *)payload;

    const char *rel_path = (const char *)(payload + sizeof(fmrb_link_file_transfer_status_t));

    if (sizeof(fmrb_link_file_transfer_status_t) + cmd->path_len > payload_len) {
        ESP_LOGE(TAG, "STATUS: path extends beyond payload");
        return -1;
    }

    if (ensure_fs_mounted() != 0) {
        return -1;
    }

    char full_path[FILE_TRANSFER_MAX_PATH];
    if (build_full_path(full_path, sizeof(full_path), rel_path, cmd->path_len) != 0) {
        ESP_LOGE(TAG, "STATUS: invalid path");
        return -1;
    }

    fmrb_link_file_transfer_status_resp_t resp = {0};

    struct stat st;
    if (stat(full_path, &st) == 0) {
        resp.exists = 1;
        resp.file_size = (uint32_t)st.st_size;
        resp.checksum = 0;  // TODO: compute CRC32
    }

    ESP_LOGI(TAG, "STATUS: path=%s, exists=%d, size=%u",
            full_path, resp.exists, (unsigned)resp.file_size);

    // Send ACK with status response
    const comm_interface_t *comm = comm_get_interface();
    if (comm) {
        comm->send_ack(type, seq, (const uint8_t *)&resp, sizeof(resp));
    }

    return 1;  // ACK already sent
}

// Handle DELETE command
static int handle_delete(uint8_t type, uint8_t seq,
                         const uint8_t *payload, size_t payload_len)
{
    if (payload_len < sizeof(fmrb_link_file_transfer_delete_t)) {
        ESP_LOGE(TAG, "DELETE: payload too small");
        return -1;
    }

    const fmrb_link_file_transfer_delete_t *cmd =
        (const fmrb_link_file_transfer_delete_t *)payload;

    const char *rel_path = (const char *)(payload + sizeof(fmrb_link_file_transfer_delete_t));

    if (sizeof(fmrb_link_file_transfer_delete_t) + cmd->path_len > payload_len) {
        ESP_LOGE(TAG, "DELETE: path extends beyond payload");
        return -1;
    }

    if (ensure_fs_mounted() != 0) {
        return -1;
    }

    char full_path[FILE_TRANSFER_MAX_PATH];
    if (build_full_path(full_path, sizeof(full_path), rel_path, cmd->path_len) != 0) {
        ESP_LOGE(TAG, "DELETE: invalid path");
        return -1;
    }

    int ret = remove(full_path);
    if (ret != 0) {
        ESP_LOGW(TAG, "DELETE: failed to remove %s: %s", full_path, strerror(errno));
    } else {
        ESP_LOGI(TAG, "DELETE: removed %s", full_path);
    }

    return 0;
}

int file_transfer_handler_init(void)
{
    memset(&g_recv, 0, sizeof(g_recv));
    return ensure_fs_mounted();
}

int file_transfer_handler_process(uint8_t type, uint8_t sub_cmd, uint8_t seq,
                                  const uint8_t *payload, size_t payload_len)
{
    switch (sub_cmd) {
        case FMRB_LINK_FILE_TRANSFER_BEGIN:
            return handle_begin(type, seq, payload, payload_len);

        case FMRB_LINK_FILE_TRANSFER_DATA:
            return handle_data(type, seq, payload, payload_len);

        case FMRB_LINK_FILE_TRANSFER_END:
            return handle_end(type, seq, payload, payload_len);

        case FMRB_LINK_FILE_TRANSFER_STATUS:
            return handle_status(type, seq, payload, payload_len);

        case FMRB_LINK_FILE_TRANSFER_DELETE:
            return handle_delete(type, seq, payload, payload_len);

        default:
            ESP_LOGE(TAG, "Unknown file transfer sub-command: 0x%02x", sub_cmd);
            return -1;
    }
}
