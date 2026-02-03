#include "graphics_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display_interface.h"
#include "graphics_handler.h"

extern "C" {
#include "socket_server.h"
#include "graphics_handler.h"
#include "audio_handler.h"
#include "input_handler.h"
#include "input_socket.h"
#include "fmrb_link_protocol.h"
#include "fmrb_gfx.h"
}

#ifdef CONFIG_IDF_TARGET_LINUX
#include "lgfx_linux.h"
#include <SDL2/SDL.h>
#else
#endif

static const char *TAG = "graphics_task";
static volatile int task_running = 0;

static volatile int display_initialized = 0;
static uint16_t display_width = 480;   // Default values
static uint16_t display_height = 320;
LGFX* g_lgfx = nullptr; // Global LGFX instance (not static, shared with graphics_handler.cpp)


// Callback function called by socket_server when display init message is received
extern "C" int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth) {
    printf("Initializing display: %dx%d, %d-bit color\n", width, height, color_depth);

    display_width = width;
    display_height = height;

    // Create LovyanGFX instance with specified resolution
    g_lgfx = new LGFX(width, height);
    if (!g_lgfx) {
        fprintf(stderr, "Failed to create LovyanGFX instance\n");
        return -1;
    }

    g_lgfx->init();
    g_lgfx->setColorDepth(color_depth);
    g_lgfx->fillScreen(FMRB_COLOR_BLACK);

    // Disable L/R key rotation shortcut by requiring Ctrl modifier
#ifdef CONFIG_IDF_TARGET_LINUX
    auto panel = (lgfx::Panel_sdl*)g_lgfx->getPanel();
    if (panel) {
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));  // Require Ctrl key for L/R rotation
    }
#endif

    ESP_LOGI(TAG, "Graphics initialized with LovyanGFX (%dx%d, %d-bit RGB)\n", width, height, color_depth);

    // Initialize graphics handler (creates back buffer)
    if (graphics_handler_init() < 0) {
        ESP_LOGE(TAG, "Graphics handler initialization failed\n");
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }


    // Initialize input handler
    if (input_handler_init() < 0) {
        ESP_LOGE(TAG, "Input handler initialization failed\n");
        audio_handler_cleanup();
        graphics_handler_cleanup();
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }

    display_initialized = 1;
    ESP_LOGI(TAG, "Display initialization complete\n");
    return 0;
}


void graphics_task_stop(void) {
    task_running = 0;
}

void graphics_task(void *pvParameters) {
    task_running = 1;
    ESP_LOGI(TAG, "Graphics task started on core %d", xPortGetCoreID());

    // Start socket server FIRST (before creating LGFX)
    // This allows us to receive display initialization command from main
    if (socket_server_start() < 0) {
        fprintf(stderr, "Socket server start failed\n");
        return;
    }

    // Start input socket server (separate from GFX socket)
    if (input_socket_start() < 0) {
        fprintf(stderr, "Input socket server start failed\n");
        socket_server_stop();
        return;
    }

    printf("Socket server started. Waiting for display initialization command...\n");

    // Wait for display initialization message from main
    // The init_display_callback() will be called when the message arrives
    int timeout_count = 0;
    while (!display_initialized && task_running) {
        socket_server_process();
        lgfx::delay(100);

        timeout_count++;
        if (timeout_count > 120*10) {  // 6 second timeout
            fprintf(stderr, "Timeout waiting for display initialization\n");
            socket_server_stop();
            return;
        }
    }

    if (!display_initialized) {
        fprintf(stderr, "Display not initialized\n");
        socket_server_stop();
        return;
    }

    printf("Host server running. Ready to receive commands.\n");

    // Main loop
    while (task_running) {
        //printf("--main loop------------------------------------.\n");

        // Process input events (keyboard, mouse)
        int input_result = input_handler_process_events();
        if (input_result == 1) {
            // Quit requested
            task_running = 0;
            break;
        }

        // Process socket messages
        socket_server_process();

        // Render all canvases to screen in Z-order
        graphics_handler_render_frame();

        // Update display
        g_lgfx->display();

        // Small delay to prevent busy waiting
        lgfx::delay(16); // ~60 FPS
    }

    printf("Shutting down...\n");

    // Cleanup
    input_handler_cleanup();
    input_socket_stop();
    socket_server_stop();
    graphics_handler_cleanup();
    delete g_lgfx;
    g_lgfx = nullptr;

    printf("Family mruby Host (SDL2 + LovyanGFX) stopped.\n");

    vTaskDelete(NULL);
}
