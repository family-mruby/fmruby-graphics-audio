#include "audio_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_handler.h"

static const char *TAG = "audio_task";
static volatile int task_running = 1;

void audio_task_stop(void) {
    task_running = 0;
}

void audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());

#ifndef CONFIG_IDF_TARGET_LINUX
    audio_check_impl();
    return;
#endif

    // Initialize audio handler
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed\n");
        return;
    }


    // Main audio processing loop
    while (task_running) {
        // Audio processing is event-driven through audio_handler
        // This task just keeps running for future audio streaming support

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    audio_handler_cleanup();

    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}
