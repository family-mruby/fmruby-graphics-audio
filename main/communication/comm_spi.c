#include "comm_interface.h"

#ifndef CONFIG_IDF_TARGET_LINUX

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// TODO: Configure these pins for your ESP32 hardware
#define SPI_HOST_ID      SPI2_HOST
#define PIN_NUM_MISO     12
#define PIN_NUM_MOSI     13
#define PIN_NUM_CLK      14
#define PIN_NUM_CS       15

static int spi_running = 0;
static spi_device_handle_t spi_handle = NULL;

// Implementation of comm_interface_t methods

static int spi_init(void) {
    if (spi_running) {
        return 0;
    }

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    // Configure SPI device
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,  // 10 MHz
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };

    // Initialize SPI bus
    esp_err_t ret = spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        printf("SPI bus initialization failed: %d\n", ret);
        return -1;
    }

    // Attach device to SPI bus
    ret = spi_bus_add_device(SPI_HOST_ID, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        printf("SPI device add failed: %d\n", ret);
        spi_bus_free(SPI_HOST_ID);
        return -1;
    }

    spi_running = 1;
    printf("SPI communication initialized\n");
    return 0;
}

static int spi_send(const uint8_t *data, size_t len) {
    if (!spi_running || !spi_handle) {
        return -1;
    }

    spi_transaction_t trans = {
        .length = len * 8,  // Length in bits
        .tx_buffer = data,
        .rx_buffer = NULL,
    };

    esp_err_t ret = spi_device_transmit(spi_handle, &trans);
    if (ret != ESP_OK) {
        return -1;
    }

    return len;
}

static int spi_receive(uint8_t *buf, size_t buf_size) {
    if (!spi_running || !spi_handle) {
        return 0;
    }

    // TODO: Implement SPI receive
    // This will depend on your specific hardware protocol
    // For now, return 0 (no data available)
    return 0;
}

static int spi_process(void) {
    if (!spi_running) {
        return 0;
    }

    // TODO: Implement SPI message processing
    // This should handle incoming SPI data similar to socket_process()
    return 0;
}

static int spi_send_ack(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len) {
    // TODO: Implement SPI ACK send
    // For now, just log
    printf("SPI ACK: type=%u seq=%u len=%u (not implemented)\n", type, seq, response_len);
    return 0;
}

static int spi_is_running(void) {
    return spi_running;
}

static void spi_cleanup(void) {
    if (spi_handle) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }

    if (spi_running) {
        spi_bus_free(SPI_HOST_ID);
    }

    spi_running = 0;
    printf("SPI communication stopped\n");
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
