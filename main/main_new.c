#include <stdio.h>
#include <signal.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "display/display_interface.h"
#include "communication/comm_interface.h"
#include "handlers/graphics_handler.h"
#include "handlers/audio_handler.h"
#include "tasks/graphics_task.h"
#include "tasks/comm_task.h"
#include "tasks/audio_task.h"

#ifdef CONFIG_IDF_TARGET_LINUX
#include "handlers/input_handler.h"
#include "handlers/input_socket.h"
#endif

static const char *TAG = "main";
static volatile int g_running = 1;
static volatile int g_display_initialized = 0;

// Signal handler for clean shutdown (Linux only)
#ifdef CONFIG_IDF_TARGET_LINUX
void signal_handler(int sig) {
    printf("\n\nReceived signal %d, shutting down...\n", sig);
    g_running = 0;
}
#endif

// Display initialization callback
// Called by communication layer when INIT_DISPLAY command is received
int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth) {
    ESP_LOGI(TAG, "Initializing display: %dx%d, %d-bit", width, height, color_depth);

    const display_interface_t *display = DISPLAY_INTERFACE;
    if (!display) {
        ESP_LOGE(TAG, "Failed to get display interface");
        return -1;
    }

    // Initialize display
    if (display->init(width, height, color_depth) != 0) {
        ESP_LOGE(TAG, "Display initialization failed");
        return -1;
    }

    // Initialize graphics handler
    void *lgfx = display->get_lgfx();
    if (graphics_handler_init(lgfx) < 0) {
        ESP_LOGE(TAG, "Graphics handler initialization failed");
        display->cleanup();
        return -1;
    }

    // Initialize audio handler
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed");
        graphics_handler_cleanup();
        display->cleanup();
        return -1;
    }

#ifdef CONFIG_IDF_TARGET_LINUX
    // Initialize input handler (Linux only)
    if (input_handler_init() < 0) {
        ESP_LOGE(TAG, "Input handler initialization failed");
        audio_handler_cleanup();
        graphics_handler_cleanup();
        display->cleanup();
        return -1;
    }
#endif

    g_display_initialized = 1;
    ESP_LOGI(TAG, "Display initialization complete");
    return 0;
}

void app_main(void) {
    ESP_LOGI(TAG, "fmruby-graphics-audio starting...");

#ifdef CONFIG_IDF_TARGET_LINUX
    // Setup signal handlers (Linux only)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    // Initialize communication interface
    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        ESP_LOGE(TAG, "Failed to get communication interface");
        return;
    }

    if (comm->init() < 0) {
        ESP_LOGE(TAG, "Communication initialization failed");
        return;
    }

#ifdef CONFIG_IDF_TARGET_LINUX
    // Start input socket server (Linux only)
    if (input_socket_start() < 0) {
        ESP_LOGE(TAG, "Input socket start failed");
        comm->cleanup();
        return;
    }
#endif

    ESP_LOGI(TAG, "Waiting for display initialization command...");

    // Wait for display initialization
    int timeout_count = 0;
    while (!g_display_initialized && g_running) {
        comm->process();
        vTaskDelay(pdMS_TO_TICKS(100));

        timeout_count++;
        if (timeout_count > 600) {  // 60 second timeout
            ESP_LOGE(TAG, "Timeout waiting for display initialization");
#ifdef CONFIG_IDF_TARGET_LINUX
            input_socket_stop();
#endif
            comm->cleanup();
            return;
        }
    }

    if (!g_display_initialized) {
        ESP_LOGE(TAG, "Display not initialized");
#ifdef CONFIG_IDF_TARGET_LINUX
        input_socket_stop();
#endif
        comm->cleanup();
        return;
    }

    ESP_LOGI(TAG, "Starting tasks...");

    // Create communication task (runs on core 0 or 1)
    xTaskCreatePinnedToCore(
        comm_task,
        "comm_task",
        4096,
        NULL,
        5,
        NULL,
        0
    );

    // Create graphics task (runs on core 0, high priority)
    xTaskCreatePinnedToCore(
        graphics_task,
        "graphics_task",
        8192,
        NULL,
        10,
        NULL,
        0
    );

    // Create audio task (runs on core 0, medium priority)
    xTaskCreatePinnedToCore(
        audio_task,
        "audio_task",
        4096,
        NULL,
        7,
        NULL,
        0
    );

    ESP_LOGI(TAG, "All tasks created. System running.");

#ifdef CONFIG_IDF_TARGET_LINUX
    // On Linux, main task monitors for shutdown
    while (g_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Shutting down...");

    // Stop tasks
    graphics_task_stop();
    comm_task_stop();
    audio_task_stop();

    vTaskDelay(pdMS_TO_TICKS(500));

    // Cleanup
    input_handler_cleanup();
    input_socket_stop();
    audio_handler_cleanup();
    graphics_handler_cleanup();

    const display_interface_t *display = DISPLAY_INTERFACE;
    if (display) {
        display->cleanup();
    }

    comm->cleanup();

    ESP_LOGI(TAG, "fmruby-graphics-audio stopped.");
#else
    // On ESP32, let FreeRTOS idle task run
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
}
