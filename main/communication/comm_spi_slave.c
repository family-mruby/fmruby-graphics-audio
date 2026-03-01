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
#include "fmrb_pin_assign.h"

static const char *TAG = "spi_slave";

// SPI Slave pin configuration - must match master's configuration
#define SPI_HOST_ID      SPI2_HOST

// Fixed frame size - MUST match Master (increased for better throughput)
#define SPI_FRAME_SIZE   256

// Double buffering for continuous operation
#define NUM_BUFFERS      2

// Message queue for decoded messages (dynamically allocated in PSRAM to save 525KB DRAM)
static message_queue_t* g_message_queue = NULL;

// DMA-capable buffers (dynamically allocated)
static uint8_t *rx_buffers[NUM_BUFFERS] = {NULL, NULL};
static uint8_t *tx_buffers[NUM_BUFFERS] = {NULL, NULL};
static spi_slave_transaction_t transactions[NUM_BUFFERS];
static int current_buf = 0;

static int spi_running = 0;
static SemaphoreHandle_t spi_mutex = NULL;
static SemaphoreHandle_t trans_ready_sem = NULL;

// Pending ACK buffer (staged here, copied to TX buffer before re-queue)
static uint8_t *pending_ack_buf = NULL;
static volatile size_t pending_ack_len = 0;
static volatile int ack_buf_idx = -1;  // Which TX buffer contains the ACK (-1 = none)

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
    result = message_queue_enqueue(g_message_queue, type, seq, sub_cmd,
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

    // Allocate message queue in PSRAM (525KB)
    if (!g_message_queue) {
        g_message_queue = (message_queue_t*)heap_caps_malloc(sizeof(message_queue_t), MALLOC_CAP_SPIRAM);
        if (!g_message_queue) {
            ESP_LOGE(TAG, "Failed to allocate message queue in PSRAM (%zu bytes)", sizeof(message_queue_t));
            return -1;
        }
        ESP_LOGI(TAG, "Message queue allocated in PSRAM (%zu bytes)", sizeof(message_queue_t));
    }

    // Initialize message queue
    message_queue_init(g_message_queue);

    // Allocate DMA-capable buffers (double buffered + pending ACK)
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
    if (!pending_ack_buf) {
        pending_ack_buf = (uint8_t *)heap_caps_malloc(SPI_FRAME_SIZE, MALLOC_CAP_DMA);
        if (!pending_ack_buf) {
            ESP_LOGE(TAG, "Failed to allocate pending ACK buffer");
            goto cleanup_buffers;
        }
        memset(pending_ack_buf, 0, SPI_FRAME_SIZE);
        pending_ack_len = 0;
    }
    ESP_LOGI(TAG, "DMA buffers allocated (frame_size=%d, double_buffered)", SPI_FRAME_SIZE);

    // Initialize handshake GPIO (active LOW, externally pulled up)
    gpio_config_t hs_conf = {
        .pin_bit_mask = (1ULL << FMRB_PIN_SPI_HANDSHAKE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&hs_conf);
    gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 1);  // HIGH = idle
    ack_buf_idx = -1;

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
        .mosi_io_num = FMRB_PIN_SPI_MOSI,
        .miso_io_num = FMRB_PIN_SPI_MISO,
        .sclk_io_num = FMRB_PIN_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_FRAME_SIZE,
    };

    // Configure SPI slave interface
    spi_slave_interface_config_t slvcfg = {
        .mode = 0,  // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = FMRB_PIN_SPI_CS,
        .queue_size = NUM_BUFFERS,  // Match buffer count
        .flags = 0,
        .post_setup_cb = spi_post_setup_cb,
        .post_trans_cb = spi_post_trans_cb,
    };

    // Enable pull-ups on SPI lines for stability
    gpio_set_pull_mode(FMRB_PIN_SPI_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(FMRB_PIN_SPI_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(FMRB_PIN_SPI_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(FMRB_PIN_SPI_CS, GPIO_PULLUP_ONLY);

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
           FMRB_PIN_SPI_MOSI, FMRB_PIN_SPI_MISO, FMRB_PIN_SPI_CLK, FMRB_PIN_SPI_CS, SPI_FRAME_SIZE);
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

static uint32_t s_trans_count = 0;
static uint32_t s_data_trans_count = 0;
static uint32_t s_frame_found_count = 0;
static uint32_t s_frame_decoded_count = 0;

// Process a single completed transaction. Returns number of messages decoded.
static int process_single_transaction(spi_slave_transaction_t *completed_trans) {
    s_trans_count++;
    size_t rx_len = completed_trans->trans_len / 8;
    int messages_processed = 0;

    // Find which buffer was used
    int buf_idx = (completed_trans->rx_buffer == rx_buffers[0]) ? 0 : 1;

    // If THIS buffer contained the ACK and was just transmitted, release handshake
    if (ack_buf_idx == buf_idx) {
        gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 1);  // HIGH = idle (ACK was sent)
        ack_buf_idx = -1;
        ESP_LOGI(TAG, "ACK transmitted from buf[%d], GPIO HIGH", buf_idx);
    }

    if (rx_len > 0) {
        s_data_trans_count++;
        uint8_t *rx_buf = (uint8_t*)completed_trans->rx_buffer;

        // Debug: log first few transactions with hex dump
        if (s_data_trans_count <= 10) {
            ESP_LOGI(TAG, "trans#%lu buf[%d] rx_len=%d first8: %02x %02x %02x %02x %02x %02x %02x %02x",
                   s_data_trans_count, buf_idx, (int)rx_len,
                   rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3],
                   rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
        }

        // Look for COBS frame terminator (0x00)
        size_t frame_end = 0;
        while (frame_end < rx_len && rx_buf[frame_end] != 0x00) {
            frame_end++;
        }

        if (frame_end > 0 && frame_end < rx_len) {
            s_frame_found_count++;
            // Found a complete COBS frame
            if (process_cobs_frame(rx_buf, frame_end) == 0) {
                s_frame_decoded_count++;
                ESP_LOGI(TAG, "FRAME DECODED OK: frame_end=%d", (int)frame_end);
                messages_processed++;
            } else {
                ESP_LOGE(TAG, "FRAME DECODE FAILED: frame_end=%d", (int)frame_end);
            }
        } else if (s_data_trans_count <= 10) {
            ESP_LOGD(TAG, "no COBS frame (frame_end=%d, rx_len=%d)", (int)frame_end, (int)rx_len);
        }
    }

    // Prepare TX buffer before re-queue (safe: buffer is not in DMA queue now)
    current_buf = buf_idx;
    memset(tx_buffers[buf_idx], 0, SPI_FRAME_SIZE);

    // Copy pending ACK to TX buffer if available
    if (pending_ack_len > 0) {
        memcpy(tx_buffers[buf_idx], pending_ack_buf, pending_ack_len);
        ESP_LOGI(TAG, "ACK loaded to TX buf[%d] (%d bytes)", buf_idx, (int)pending_ack_len);
        pending_ack_len = 0;
        ack_buf_idx = buf_idx;
        // GPIO already set LOW by spi_send_ack(), no need to set again
    }

    queue_next_transaction();
    return messages_processed;
}

static int spi_process(void) {
    if (!spi_running) {
        return 0;
    }

    // Wait for at least one transaction to complete (signaled from ISR)
    // Note: trans_ready_sem is binary, so multiple ISR gives may be collapsed into one.
    // We compensate by draining all available results below.
    if (xSemaphoreTake(trans_ready_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;  // Timeout - no transaction
    }

    int messages_processed = 0;
    spi_slave_transaction_t *completed_trans;

    // Drain ALL completed transactions from the SPI driver's result queue.
    // The binary semaphore only tells us "at least one completed", but there
    // may be more results waiting (if ISR fired multiple times before we ran).
    while (spi_slave_get_trans_result(SPI_HOST_ID, &completed_trans, 0) == ESP_OK) {
        messages_processed += process_single_transaction(completed_trans);
    }

    return messages_processed;
}

// Call periodically to print SPI stats
void spi_slave_print_stats(void) {
    ESP_LOGI(TAG, "stats: trans=%lu data=%lu frames_found=%lu decoded=%lu",
           s_trans_count, s_data_trans_count, s_frame_found_count, s_frame_decoded_count);
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

    // Encode ACK into pending buffer (NOT directly into DMA TX buffer)
    size_t encoded_len;
    memset(pending_ack_buf, 0, SPI_FRAME_SIZE);
    int result = fmrb_link_encode_ack(type, seq, response_data, response_len,
                                     pending_ack_buf, SPI_FRAME_SIZE,
                                     &encoded_len);

    if (result != 0) {
        xSemaphoreGive(spi_mutex);
        ESP_LOGE(TAG, "Failed to encode ACK");
        return -1;
    }

    pending_ack_len = encoded_len;
    xSemaphoreGive(spi_mutex);

    // Signal Master that ACK is pending (active LOW)
    // Master will poll, triggering a transaction. spi_process() will then
    // load the ACK from pending_ack_buf into the TX buffer.
    gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 0);

    ESP_LOGI(TAG, "ACK staged: type=%u seq=%u resp_len=%u encoded=%u, GPIO LOW",
           type, seq, response_len, (unsigned)encoded_len);
    return 0;
}

static int spi_receive_message(uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
                                const uint8_t **payload, size_t *payload_len) {
    // Dequeue message using common queue module
    int result = message_queue_dequeue(g_message_queue, type, seq, sub_cmd,
                                       payload, payload_len);

    if (result > 0) {
        ESP_LOGD(TAG, "Dequeued message: type=%u seq=%u sub_cmd=0x%02x len=%zu (queue=%d/%d)",
                   *type, *seq, *sub_cmd, *payload_len,
                   message_queue_count(g_message_queue), MSG_QUEUE_MAX_MESSAGES);
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

    // Free pending ACK buffer
    if (pending_ack_buf) {
        heap_caps_free(pending_ack_buf);
        pending_ack_buf = NULL;
    }
    pending_ack_len = 0;

    // Free message queue from PSRAM
    if (g_message_queue) {
        heap_caps_free(g_message_queue);
        g_message_queue = NULL;
    }

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
