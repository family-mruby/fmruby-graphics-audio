#include "comm_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "comm_interface.h"
#include <string.h>

static const char *TAG = "comm_task";
static volatile int task_running = 1;

void comm_task_stop(void) {
    task_running = 0;
}

void comm_task(void *pvParameters) {
    ESP_LOGI(TAG, "started on core %d", (int)xPortGetCoreID());

    MessageBufferHandle_t msg_buffer = (MessageBufferHandle_t)pvParameters;
    if (!msg_buffer) {
        ESP_LOGE(TAG, "Invalid MessageBuffer handle");
        vTaskDelete(NULL);
        return;
    }

    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        ESP_LOGE(TAG, "comm interface is NULL");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Calling comm->init()...");
    int init_ret = comm->init(msg_buffer);
    ESP_LOGI(TAG, "comm->init() returned %d", init_ret);
    if (init_ret < 0) {
        ESP_LOGE(TAG, "comm init failed (%d)", init_ret);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "comm initialized OK. Entering main loop.");

#ifndef CONFIG_IDF_TARGET_LINUX
    extern void spi_slave_print_stats(void);
    int loop_count = 0;
#endif

    // Main communication processing loop
    // Decoded messages are sent directly to MessageBuffer by comm->process()
    while (task_running) {
        int frames_received = comm->process();

        if (frames_received < 0) {
            //ESP_LOGW(TAG, "Communication process error");
        }

#ifndef CONFIG_IDF_TARGET_LINUX
        // Print stats every 5 seconds
        loop_count++;
        if (loop_count % 5000 == 0) {
            spi_slave_print_stats();
        }
#endif

        // Only delay when idle (no frames processed).
        // When busy, loop immediately to re-queue SPI buffers promptly
        // and prevent the slave's transaction queue from being starved.
        if (frames_received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Cleanup communication interface
    comm->cleanup();

    ESP_LOGI(TAG, "Communication task stopped");
    vTaskDelete(NULL);
}
