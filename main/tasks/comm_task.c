#include "comm_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "../communication/comm_interface.h"

static const char *TAG = "comm_task";
static volatile int task_running = 1;

void comm_task_stop(void) {
    task_running = 0;
}

void comm_task(void *pvParameters) {
    ESP_LOGI(TAG, "Communication task started on core %d", xPortGetCoreID());

    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        ESP_LOGE(TAG, "Failed to get communication interface");
        vTaskDelete(NULL);
        return;
    }

    // Main communication processing loop
    while (task_running) {
        // Process incoming messages
        int result = comm->process();

        if (result < 0) {
            ESP_LOGW(TAG, "Communication process error");
        }

        // Small delay to prevent busy waiting
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "Communication task stopped");
    vTaskDelete(NULL);
}
