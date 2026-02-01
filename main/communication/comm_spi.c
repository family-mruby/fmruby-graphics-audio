#include "comm_interface.h"

#ifndef CONFIG_IDF_TARGET_LINUX

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"

// SPI Slave pin configuration - must match master's configuration
#define SPI_HOST_ID      SPI2_HOST
#define PIN_NUM_MISO     18   // Slave Output (to Master Input)
#define PIN_NUM_MOSI     21   // Slave Input (from Master Output)
#define PIN_NUM_CLK      19
#define PIN_NUM_CS       22

#define SPI_BUFFER_SIZE  128

// DMA-capable buffers (must be in DMA-capable memory)
WORD_ALIGNED_ATTR static uint8_t rx_buffer[SPI_BUFFER_SIZE];
WORD_ALIGNED_ATTR static uint8_t tx_buffer[SPI_BUFFER_SIZE];

static int spi_running = 0;
static SemaphoreHandle_t spi_mutex = NULL;

// Callback called after a transaction is done
static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *trans)
{
    // This is called from ISR context
    // Can be used to signal task that data is ready
}

// Callback called before a transaction starts
static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t *trans)
{
    // This is called from ISR context
}

static int spi_init(void) {
    if (spi_running) {
        return 0;
    }

    // Create mutex for thread safety
    spi_mutex = xSemaphoreCreateMutex();
    if (!spi_mutex) {
        printf("SPI: Failed to create mutex\n");
        return -1;
    }

    // Configure SPI bus for slave mode
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUFFER_SIZE,
    };

    // Configure SPI slave interface
    spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 3,
        .flags = 0,
        .post_setup_cb = spi_post_setup_cb,
        .post_trans_cb = spi_post_trans_cb,
    };

    // Enable pull-ups on SPI lines for stability
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);

    // Initialize SPI slave interface
    esp_err_t ret = spi_slave_initialize(SPI_HOST_ID, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        printf("SPI slave initialization failed: %d\n", ret);
        vSemaphoreDelete(spi_mutex);
        spi_mutex = NULL;
        return -1;
    }

    // Clear buffers
    memset(rx_buffer, 0, SPI_BUFFER_SIZE);
    memset(tx_buffer, 0, SPI_BUFFER_SIZE);

    spi_running = 1;
    printf("SPI slave initialized - MOSI:%d MISO:%d CLK:%d CS:%d\n",
           PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS);
    return 0;
}

static int spi_send(const uint8_t *data, size_t len) {
    if (!spi_running || !data || len == 0) {
        return -1;
    }

    if (len > SPI_BUFFER_SIZE) {
        len = SPI_BUFFER_SIZE;
    }

    if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }

    // Copy data to TX buffer for next transaction
    memcpy(tx_buffer, data, len);

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

    // Copy received data from RX buffer
    size_t copy_len = (buf_size < SPI_BUFFER_SIZE) ? buf_size : SPI_BUFFER_SIZE;
    memcpy(buf, rx_buffer, copy_len);

    xSemaphoreGive(spi_mutex);
    return copy_len;
}

static int spi_process(void) {
    if (!spi_running) {
        return 0;
    }

    spi_slave_transaction_t trans = {
        .length = SPI_BUFFER_SIZE * 8,  // Length in bits
        .tx_buffer = tx_buffer,
        .rx_buffer = rx_buffer,
    };

    // Queue transaction and wait for master
    // Use a short timeout to allow checking for stop condition
    esp_err_t ret = spi_slave_transmit(SPI_HOST_ID, &trans, pdMS_TO_TICKS(100));

    if (ret == ESP_OK) {
        size_t rx_len = trans.trans_len / 8;
        if (rx_len > 0) {
            printf("SPI Slave: Received %d bytes: ", (int)rx_len);
            for (int i = 0; i < rx_len && i < 16; i++) {
                printf("%02X ", rx_buffer[i]);
            }
            printf("\n");
            return 1;  // Processed one transaction
        }
    } else if (ret != ESP_ERR_TIMEOUT) {
        printf("SPI slave transmit error: %d\n", ret);
        return -1;
    }

    return 0;  // No data (timeout)
}

static int spi_send_ack(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len) {
    if (!spi_running) {
        return -1;
    }

    // Prepare ACK response in TX buffer for next transaction
    if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }

    tx_buffer[0] = 0xAC;  // ACK marker
    tx_buffer[1] = type;
    tx_buffer[2] = seq;

    if (response_data && response_len > 0) {
        size_t copy_len = (response_len < SPI_BUFFER_SIZE - 3) ? response_len : SPI_BUFFER_SIZE - 3;
        memcpy(&tx_buffer[3], response_data, copy_len);
    }

    xSemaphoreGive(spi_mutex);

    printf("SPI ACK prepared: type=%u seq=%u len=%u\n", type, seq, response_len);
    return 0;
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

    spi_running = 0;
    printf("SPI slave communication stopped\n");
}

static const comm_interface_t spi_comm = {
    .init = spi_init,
    .send = spi_send,
    .receive = spi_receive,
    .process = spi_process,
    .send_ack = spi_send_ack,
    .is_running = spi_is_running,
    .cleanup = spi_cleanup,
};

const comm_interface_t* comm_get_interface(void) {
    return &spi_comm;
}

#endif // !CONFIG_IDF_TARGET_LINUX
