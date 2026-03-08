#include "comm_interface.h"

#ifndef CONFIG_IDF_TARGET_LINUX

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/message_buffer.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fmrb_link_msgpack.h"
#include "comm_message.h"
#include "fmrb_link_protocol.h"
#include "fmrb_pin_assign.h"

static const char *TAG = "spi_slave";

// Debug: set to true to enable TX/RX frame hex dumps
static bool s_debug_dump = false;

// SPI Slave pin configuration - must match master's configuration
#define SPI_HOST_ID      SPI2_HOST

// Fixed frame size - MUST match Master (increased for better throughput)
#define SPI_FRAME_SIZE   (COMM_MSG_MAX_PAYLOAD)

// Double buffering for continuous operation
// Keep at 2: ACK loaded into completed buffer is re-queued next,
// so master receives it after just 1 additional poll.
#define NUM_BUFFERS      2

// MessageBuffer handle for forwarding decoded messages
static MessageBufferHandle_t s_msg_buffer = NULL;

// DMA-capable buffers (dynamically allocated)
static uint8_t *rx_buffers[NUM_BUFFERS] = {NULL, NULL};
static uint8_t *tx_buffers[NUM_BUFFERS] = {NULL, NULL};
static spi_slave_transaction_t transactions[NUM_BUFFERS];
static int current_buf = 0;

static int spi_running = 0;
static SemaphoreHandle_t trans_ready_sem = NULL;

// ACK queue: message_handler_task enqueues, comm_task dequeues and handles DMA+GPIO
typedef struct {
    uint8_t data[SPI_FRAME_SIZE];
    size_t len;
} ack_queue_item_t;

static QueueHandle_t s_ack_queue = NULL;
static volatile int ack_buf_idx = -1;  // Which TX buffer contains the ACK (-1 = none)

// Handshake GPIO control (active LOW: LOW = slave has data, HIGH = idle)
static inline void spi_handshake_set_ready(void) {
    gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 0);
}

static inline void spi_handshake_set_idle(void) {
    gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 1);
}

// Cached EMPTY frame (encoded once at init, reused for all TX buffer fills)
static uint8_t s_empty_frame[SPI_FRAME_SIZE];
static size_t s_empty_frame_len = 0;

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

// Process a COBS frame and send the decoded message to MessageBuffer
static int process_cobs_frame(const uint8_t *encoded_data, size_t encoded_len) {
    message_data_t msg;
    size_t payload_len;

    // Decode frame directly into message_data_t
    int result = fmrb_link_decode_frame(encoded_data, encoded_len,
                                       &msg.type, &msg.seq, &msg.sub_cmd,
                                       msg.payload, sizeof(msg.payload),
                                       &payload_len);
    if (result != 0) {
        ESP_LOGE(TAG, "Frame decode failed");
        return -1;
    }
    msg.payload_len = (uint16_t)payload_len;

    // Skip EMPTY frames (no-data polling from master)
    if (msg.type == FMRB_LINK_TYPE_EMPTY) {
        return 0;
    }

    ESP_LOGD(TAG, "RX msgpack: type=%d seq=%d sub_cmd=0x%02x payload_len=%zu",
               msg.type, msg.seq, msg.sub_cmd, payload_len);

    // Send directly to MessageBuffer
    size_t send_size = offsetof(message_data_t, payload) + payload_len;
    size_t bytes_sent = xMessageBufferSend(s_msg_buffer, &msg, send_size, pdMS_TO_TICKS(100));
    if (bytes_sent == 0) {
        ESP_LOGE(TAG, "Failed to send to MessageBuffer (full?)");
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

static int spi_init(MessageBufferHandle_t msg_buffer) {
    if (spi_running) {
        return 0;
    }

    s_msg_buffer = msg_buffer;

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
    // Create ACK queue (message_handler_task -> comm_task)
    s_ack_queue = xQueueCreate(2, sizeof(ack_queue_item_t));
    if (!s_ack_queue) {
        ESP_LOGE(TAG, "Failed to create ACK queue");
        goto cleanup_buffers;
    }
    // Encode EMPTY frame (cached for reuse in TX buffer fills)
    memset(s_empty_frame, 0, SPI_FRAME_SIZE);
    int enc_ret = fmrb_link_encode_ack(FMRB_LINK_TYPE_EMPTY, 0, NULL, 0,
                                        s_empty_frame, SPI_FRAME_SIZE, &s_empty_frame_len);
    if (enc_ret != 0) {
        ESP_LOGE(TAG, "Failed to encode EMPTY frame");
        goto cleanup_buffers;
    }

    // Fill initial TX buffers with EMPTY frame
    for (int i = 0; i < NUM_BUFFERS; i++) {
        memcpy(tx_buffers[i], s_empty_frame, s_empty_frame_len);
    }
    ESP_LOGI(TAG, "DMA buffers allocated (frame_size=%d, num_buffers=%d, empty_frame=%zu bytes)",
             SPI_FRAME_SIZE, NUM_BUFFERS, s_empty_frame_len);

    // Initialize handshake GPIO (active LOW, externally pulled up)
    gpio_config_t hs_conf = {
        .pin_bit_mask = (1ULL << FMRB_PIN_SPI_HANDSHAKE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&hs_conf);
    spi_handshake_set_idle();
    ack_buf_idx = -1;

    // Create binary semaphore for transaction complete signaling
    trans_ready_sem = xSemaphoreCreateBinary();
    if (!trans_ready_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        goto cleanup_buffers;
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
cleanup_buffers:
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (rx_buffers[i]) heap_caps_free(rx_buffers[i]);
        if (tx_buffers[i]) heap_caps_free(tx_buffers[i]);
        rx_buffers[i] = NULL;
        tx_buffers[i] = NULL;
    }
    return -1;
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
                ESP_LOGD(TAG, "FRAME DECODED OK: frame_end=%d", (int)frame_end);
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

    // Check if THIS completed buffer carried a previous ACK (now transmitted)
    bool prev_ack_transmitted = (ack_buf_idx == buf_idx);
    if (prev_ack_transmitted) {
        ack_buf_idx = -1;
    }

    // Pack ACK frames into TX buffer (multiple frames concatenated with 0x00 delimiters)
    size_t tx_offset = 0;
    bool ack_loaded = false;
    ack_queue_item_t ack_item;
    while (xQueueReceive(s_ack_queue, &ack_item, 0) == pdTRUE) {
        if (tx_offset + ack_item.len <= SPI_FRAME_SIZE) {
            memcpy(tx_buffers[buf_idx] + tx_offset, ack_item.data, ack_item.len);
            tx_offset += ack_item.len;
            ack_loaded = true;
        } else {
            // No room - put back at front of queue for next transaction
            xQueueSendToFront(s_ack_queue, &ack_item, 0);
            break;
        }
    }

    if (ack_loaded) {
        ack_buf_idx = buf_idx;
    } else {
        // No ACK - fill with EMPTY
        memcpy(tx_buffers[buf_idx], s_empty_frame, s_empty_frame_len);
    }

    // Re-queue ASAP to keep slave SPI queue non-empty.
    // All slow operations (logging, GPIO) must be AFTER this point.
    queue_next_transaction();

    // GPIO control after queue: buffer is in SPI queue before signaling master
    if (ack_loaded) {
        ESP_LOGI(TAG, "ACK loaded to TX buf[%d], GPIO LOW", buf_idx);
        if (s_debug_dump) ESP_LOGI(TAG, "TX buf[%d][0..31]: "
                 "%02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x",
                 buf_idx,
                 tx_buffers[buf_idx][0],  tx_buffers[buf_idx][1],
                 tx_buffers[buf_idx][2],  tx_buffers[buf_idx][3],
                 tx_buffers[buf_idx][4],  tx_buffers[buf_idx][5],
                 tx_buffers[buf_idx][6],  tx_buffers[buf_idx][7],
                 tx_buffers[buf_idx][8],  tx_buffers[buf_idx][9],
                 tx_buffers[buf_idx][10], tx_buffers[buf_idx][11],
                 tx_buffers[buf_idx][12], tx_buffers[buf_idx][13],
                 tx_buffers[buf_idx][14], tx_buffers[buf_idx][15],
                 tx_buffers[buf_idx][16], tx_buffers[buf_idx][17],
                 tx_buffers[buf_idx][18], tx_buffers[buf_idx][19],
                 tx_buffers[buf_idx][20], tx_buffers[buf_idx][21],
                 tx_buffers[buf_idx][22], tx_buffers[buf_idx][23],
                 tx_buffers[buf_idx][24], tx_buffers[buf_idx][25],
                 tx_buffers[buf_idx][26], tx_buffers[buf_idx][27],
                 tx_buffers[buf_idx][28], tx_buffers[buf_idx][29],
                 tx_buffers[buf_idx][30], tx_buffers[buf_idx][31]);
        spi_handshake_set_ready();
    } else if (prev_ack_transmitted) {
        spi_handshake_set_idle();
        ESP_LOGI(TAG, "ACK transmitted from buf[%d], GPIO HIGH", buf_idx);
    }
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

    // Encode ACK into queue item (comm_task will handle DMA buffer + GPIO)
    ack_queue_item_t item;
    memset(item.data, 0, SPI_FRAME_SIZE);
    int result = fmrb_link_encode_ack(type, seq, response_data, response_len,
                                     item.data, SPI_FRAME_SIZE,
                                     &item.len);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to encode ACK");
        return -1;
    }

    if (xQueueSend(s_ack_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "ACK queue full");
        return -1;
    }

    // Signal master that slave has data (master will poll via SPI)
    spi_handshake_set_ready();

    ESP_LOGI(TAG, "ACK queued: type=%u seq=%u resp_len=%u encoded=%u",
           type, seq, response_len, (unsigned)item.len);
    return 0;
}

static int spi_is_running(void) {
    return spi_running;
}

static void spi_cleanup(void) {
    if (spi_running) {
        spi_slave_free(SPI_HOST_ID);
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

    // Free ACK queue
    if (s_ack_queue) {
        vQueueDelete(s_ack_queue);
        s_ack_queue = NULL;
    }

    s_msg_buffer = NULL;
    spi_running = 0;
    ESP_LOGI(TAG, "SPI slave communication stopped");
}

static const comm_interface_t spi_comm = {
    .init = spi_init,
    .send = NULL,
    .receive = NULL,
    .process = spi_process,
    .send_ack = spi_send_ack,
    .is_running = spi_is_running,
    .cleanup = spi_cleanup,
};

const comm_interface_t* comm_get_interface(void) {
    return &spi_comm;
}

#endif // !CONFIG_IDF_TARGET_LINUX
