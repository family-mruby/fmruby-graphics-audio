#include "graphics_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "../display/display_interface.h"
#include "../handlers/graphics_handler.h"

static const char *TAG = "graphics_task";
static volatile int task_running = 1;

void graphics_task_stop(void) {
    task_running = 0;
}

void graphics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Graphics task started on core %d", xPortGetCoreID());

    const display_interface_t *display = DISPLAY_INTERFACE;
    if (!display) {
        ESP_LOGE(TAG, "Failed to get display interface");
        vTaskDelete(NULL);
        return;
    }

    // Main rendering loop
    while (task_running) {
#ifdef CONFIG_IDF_TARGET_LINUX
        // Process SDL2 events on Linux
        int event_result = display->process_events();
        if (event_result == 1) {
            // Quit requested
            ESP_LOGI(TAG, "Quit requested");
            task_running = 0;
            break;
        }
#endif

        // Render all canvases to screen in Z-order
        graphics_handler_render_frame();

        // Update display
        display->display();

        // Frame rate control (~60 FPS)
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    ESP_LOGI(TAG, "Graphics task stopped");
    vTaskDelete(NULL);
}
