#include "graphics_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display_interface.h"
#include "graphics_handler.h"

extern "C" {
#include "graphics_handler.h"
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
#ifdef CONFIG_IDF_TARGET_LINUX
    if (input_handler_init() < 0) {
        ESP_LOGE(TAG, "Input handler initialization failed\n");
        graphics_handler_cleanup();
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }
#endif

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

#ifdef CONFIG_IDF_TARGET_LINUX
    // Start input socket server (separate from GFX socket)
    if (input_socket_start() < 0) {
        fprintf(stderr, "Input socket server start failed\n");
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
    printf("Host server running. Ready to receive commands.\n");

    // Main loop
    while (task_running) {
        //printf("--main loop------------------------------------.\n");

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
        g_lgfx->display();

        // Small delay to prevent busy waiting
        lgfx::delay(16); // ~60 FPS
    }

    printf("Shutting down...\n");

    // Cleanup
#ifdef CONFIG_IDF_TARGET_LINUX
    input_handler_cleanup();
#endif
    graphics_handler_cleanup();
    delete g_lgfx;
    g_lgfx = nullptr;

    printf("Family mruby Host (SDL2 + LovyanGFX) stopped.\n");

    vTaskDelete(NULL);
}
