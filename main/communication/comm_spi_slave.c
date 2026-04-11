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
#include "spi_frame.h"

static const char *TAG = "spi_slave";

#define SPI_HOST_ID      SPI2_HOST
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

// ACK queue: message_handler_task enqueues via send_ack(), comm_task dequeues
typedef struct {
    uint8_t ack_seq;
    uint8_t status;
    uint8_t data[FMRB_LINK_FRAME_MAX_DATA];  // COBS encoded response (optional)
    uint16_t data_len;
} ack_queue_item_t;

static QueueHandle_t s_ack_queue = NULL;

// Latest response state (updated from ACK queue, referenced by fill_response)
static volatile uint8_t s_last_status = STS_BOOT;
static volatile uint8_t s_last_ack_seq = 0;
static uint8_t s_resp_data[FMRB_LINK_FRAME_MAX_DATA];
static volatile uint16_t s_resp_data_len = 0;

// Set during COBS parsing when any message has ACK_REQUIRED flag
static volatile bool s_ack_required_in_frame = false;

// Pending transaction counter (ISR decrements, task increments)
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_pending_trans = 0;
static volatile uint32_t s_queue_empty_count = 0;

// READY GPIO control (HIGH = ready to receive, LOW = busy)
static inline void IRAM_ATTR set_spi_ready(void) {
    gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 1);
}
static inline void IRAM_ATTR set_spi_busy(void) {
    gpio_set_level(FMRB_PIN_SPI_HANDSHAKE, 0);
}

// ISR: transaction setup (do nothing; READY is managed by pending counter)
static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
}

// ISR: transaction complete → pending update + READY control + semaphore notify
static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    portENTER_CRITICAL_ISR(&s_pending_mux);
    s_pending_trans--;
    if (s_pending_trans <= 0) {
        set_spi_busy();  // Queue exhausted → block master
        s_queue_empty_count++;
    }
    portEXIT_CRITICAL_ISR(&s_pending_mux);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(trans_ready_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// Build response frame into TX buffer
static void fill_response(uint8_t *tx_buf)
{
    spi_frame_t *f = (spi_frame_t *)tx_buf;
    memset(f, 0, FMRB_LINK_FRAME_SIZE);
    f->magic = SPI_FRAME_MAGIC;
    f->seq = 0;
    f->ack_seq = s_last_ack_seq;
    f->status = s_last_status;
    f->data_len = s_resp_data_len;
    if (s_resp_data_len > 0) {
        memcpy(f->data, s_resp_data, s_resp_data_len);
    }
    spi_frame_finalize(f);
}

// Queue next transaction to keep slave always ready
static esp_err_t queue_next_transaction(void)
{
    int buf_idx = current_buf;

    transactions[buf_idx].length = FMRB_LINK_FRAME_SIZE * 8;  // Length in bits
    transactions[buf_idx].tx_buffer = tx_buffers[buf_idx];
    transactions[buf_idx].rx_buffer = rx_buffers[buf_idx];

    esp_err_t ret = spi_slave_queue_trans(SPI_HOST_ID, &transactions[buf_idx], 0);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_pending_mux);
        s_pending_trans++;
        portEXIT_CRITICAL(&s_pending_mux);
    }
    return ret;
}

// Process a COBS frame from data[] and send decoded message to MessageBuffer
static int process_cobs_frame(const uint8_t *encoded_data, size_t encoded_len) {
    message_data_t msg;
    size_t payload_len;

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

    // Track whether any message in this frame requires a per-message ACK
    if (msg.type & FMRB_LINK_FLAG_ACK_REQUIRED) {
        s_ack_required_in_frame = true;
    }

    ESP_LOGD(TAG, "RX msgpack: type=%d seq=%d sub_cmd=0x%02x payload_len=%zu",
               msg.type, msg.seq, msg.sub_cmd, payload_len);

    size_t send_size = offsetof(message_data_t, payload) + payload_len;
    size_t bytes_sent = xMessageBufferSend(s_msg_buffer, &msg, send_size, pdMS_TO_TICKS(100));
    if (bytes_sent == 0) {
        ESP_LOGE(TAG, "Failed to send to MessageBuffer (full?)");
        return -1;
    }

    return 0;
}

static uint32_t s_trans_count = 0;
static uint32_t s_data_trans_count = 0;
static uint32_t s_frame_decoded_count = 0;

// Process a single completed transaction using spi_frame_t header
static int process_single_transaction(spi_slave_transaction_t *completed_trans) {
    s_trans_count++;
    int messages_processed = 0;

    int buf_idx = (completed_trans->rx_buffer == rx_buffers[0]) ? 0 : 1;
    spi_frame_t *f = (spi_frame_t *)completed_trans->rx_buffer;

    // Validate frame header + CRC16 and process COBS payload
    bool data_frame_received = false;
    s_ack_required_in_frame = false;
    if (spi_frame_validate(f) && f->data_len > 0) {
        s_data_trans_count++;
        data_frame_received = true;

        // Update status to RX_OK immediately (master can see via polling)
        s_last_ack_seq = f->seq;
        s_last_status = STS_RX_OK;
        s_resp_data_len = 0;

        // Parse multiple COBS messages from data[0..data_len-1]
        size_t pos = 0;
        while (pos < f->data_len) {
            // Skip leading 0x00 delimiters
            while (pos < f->data_len && f->data[pos] == 0x00) pos++;
            if (pos >= f->data_len) break;

            // Find COBS frame end (0x00)
            size_t frame_start = pos;
            while (pos < f->data_len && f->data[pos] != 0x00) pos++;
            size_t frame_len = pos - frame_start;

            if (process_cobs_frame(f->data + frame_start, frame_len) == 0) {
                s_frame_decoded_count++;
                messages_processed++;
            }
        }
    } else if (f->magic == SPI_FRAME_MAGIC && !spi_frame_validate(f)) {
        // CRC error
        s_last_ack_seq = f->seq;
        s_last_status = STS_CRC_ERR;
        s_resp_data_len = 0;
        ESP_LOGW(TAG, "CRC error on seq=%u", f->seq);
    }
    // magic mismatch or data_len==0: dummy polling → no status change

    // Dequeue ACK response from message_handler_task (if available).
    // Only CONTROL commands (VERSION, INIT_DISPLAY) enqueue here;
    // GRAPHICS commands do not send per-message ACKs.
    ack_queue_item_t ack_item;
    if (xQueueReceive(s_ack_queue, &ack_item, 0) == pdTRUE) {
        s_last_ack_seq = ack_item.ack_seq;
        s_last_status = ack_item.status;
        s_resp_data_len = ack_item.data_len;
        if (ack_item.data_len > 0) {
            memcpy(s_resp_data, ack_item.data, ack_item.data_len);
        }
    } else if (data_frame_received && s_last_status == STS_RX_OK && !s_ack_required_in_frame) {
        // No per-message ACK expected (fire-and-forget, e.g. GFX batch).
        // Promote to APP_OK at frame level so the master does not
        // have to poll indefinitely for a matching ACK.
        s_last_status = STS_APP_OK;
    }

    // Fill TX buffer with response → re-queue → update READY
    current_buf = buf_idx;
    fill_response(tx_buffers[buf_idx]);

    ESP_LOGD(TAG, "RX: seq=%u ack=%u st=0x%02x dlen=%u msgs=%d | TX: ack=%u st=0x%02x dlen=%u",
             f->seq, f->ack_seq, f->status, f->data_len, messages_processed,
             s_last_ack_seq, s_last_status, s_resp_data_len);

    queue_next_transaction();
    portENTER_CRITICAL(&s_pending_mux);
    int pending = s_pending_trans;
    portEXIT_CRITICAL(&s_pending_mux);
    if (pending > 0) {
        set_spi_ready();
    }

    // Queue exhaustion warning (logged from task context)
    portENTER_CRITICAL(&s_pending_mux);
    uint32_t empty_count = s_queue_empty_count;
    s_queue_empty_count = 0;
    portEXIT_CRITICAL(&s_pending_mux);
    if (empty_count > 0) {
        //ESP_LOGW(TAG, "SPI queue exhausted %lu times", empty_count);
    }

    return messages_processed;
}

static int spi_process(void) {
    if (!spi_running) {
        return 0;
    }

    // Wait for at least one transaction to complete (signaled from ISR)
    if (xSemaphoreTake(trans_ready_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    int messages_processed = 0;
    spi_slave_transaction_t *completed_trans;

    // Drain ALL completed transactions from the SPI driver's result queue
    while (spi_slave_get_trans_result(SPI_HOST_ID, &completed_trans, 0) == ESP_OK) {
        messages_processed += process_single_transaction(completed_trans);
    }

    return messages_processed;
}

// Call periodically to print SPI stats
void spi_slave_print_stats(void) {
    ESP_LOGI(TAG, "stats: trans=%lu data=%lu decoded=%lu queue_empty=%lu",
           s_trans_count, s_data_trans_count, s_frame_decoded_count,
           s_queue_empty_count);
}

static int spi_send_ack(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len) {
    if (!spi_running) {
        ESP_LOGE(TAG, "Cannot send ACK: SPI not running");
        return -1;
    }

    ack_queue_item_t item = {
        .ack_seq = seq,
        .status = STS_APP_OK,
        .data_len = 0,
    };

    // If response payload exists, COBS encode it into data[]
    if (response_data && response_len > 0) {
        size_t enc_len = 0;
        int enc_ret = fmrb_link_encode_ack(type, seq, response_data, response_len,
                                           item.data, FMRB_LINK_FRAME_MAX_DATA, &enc_len);
        if (enc_ret != 0) {
            ESP_LOGE(TAG, "Failed to encode ACK");
            return -1;
        }
        item.data_len = (uint16_t)enc_len;
    }

    if (xQueueSend(s_ack_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "ACK queue full");
        return -1;
    }

    ESP_LOGD(TAG, "ACK queued: type=%u seq=%u resp_len=%u data_len=%u",
           type, seq, response_len, item.data_len);
    return 0;
}

static int spi_init(MessageBufferHandle_t msg_buffer) {
    if (spi_running) {
        return 0;
    }

    s_msg_buffer = msg_buffer;

    // Allocate DMA-capable buffers (double buffered)
    for (int i = 0; i < NUM_BUFFERS; i++) {
        rx_buffers[i] = (uint8_t *)heap_caps_malloc(FMRB_LINK_FRAME_SIZE, MALLOC_CAP_DMA);
        tx_buffers[i] = (uint8_t *)heap_caps_malloc(FMRB_LINK_FRAME_SIZE, MALLOC_CAP_DMA);
        if (!rx_buffers[i] || !tx_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate DMA buffers");
            goto cleanup_buffers;
        }
        memset(rx_buffers[i], 0, FMRB_LINK_FRAME_SIZE);
        memset(tx_buffers[i], 0, FMRB_LINK_FRAME_SIZE);
        memset(&transactions[i], 0, sizeof(spi_slave_transaction_t));
    }

    // Create ACK queue (message_handler_task -> comm_task)
    s_ack_queue = xQueueCreate(2, sizeof(ack_queue_item_t));
    if (!s_ack_queue) {
        ESP_LOGE(TAG, "Failed to create ACK queue");
        goto cleanup_buffers;
    }

    // READY GPIO initialization (LOW = not ready during init)
    gpio_config_t hs_conf = {
        .pin_bit_mask = (1ULL << FMRB_PIN_SPI_HANDSHAKE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&hs_conf);
    set_spi_busy();

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
        .max_transfer_sz = FMRB_LINK_FRAME_SIZE,
    };

    spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = FMRB_PIN_SPI_CS,
        .queue_size = NUM_BUFFERS,
        .flags = 0,
        .post_setup_cb = spi_post_setup_cb,
        .post_trans_cb = spi_post_trans_cb,
    };

    // Enable pull-ups on SPI lines for stability
    gpio_set_pull_mode(FMRB_PIN_SPI_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(FMRB_PIN_SPI_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(FMRB_PIN_SPI_CS, GPIO_PULLUP_ONLY);

    esp_err_t ret = spi_slave_initialize(SPI_HOST_ID, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI slave initialization failed: %d", ret);
        goto cleanup_sem;
    }

    // Fill initial TX buffers with BOOT status frame and queue
    for (int i = 0; i < NUM_BUFFERS; i++) {
        fill_response(tx_buffers[i]);
        current_buf = i;
        ret = queue_next_transaction();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to queue initial transaction %d: %d", i, ret);
        }
    }
    current_buf = 0;
    portENTER_CRITICAL(&s_pending_mux);
    s_pending_trans = NUM_BUFFERS;
    s_queue_empty_count = 0;
    portEXIT_CRITICAL(&s_pending_mux);

    set_spi_ready();
    spi_running = 1;
    ESP_LOGI(TAG, "SPI slave initialized - MOSI:%d MISO:%d CLK:%d CS:%d HS:%d (frame=%d bytes)",
           FMRB_PIN_SPI_MOSI, FMRB_PIN_SPI_MISO, FMRB_PIN_SPI_CLK,
           FMRB_PIN_SPI_CS, FMRB_PIN_SPI_HANDSHAKE, FMRB_LINK_FRAME_SIZE);
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
