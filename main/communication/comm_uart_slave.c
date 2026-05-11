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
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "fmrb_link_msgpack.h"
#include "comm_message.h"
#include "fmrb_link_protocol.h"
#include "fmrb_pin_assign.h"
#include "uart_link_frame.h"

extern uint32_t graphics_handler_get_and_reset_present_count(void);

static const char *TAG = "uart_slave";

#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      921600
#define UART_RX_BUF_SIZE    2048
#define UART_TX_BUF_SIZE    512

// MessageBuffer handle for forwarding decoded messages
static MessageBufferHandle_t s_msg_buffer = NULL;

static int uart_running = 0;

// ACK queue: message_handler_task enqueues via send_ack(), comm_task dequeues
typedef struct {
    uint8_t ack_seq;
    uint8_t status;
    uint8_t data[UART_LINK_MAX_DATA];
    uint16_t data_len;
} ack_queue_item_t;

static QueueHandle_t s_ack_queue = NULL;

// Latest response state
static volatile uint8_t s_last_status = UART_LINK_STS_BOOT;
static volatile uint8_t s_last_ack_seq = 0;

// Set during COBS parsing when any message has ACK_REQUIRED flag
static volatile bool s_ack_required_in_frame = false;

// TX buffer
static uint8_t s_tx_buf[UART_LINK_MAX_FRAME_SIZE];

// RX state machine
typedef enum {
    RX_STATE_SYNC,
    RX_STATE_HEADER,
    RX_STATE_DATA,
    RX_STATE_CRC,
} rx_state_t;

static rx_state_t s_rx_state = RX_STATE_SYNC;
static uint8_t s_rx_header[UART_LINK_HEADER_SIZE];
static uint8_t s_rx_data[UART_LINK_MAX_DATA];
static uint8_t s_rx_crc[UART_LINK_CRC_SIZE];
static size_t s_rx_pos = 0;
static uint16_t s_rx_data_len = 0;

// Statistics
static uint32_t s_frame_count = 0;
static uint32_t s_data_frame_count = 0;
static uint32_t s_decoded_count = 0;
static uint32_t s_crc_error_count = 0;

static void rx_reset(void) {
    s_rx_state = RX_STATE_SYNC;
    s_rx_pos = 0;
    s_rx_data_len = 0;
}

// Process a COBS frame and send decoded message to MessageBuffer
static int process_cobs_frame(const uint8_t *encoded_data, size_t encoded_len) {
    // Skip COBS-encoded empty frames (0x01 = zero-length payload)
    if (encoded_len == 1 && encoded_data[0] == 0x01) {
        return 0;
    }

    message_data_t msg;
    size_t payload_len;

    int result = fmrb_link_decode_frame(encoded_data, encoded_len,
                                       &msg.type, &msg.seq, &msg.sub_cmd,
                                       msg.payload, sizeof(msg.payload),
                                       &payload_len);
    if (result != 0) {
        ESP_LOGE(TAG, "Frame decode failed: len=%zu first=%02X %02X %02X %02X",
                 encoded_len,
                 encoded_len > 0 ? encoded_data[0] : 0,
                 encoded_len > 1 ? encoded_data[1] : 0,
                 encoded_len > 2 ? encoded_data[2] : 0,
                 encoded_len > 3 ? encoded_data[3] : 0);
        return -1;
    }
    msg.payload_len = (uint16_t)payload_len;

    // Skip EMPTY frames
    if (msg.type == FMRB_LINK_TYPE_EMPTY) {
        return 0;
    }

    if (msg.type & FMRB_LINK_FLAG_ACK_REQUIRED) {
        s_ack_required_in_frame = true;
    }

    ESP_LOGD(TAG, "RX msgpack: type=%d seq=%d sub_cmd=0x%02x payload_len=%zu",
               msg.type, msg.seq, msg.sub_cmd, payload_len);

    // Block indefinitely on full buffer: UART HW flow control (RTS/CTS) will
    // backpressure the master, which is the correct response. Dropping here
    // silently lost trailing pixels of icon sprite uploads.
    size_t send_size = offsetof(message_data_t, payload) + payload_len;
    size_t bytes_sent = xMessageBufferSend(s_msg_buffer, &msg, send_size, portMAX_DELAY);
    if (bytes_sent == 0) {
        ESP_LOGE(TAG, "Failed to send to MessageBuffer");
        return -1;
    }

    return 0;
}

// Throughput statistics (forward declaration for send_response)
static uint32_t s_stats_tx_bytes;

// Send ACK/response frame via UART
static void send_response(uint8_t ack_seq, uint8_t status,
                          const uint8_t *data, uint16_t data_len) {
    size_t frame_size = uart_link_build_frame(s_tx_buf, 0, ack_seq, status,
                                              data, data_len);
    uart_write_bytes(UART_PORT_NUM, s_tx_buf, frame_size);
    s_stats_tx_bytes += frame_size;
    ESP_LOGD(TAG, "TX ACK: ack_seq=%u status=0x%02X data_len=%u",
             ack_seq, status, data_len);
}

// Process a complete received frame
// NOTE: Called after rx_reset(), so s_rx_data_len is 0.
//       Use hdr->data_len from s_rx_header (not cleared by rx_reset).
//       s_rx_data[] content is also preserved.
static int process_complete_frame(void) {
    s_frame_count++;
    int messages_processed = 0;

    const uart_link_header_t *hdr = (const uart_link_header_t *)s_rx_header;
    uint16_t data_len = hdr->data_len;

    ESP_LOGD(TAG, "Frame OK: seq=%u data_len=%u", hdr->seq, data_len);

    if (data_len == 0) {
        return 0;
    }

    s_data_frame_count++;
    s_ack_required_in_frame = false;

    s_last_ack_seq = hdr->seq;
    s_last_status = UART_LINK_STS_RX_OK;

    send_response(s_last_ack_seq, s_last_status, NULL, 0);

    // Parse multiple COBS messages from data (use hdr->data_len, not s_rx_data_len)
    size_t pos = 0;
    while (pos < data_len) {
        while (pos < data_len && s_rx_data[pos] == 0x00) pos++;
        if (pos >= data_len) break;

        size_t frame_start = pos;
        while (pos < data_len && s_rx_data[pos] != 0x00) pos++;
        size_t frame_len = pos - frame_start;

        if (process_cobs_frame(s_rx_data + frame_start, frame_len) == 0) {
            s_decoded_count++;
            messages_processed++;
        }
    }

    // Dequeue ACK response from message_handler_task (non-blocking).
    // ACKs that are not ready yet will be drained by uart_process() loop.
    ack_queue_item_t ack_item;
    TickType_t ack_wait = 0;
    if (xQueueReceive(s_ack_queue, &ack_item, ack_wait) == pdTRUE) {
        s_last_ack_seq = ack_item.ack_seq;
        s_last_status = ack_item.status;
        send_response(s_last_ack_seq, s_last_status,
                      ack_item.data_len > 0 ? ack_item.data : NULL,
                      ack_item.data_len);
    } else if (!s_ack_required_in_frame) {
        s_last_status = UART_LINK_STS_APP_OK;
        send_response(s_last_ack_seq, s_last_status, NULL, 0);
    }

    return messages_processed;
}

// Feed one byte into RX state machine
static bool rx_feed_byte(uint8_t byte) {
    switch (s_rx_state) {
    case RX_STATE_SYNC:
        if (byte == UART_LINK_SYNC_BYTE) {
            s_rx_state = RX_STATE_HEADER;
            s_rx_pos = 0;
        }
        return false;

    case RX_STATE_HEADER:
        s_rx_header[s_rx_pos++] = byte;
        if (s_rx_pos >= UART_LINK_HEADER_SIZE) {
            const uart_link_header_t *hdr = (const uart_link_header_t *)s_rx_header;
            if (hdr->magic != UART_LINK_MAGIC || hdr->data_len > UART_LINK_MAX_DATA) {
                ESP_LOGW(TAG, "Invalid header: magic=0x%02X data_len=%u",
                         hdr->magic, hdr->data_len);
                rx_reset();
                return false;
            }
            s_rx_data_len = hdr->data_len;
            s_rx_pos = 0;
            if (s_rx_data_len > 0) {
                s_rx_state = RX_STATE_DATA;
            } else {
                s_rx_state = RX_STATE_CRC;
            }
        }
        return false;

    case RX_STATE_DATA:
        s_rx_data[s_rx_pos++] = byte;
        if (s_rx_pos >= s_rx_data_len) {
            s_rx_pos = 0;
            s_rx_state = RX_STATE_CRC;
        }
        return false;

    case RX_STATE_CRC:
        s_rx_crc[s_rx_pos++] = byte;
        if (s_rx_pos >= UART_LINK_CRC_SIZE) {
            // Validate CRC
            size_t crc_data_len = UART_LINK_HEADER_SIZE + s_rx_data_len;
            uint8_t crc_buf[UART_LINK_HEADER_SIZE + UART_LINK_MAX_DATA];
            memcpy(crc_buf, s_rx_header, UART_LINK_HEADER_SIZE);
            if (s_rx_data_len > 0) {
                memcpy(crc_buf + UART_LINK_HEADER_SIZE, s_rx_data, s_rx_data_len);
            }
            uint16_t expected_crc = uart_link_crc16(crc_buf, crc_data_len);
            uint16_t actual_crc = (uint16_t)s_rx_crc[0] | ((uint16_t)s_rx_crc[1] << 8);

            bool valid = (expected_crc == actual_crc);
            rx_reset();

            if (!valid) {
                s_crc_error_count++;
                if (s_crc_error_count <= 10) {
                    ESP_LOGW(TAG, "CRC error #%lu: expected=0x%04X actual=0x%04X",
                             s_crc_error_count, expected_crc, actual_crc);
                }
                // Send CRC error response
                const uart_link_header_t *hdr = (const uart_link_header_t *)s_rx_header;
                send_response(hdr->seq, UART_LINK_STS_CRC_ERR, NULL, 0);
                return false;
            }

            return true;  // Frame ready for processing
        }
        return false;
    }

    rx_reset();
    return false;
}

// Throughput statistics (reset every STATS_INTERVAL_MS)
// Note: s_stats_tx_bytes declared earlier (before send_response)
#define STATS_INTERVAL_MS  5000
static uint32_t s_stats_rx_bytes = 0;
static uint32_t s_stats_frames = 0;
static uint32_t s_stats_last_ms = 0;

static void stats_update_and_print(void) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_stats_last_ms == 0) {
        s_stats_last_ms = now;
        return;
    }
    uint32_t elapsed = now - s_stats_last_ms;
    if (elapsed >= STATS_INTERVAL_MS) {
        uint32_t rx_bps = (s_stats_rx_bytes * 1000) / elapsed;
        uint32_t tx_bps = (s_stats_tx_bytes * 1000) / elapsed;
        uint32_t fps = (s_stats_frames * 1000) / elapsed;
        uint32_t presents = graphics_handler_get_and_reset_present_count();
        uint32_t pps = (presents * 1000) / elapsed;
        ESP_LOGI(TAG, "stats: rx=%lu B/s tx=%lu B/s frames=%lu/s presents=%lu/s crc_err=%lu",
                 rx_bps, tx_bps, fps, pps, s_crc_error_count);
        s_stats_rx_bytes = 0;
        s_stats_tx_bytes = 0;
        s_stats_frames = 0;
        s_stats_last_ms = now;
    }
}

static int uart_process(void) {
    if (!uart_running) {
        return 0;
    }

    // Drain pending app ACKs independent of frame reception.
    // This prevents deadlock where Core waits for ACK but no new frame
    // arrives to trigger dequeue in process_complete_frame().
    ack_queue_item_t ack_item;
    while (xQueueReceive(s_ack_queue, &ack_item, 0) == pdTRUE) {
        send_response(ack_item.ack_seq, ack_item.status,
                      ack_item.data_len > 0 ? ack_item.data : NULL,
                      ack_item.data_len);
    }

    uint8_t buf[128];
    int len = uart_read_bytes(UART_PORT_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10));
    if (len <= 0) {
        stats_update_and_print();
        return 0;
    }

    s_stats_rx_bytes += len;

    int messages_processed = 0;
    for (int i = 0; i < len; i++) {
        if (rx_feed_byte(buf[i])) {
            s_stats_frames++;
            messages_processed += process_complete_frame();
        }
    }

    stats_update_and_print();
    return messages_processed;
}

void uart_slave_print_stats(void) {
    ESP_LOGI(TAG, "stats: frames=%lu data=%lu decoded=%lu crc_err=%lu",
           s_frame_count, s_data_frame_count, s_decoded_count, s_crc_error_count);
}

static int uart_send_ack(uint8_t type, uint8_t seq,
                         const uint8_t *response_data, uint16_t response_len) {
    if (!uart_running) {
        ESP_LOGE(TAG, "Cannot send ACK: UART not running");
        return -1;
    }

    ack_queue_item_t item = {
        .ack_seq = seq,
        .status = UART_LINK_STS_APP_OK,
        .data_len = 0,
    };

    if (response_data && response_len > 0) {
        size_t enc_len = 0;
        int enc_ret = fmrb_link_encode_ack(type, seq, response_data, response_len,
                                           item.data, UART_LINK_MAX_DATA, &enc_len);
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

static int uart_init(MessageBufferHandle_t msg_buffer) {
    if (uart_running) {
        return 0;
    }

    s_msg_buffer = msg_buffer;

    // Create ACK queue
    s_ack_queue = xQueueCreate(2, sizeof(ack_queue_item_t));
    if (!s_ack_queue) {
        ESP_LOGE(TAG, "Failed to create ACK queue");
        return -1;
    }

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 120,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_PORT_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %d", err);
        goto cleanup;
    }

    err = uart_set_pin(UART_PORT_NUM,
                       FMRB_PIN_UART_LINK_TX, FMRB_PIN_UART_LINK_RX,
                       FMRB_PIN_UART_LINK_RTS, FMRB_PIN_UART_LINK_CTS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %d", err);
        goto cleanup;
    }

    err = uart_driver_install(UART_PORT_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %d", err);
        goto cleanup;
    }

    // Flush any stale data from RX buffer (noise during boot)
    uart_flush_input(UART_PORT_NUM);

    rx_reset();
    uart_running = 1;

    ESP_LOGI(TAG, "UART slave initialized - TX:%d RX:%d (%d bps)",
           FMRB_PIN_UART_LINK_TX, FMRB_PIN_UART_LINK_RX, UART_BAUD_RATE);
    return 0;

cleanup:
    if (s_ack_queue) {
        vQueueDelete(s_ack_queue);
        s_ack_queue = NULL;
    }
    return -1;
}

static int uart_is_running(void) {
    return uart_running;
}

static void uart_cleanup(void) {
    if (uart_running) {
        uart_driver_delete(UART_PORT_NUM);
    }

    if (s_ack_queue) {
        vQueueDelete(s_ack_queue);
        s_ack_queue = NULL;
    }

    s_msg_buffer = NULL;
    uart_running = 0;
    ESP_LOGI(TAG, "UART slave communication stopped");
}

static const comm_interface_t uart_comm = {
    .init = uart_init,
    .send = NULL,
    .receive = NULL,
    .process = uart_process,
    .send_ack = uart_send_ack,
    .is_running = uart_is_running,
    .cleanup = uart_cleanup,
};

const comm_interface_t* comm_get_interface(void) {
    return &uart_comm;
}

#endif // !CONFIG_IDF_TARGET_LINUX
