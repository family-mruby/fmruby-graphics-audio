#include "comm_interface.h"

#ifndef CONFIG_IDF_TARGET_LINUX

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fmrb_link_msgpack.h"
#include "message_queue.h"
#include "fmrb_link_protocol.h"

static const char *TAG = "spi_slave";

// SPI Slave pin configuration - must match master's configuration
#define SPI_HOST_ID      SPI2_HOST
#define PIN_NUM_MISO     18   // Slave Output (to Master Input)
#define PIN_NUM_MOSI     21   // Slave Input (from Master Output)
#define PIN_NUM_CLK      19
#define PIN_NUM_CS       22

// Fixed frame size - MUST match Master
#define SPI_FRAME_SIZE   64

// Double buffering for continuous operation
#define NUM_BUFFERS      2

// Message queue for decoded messages
static message_queue_t g_message_queue;

// DMA-capable buffers (dynamically allocated)
static uint8_t *rx_buffers[NUM_BUFFERS] = {NULL, NULL};
static uint8_t *tx_buffers[NUM_BUFFERS] = {NULL, NULL};
static spi_slave_transaction_t transactions[NUM_BUFFERS];
static int current_buf = 0;

static int spi_running = 0;
static SemaphoreHandle_t spi_mutex = NULL;
static SemaphoreHandle_t trans_ready_sem = NULL;

// Callback called after a transaction is done (ISR context)
static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *trans)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(trans_ready_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// Callback called before a transaction starts (ISR context)
static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t *trans)
{
    // Transaction is queued and ready
}

// Process a COBS frame and enqueue the decoded message
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

// Queue next transaction to keep slave always ready
static esp_err_t queue_next_transaction(void)
{
    int buf_idx = current_buf;

    transactions[buf_idx].length = SPI_FRAME_SIZE * 8;  // Length in bits
    transactions[buf_idx].tx_buffer = tx_buffers[buf_idx];
    transactions[buf_idx].rx_buffer = rx_buffers[buf_idx];

    return spi_slave_queue_trans(SPI_HOST_ID, &transactions[buf_idx], 0);
}

static int spi_init(void) {
    if (spi_running) {
        return 0;
    }

    // Initialize message queue
    message_queue_init(&g_message_queue);

    // Allocate DMA-capable buffers (double buffered)
    for (int i = 0; i < NUM_BUFFERS; i++) {
        rx_buffers[i] = (uint8_t *)heap_caps_malloc(SPI_FRAME_SIZE, MALLOC_CAP_DMA);
        tx_buffers[i] = (uint8_t *)heap_caps_malloc(SPI_FRAME_SIZE, MALLOC_CAP_DMA);
        if (!rx_buffers[i] || !tx_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate DMA buffers");
            goto cleanup_buffers;
        }
        memset(rx_buffers[i], 0, SPI_FRAME_SIZE);
        memset(tx_buffers[i], 0, SPI_FRAME_SIZE);
        memset(&transactions[i], 0, sizeof(spi_slave_transaction_t));
    }
    ESP_LOGI(TAG, "DMA buffers allocated (frame_size=%d, double_buffered)", SPI_FRAME_SIZE);

    // Create mutex for thread safety
    spi_mutex = xSemaphoreCreateMutex();
    if (!spi_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        goto cleanup_buffers;
    }

    // Create binary semaphore for transaction complete signaling
    trans_ready_sem = xSemaphoreCreateBinary();
    if (!trans_ready_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        goto cleanup_mutex;
    }

    // Configure SPI bus for slave mode
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_FRAME_SIZE,
    };

    // Configure SPI slave interface
    spi_slave_interface_config_t slvcfg = {
        .mode = 0,  // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = PIN_NUM_CS,
        .queue_size = NUM_BUFFERS,  // Match buffer count
        .flags = 0,
        .post_setup_cb = spi_post_setup_cb,
        .post_trans_cb = spi_post_trans_cb,
    };

    // Enable pull-ups on SPI lines for stability
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);

    // Initialize SPI slave interface
    esp_err_t ret = spi_slave_initialize(SPI_HOST_ID, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI slave initialization failed: %d", ret);
        goto cleanup_sem;
    }

    // Pre-queue transactions to be always ready
    for (int i = 0; i < NUM_BUFFERS; i++) {
        current_buf = i;
        ret = queue_next_transaction();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to queue initial transaction %d: %d", i, ret);
        }
    }
    current_buf = 0;

    spi_running = 1;
    ESP_LOGI(TAG, "SPI slave initialized - MOSI:%d MISO:%d CLK:%d CS:%d (frame=%d bytes)",
           PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS, SPI_FRAME_SIZE);
    return 0;

cleanup_sem:
    vSemaphoreDelete(trans_ready_sem);
    trans_ready_sem = NULL;
cleanup_mutex:
    vSemaphoreDelete(spi_mutex);
    spi_mutex = NULL;
cleanup_buffers:
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (rx_buffers[i]) heap_caps_free(rx_buffers[i]);
        if (tx_buffers[i]) heap_caps_free(tx_buffers[i]);
        rx_buffers[i] = NULL;
        tx_buffers[i] = NULL;
    }
    return -1;
}

static int spi_send(const uint8_t *data, size_t len) {
    if (!spi_running || !data || len == 0) {
        return -1;
    }

    if (len > SPI_FRAME_SIZE) {
        len = SPI_FRAME_SIZE;
    }

    if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }

    // Copy data to current TX buffer
    memcpy(tx_buffers[current_buf], data, len);

    xSemaphoreGive(spi_mutex);
    return len;
}

static int spi_receive(uint8_t *buf, size_t buf_size) {
    if (!spi_running || !buf || buf_size == 0) {
        return 0;
    }

    if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    // Copy received data from current RX buffer
    size_t copy_len = (buf_size < SPI_FRAME_SIZE) ? buf_size : SPI_FRAME_SIZE;
    memcpy(buf, rx_buffers[current_buf], copy_len);

    xSemaphoreGive(spi_mutex);
    return copy_len;
}

static int spi_process(void) {
    if (!spi_running) {
        return 0;
    }

    // Wait for transaction complete (signaled from ISR)
    if (xSemaphoreTake(trans_ready_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;  // Timeout - no transaction
    }

    // Get the completed transaction
    spi_slave_transaction_t *completed_trans;
    esp_err_t ret = spi_slave_get_trans_result(SPI_HOST_ID, &completed_trans, 0);

    if (ret != ESP_OK) {
        return 0;
    }

    size_t rx_len = completed_trans->trans_len / 8;
    int messages_processed = 0;

    if (rx_len > 0) {
        // Find which buffer was used
        int buf_idx = (completed_trans->rx_buffer == rx_buffers[0]) ? 0 : 1;
        uint8_t *rx_buf = (uint8_t*)completed_trans->rx_buffer;

        ESP_LOGD(TAG, "SPI Slave[%d]: Received %d bytes", buf_idx, (int)rx_len);

        // Look for COBS frame terminator (0x00)
        size_t frame_end = 0;
        while (frame_end < rx_len && rx_buf[frame_end] != 0x00) {
            frame_end++;
        }

        if (frame_end > 0 && frame_end < rx_len) {
            // Found a complete COBS frame
            if (process_cobs_frame(rx_buf, frame_end) == 0) {
                messages_processed++;
            }
        }

        // Re-queue this buffer for next transaction
        current_buf = buf_idx;
        queue_next_transaction();

        return messages_processed;
    }

    // Re-queue even if no data
    queue_next_transaction();
    return 0;
}

static int spi_send_ack(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len) {
    if (!spi_running) {
        ESP_LOGE(TAG, "Cannot send ACK: SPI not running");
        return -1;
    }

    if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Cannot send ACK: mutex timeout");
        return -1;
    }

    uint8_t *tx_buf = tx_buffers[current_buf];
    size_t encoded_len;

    // Encode ACK using common msgpack module
    int result = fmrb_link_encode_ack(type, seq, response_data, response_len,
                                     tx_buf, SPI_FRAME_SIZE,
                                     &encoded_len);

    xSemaphoreGive(spi_mutex);

    if (result != 0) {
        ESP_LOGE(TAG, "Failed to encode ACK");
        return -1;
    }

    ESP_LOGD(TAG, "ACK prepared: type=%u seq=%u response_len=%u encoded_len=%zu",
              type, seq, response_len, encoded_len);
    return 0;
}

static int spi_receive_message(uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
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

static int spi_is_running(void) {
    return spi_running;
}

static void spi_cleanup(void) {
    if (spi_running) {
        spi_slave_free(SPI_HOST_ID);
    }

    if (spi_mutex) {
        vSemaphoreDelete(spi_mutex);
        spi_mutex = NULL;
    }

    if (trans_ready_sem) {
        vSemaphoreDelete(trans_ready_sem);
        trans_ready_sem = NULL;
    }

    // Free DMA buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (rx_buffers[i]) {
            heap_caps_free(rx_buffers[i]);
            rx_buffers[i] = NULL;
        }
        if (tx_buffers[i]) {
            heap_caps_free(tx_buffers[i]);
            tx_buffers[i] = NULL;
        }
    }

    // Clear message queue
    message_queue_init(&g_message_queue);

    spi_running = 0;
    ESP_LOGI(TAG, "SPI slave communication stopped");
}

static const comm_interface_t spi_comm = {
    .init = spi_init,
    .send = spi_send,
    .receive = spi_receive,
    .process = spi_process,
    .receive_message = spi_receive_message,
    .send_ack = spi_send_ack,
    .is_running = spi_is_running,
    .cleanup = spi_cleanup,
};

const comm_interface_t* comm_get_interface(void) {
    return &spi_comm;
}

#endif // !CONFIG_IDF_TARGET_LINUX
