#include "graphics_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "display_interface.h"
#include "graphics_handler.h"

extern "C" {
#include "input_handler.h"
#include "input_socket.h"
#include "fmrb_link_protocol.h"
#include "fmrb_gfx.h"
#include "../mempool/fmrb_mempool.h"
}

static const char *TAG = "graphics_task";
static volatile int task_running = 0;

static volatile int display_initialized = 0;
static uint16_t display_width = 480;   // Default values
static uint16_t display_height = 320;

// Callback function called by socket_server when display init message is received
extern "C" int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth) {
    ESP_LOGI(TAG, "Initializing display: %dx%d, %d-bit color", width, height, color_depth);

    display_width = width;
    display_height = height;

    // Initialize display first (Panel_CVBS allocates smaller fragmented blocks)
    // This reduces PSRAM fragmentation when canvas pool allocates large contiguous block
    if (DISPLAY_INTERFACE->init(width, height, color_depth) < 0) {
        ESP_LOGE(TAG, "Display initialization failed");
        return -1;
    }

    // Initialize canvas memory pool with display dimensions (large contiguous block)
    if (fmrb_mempool_canvas_init(width, height, color_depth) != 0) {
        ESP_LOGE(TAG, "Failed to initialize canvas memory pool");
        DISPLAY_INTERFACE->cleanup();
        return -1;
    }

    ESP_LOGI(TAG, "Graphics initialized with LovyanGFX (%dx%d, %d-bit RGB)", width, height, color_depth);

    // Initialize graphics handler (creates back buffer)
    if (graphics_handler_init() < 0) {
        ESP_LOGE(TAG, "Graphics handler initialization failed");
        fmrb_mempool_canvas_deinit();
        DISPLAY_INTERFACE->cleanup();
        return -1;
    }

    // Initialize input handler (Linux/SDL2 only)
#ifdef CONFIG_IDF_TARGET_LINUX
    if (input_handler_init() < 0) {
        ESP_LOGE(TAG, "Input handler initialization failed");
        graphics_handler_cleanup();
        DISPLAY_INTERFACE->cleanup();
        return -1;
    }
#endif

    display_initialized = 1;
    ESP_LOGI(TAG, "Display initialization complete");
    return 0;
}


void graphics_task_stop(void) {
    task_running = 0;
}

void graphics_task(void *pvParameters) {
    task_running = 1;
    ESP_LOGI(TAG, "Graphics task started on core %d", xPortGetCoreID());

#ifdef CONFIG_IDF_TARGET_LINUX
    // Start input socket server (separate from GFX socket)
    if (input_socket_start() < 0) {
        ESP_LOGE(TAG, "Input socket server start failed");
        return;
    }
#endif

    // Wait for display initialization message from comm_task
    // The init_display_callback() will be called by comm_task when the message arrives
    int timeout_count = 0;
    while (!display_initialized && task_running) {
        lgfx::delay(100);
        // comm_task handles communication processing

        timeout_count++;
        if (timeout_count > 60) {  // 6 second timeout
            ESP_LOGE(TAG, "Timeout waiting for display initialization");
            return;
        }
    }
    ESP_LOGI(TAG, "Host server running. Ready to receive commands.");

    // Main loop
    while (task_running) {
        //printf("--main loop------------------------------------.\n");

        // Process display events (e.g., SDL2 window close)
        int display_result = DISPLAY_INTERFACE->process_events();
        if (display_result == 1) {
            // Quit requested
            task_running = 0;
            break;
        }

#ifdef CONFIG_IDF_TARGET_LINUX
        // Process input events (keyboard, mouse)
        int input_result = input_handler_process_events();
        if (input_result == 1) {
            // Quit requested
            task_running = 0;
            break;
        }
#endif

        // Render all canvases to screen in Z-order
        graphics_handler_render_frame();

        // Update display
        DISPLAY_INTERFACE->display();

        // Small delay to prevent busy waiting
        lgfx::delay(16); // ~60 FPS
    }

    ESP_LOGI(TAG, "Shutting down...");

    // Cleanup (reverse order of initialization)
#ifdef CONFIG_IDF_TARGET_LINUX
    input_handler_cleanup();
#endif
    graphics_handler_cleanup();
    fmrb_mempool_canvas_deinit();
    DISPLAY_INTERFACE->cleanup();

    ESP_LOGI(TAG, "Family mruby graphics system stopped.");

    vTaskDelete(NULL);
}
