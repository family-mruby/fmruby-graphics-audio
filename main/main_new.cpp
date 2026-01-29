#include <cstdio>
#include <csignal>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#ifdef CONFIG_IDF_TARGET_LINUX
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <SDL2/SDL.h>
#endif

extern "C" {
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
}

static const char *TAG = "main";
static volatile int g_running = 1;
static volatile int g_display_initialized = 0;

// Signal handler for clean shutdown (Linux only)
#ifdef CONFIG_IDF_TARGET_LINUX
extern "C" void signal_handler(int sig) {
    printf("\n\nReceived signal %d, shutting down...\n", sig);
    g_running = 0;

    // Post SDL_QUIT event to stop LovyanGFX event loop
    SDL_Event quit_event;
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
}
#endif

// Display initialization callback
// Called by communication layer when INIT_DISPLAY command is received
extern "C" int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth) {
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
    if (graphics_handler_init() < 0) {
        ESP_LOGE(TAG, "Graphics handler initialization failed");
        display->cleanup();
        return -1;
    }

#ifndef CONFIG_IDF_TARGET_LINUX
    // Initialize audio handler
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed");
        graphics_handler_cleanup();
        display->cleanup();
        return -1;
    }
#endif

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

#ifdef CONFIG_IDF_TARGET_LINUX
// Linux version of app_main - integrates Panel_sdl event loop
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "fmruby-graphics-audio starting (SDL2 mode)...");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Setup SDL2
    lgfx::Panel_sdl::setup();

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

    // Start input socket server
    if (input_socket_start() < 0) {
        ESP_LOGE(TAG, "Input socket start failed");
        comm->cleanup();
        return;
    }

    ESP_LOGI(TAG, "Waiting for display initialization command...");

    // Wait for display initialization
    int timeout_count = 0;
    while (!g_display_initialized && g_running) {
        comm->process();
        lgfx::delay(100);

        timeout_count++;
        if (timeout_count > 600) {  // 60 second timeout
            ESP_LOGE(TAG, "Timeout waiting for display initialization");
            input_socket_stop();
            comm->cleanup();
            return;
        }
    }

    if (!g_display_initialized) {
        ESP_LOGE(TAG, "Display not initialized");
        input_socket_stop();
        comm->cleanup();
        return;
    }

    ESP_LOGI(TAG, "Host server running. Ready to receive commands.");

    // Get display interface for g_lgfx access
    const display_interface_t *display = DISPLAY_INTERFACE;
    extern LGFX* g_lgfx;  // Defined in display_sdl2.cpp

    // Main loop with Panel_sdl event processing
    while (g_running) {
        // Process SDL2 events and update (creates window if needed)
        int sdl_result = lgfx::Panel_sdl::loop();
        if (sdl_result != 0) {
            // Window closed
            g_running = 0;
            break;
        }

        // Process input events (keyboard, mouse)
        int input_result = input_handler_process_events();
        if (input_result == 1) {
            // Quit requested
            g_running = 0;
            break;
        }

        // Process socket messages
        comm->process();

        // Render all canvases to screen in Z-order
        graphics_handler_render_frame();

        // Update display
        if (g_lgfx) {
            g_lgfx->display();
        }

        // Frame rate control (~60 FPS)
        lgfx::delay(16);
    }

    ESP_LOGI(TAG, "Shutting down...");

    // Cleanup
    input_handler_cleanup();
    input_socket_stop();
    audio_handler_cleanup();
    graphics_handler_cleanup();

    if (display) {
        display->cleanup();
    }

    comm->cleanup();

    // Cleanup SDL2
    lgfx::Panel_sdl::close();

    ESP_LOGI(TAG, "fmruby-graphics-audio stopped.");
}
#else
// ESP32 entry point - use FreeRTOS app_main()
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "fmruby-graphics-audio starting...");

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

    ESP_LOGI(TAG, "Waiting for display initialization command...");

    // Wait for display initialization
    int timeout_count = 0;
    while (!g_display_initialized && g_running) {
        comm->process();
        vTaskDelay(pdMS_TO_TICKS(100));

        timeout_count++;
        if (timeout_count > 600) {  // 60 second timeout
            ESP_LOGE(TAG, "Timeout waiting for display initialization");
            comm->cleanup();
            return;
        }
    }

    if (!g_display_initialized) {
        ESP_LOGE(TAG, "Display not initialized");
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

    // On ESP32, let FreeRTOS idle task run
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif
