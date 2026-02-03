#include "lgfx_linux.h"  // Must be first - defines LGFX class for Linux/SDL

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

extern "C" {
#include "socket_server.h"
#include "graphics_handler.h"
#include "audio_handler.h"
#include "input_handler.h"
#include "input_socket.h"
#include "fmrb_link_protocol.h"
#include "fmrb_gfx.h"
}

static volatile int running = 1;
static volatile int display_initialized = 0;
static uint16_t display_width = 480;   // Default values
static uint16_t display_height = 320;
LGFX* g_lgfx = nullptr; // Global LGFX instance (not static, shared with graphics_handler.cpp)

extern "C" void signal_handler(int sig) {
    printf("\n\n\n+++++++++++++++++++++++++++++++++++++++");
    printf("\n+++++++++++++++++++++++++++++++++++++++\n");
    printf("Received signal %d, shutting down...\n", sig);
    running = 0;

    // Post SDL_QUIT event to stop LovyanGFX event loop
    SDL_Event quit_event;
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
}

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
    auto panel = (lgfx::Panel_sdl*)g_lgfx->getPanel();
    if (panel) {
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));  // Require Ctrl key for L/R rotation
    }

    printf("Graphics initialized with LovyanGFX (%dx%d, %d-bit RGB)\n", width, height, color_depth);

    // Initialize graphics handler (creates back buffer)
    if (graphics_handler_init() < 0) {
        fprintf(stderr, "Graphics handler initialization failed\n");
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }

    // Initialize audio handler
    if (audio_handler_init() < 0) {
        fprintf(stderr, "Audio handler initialization failed\n");
        graphics_handler_cleanup();
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }

    // Initialize input handler
    if (input_handler_init() < 0) {
        fprintf(stderr, "Input handler initialization failed\n");
        audio_handler_cleanup();
        graphics_handler_cleanup();
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }

    display_initialized = 1;
    printf("Display initialization complete\n");
    return 0;
}

// User function that runs in a separate thread
int user_func(bool* thread_running) {
    printf("Family mruby Host (SDL2 + LovyanGFX) starting...\n");

    // Disable SDL2 hardware cursor (we'll draw our own)
    SDL_ShowCursor(SDL_DISABLE);

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Start socket server FIRST (before creating LGFX)
    // This allows us to receive display initialization command from main
    if (socket_server_start() < 0) {
        fprintf(stderr, "Socket server start failed\n");
        return 1;
    }

    // Start input socket server (separate from GFX socket)
    if (input_socket_start() < 0) {
        fprintf(stderr, "Input socket server start failed\n");
        socket_server_stop();
        return 1;
    }

    printf("Socket server started. Waiting for display initialization command...\n");

    // Wait for display initialization message from main
    // The init_display_callback() will be called when the message arrives
    int timeout_count = 0;
    while (!display_initialized && running && *thread_running) {
        socket_server_process();
        lgfx::delay(100);

        timeout_count++;
        if (timeout_count > 120*10) {  // 6 second timeout
            fprintf(stderr, "Timeout waiting for display initialization\n");
            socket_server_stop();
            return 1;
        }
    }

    if (!display_initialized) {
        fprintf(stderr, "Display not initialized\n");
        socket_server_stop();
        return 1;
    }

    printf("Host server running. Ready to receive commands.\n");

    // Main loop
    while (running && *thread_running) {
        //printf("--main loop------------------------------------.\n");

        // Process input events (keyboard, mouse)
        int input_result = input_handler_process_events();
        if (input_result == 1) {
            // Quit requested
            running = 0;
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
    audio_handler_cleanup();
    graphics_handler_cleanup();
    delete g_lgfx;
    g_lgfx = nullptr;

    printf("Family mruby Host (SDL2 + LovyanGFX) stopped.\n");
    return 0;
}

extern "C" int app_main(void)
{
    // Run the user function with LovyanGFX event loop
    return lgfx::Panel_sdl::main(user_func);
}



// #include <stdio.h>
// #include <stdlib.h>
// #include "sdkconfig.h"
// #define LGFX_USE_V1
// #include <LovyanGFX.hpp>
// #include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

// // Linux向けSDL設定（手動設定 - LGFX_AUTODETECTは使用しない）
// class LGFX : public lgfx::LGFX_Device
// {
//     lgfx::Panel_sdl* _panel_instance;

//     bool init_impl(bool use_reset, bool use_clear) override
//     {
//         return lgfx::LGFX_Device::init_impl(false, use_clear);
//     }

// public:
//     LGFX(int width = 256, int height = 240, uint_fast8_t scaling_x = 2, uint_fast8_t scaling_y = 2)
//         : _panel_instance(nullptr)
//     {
//         _panel_instance = new lgfx::Panel_sdl();
//         auto cfg = _panel_instance->config();
//         cfg.memory_width = width;
//         cfg.panel_width = width;
//         cfg.memory_height = height;
//         cfg.panel_height = height;
//         _panel_instance->config(cfg);

//         if (scaling_x == 0) { scaling_x = 1; }
//         if (scaling_y == 0) { scaling_y = scaling_x; }
//         _panel_instance->setScaling(scaling_x, scaling_y);

//         setPanel(_panel_instance);
//         _board = lgfx::board_t::board_SDL;
//     }

//     ~LGFX()
//     {
//         // Don't delete _panel_instance here - let Panel_sdl::main() clean up SDL
//         // Just detach it
//         setPanel(nullptr);
//         _panel_instance = nullptr;
//     }
// };

// static LGFX* lcd = nullptr;
// static bool setup_done = false;

// void setup(void)
// {
//     printf("=== LovyanGFX Test (main_linux.cpp) ===\n");
//     printf("DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "not set");
//     printf("WAYLAND_DISPLAY=%s\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "not set");

//     // Allocate LCD object dynamically to avoid stack overflow
//     printf("Allocating LGFX object (size: %zu bytes)...\n", sizeof(LGFX));
//     lcd = new LGFX();
//     printf("LGFX object allocated at: %p\n", (void*)lcd);

//     printf("Initializing display...\n");
//     lcd->init();

//     printf("Setting rotation...\n");
//     lcd->setRotation(0);

//     printf("Filling screen with black...\n");
//     lcd->fillScreen(TFT_BLACK);

//     printf("Drawing test patterns...\n");

//     // Color bars
//     int barWidth = lcd->width() / 7;
//     lcd->fillRect(barWidth * 0, 0, barWidth, 60, TFT_RED);
//     lcd->fillRect(barWidth * 1, 0, barWidth, 60, TFT_GREEN);
//     lcd->fillRect(barWidth * 2, 0, barWidth, 60, TFT_BLUE);
//     lcd->fillRect(barWidth * 3, 0, barWidth, 60, TFT_YELLOW);
//     lcd->fillRect(barWidth * 4, 0, barWidth, 60, TFT_CYAN);
//     lcd->fillRect(barWidth * 5, 0, barWidth, 60, TFT_MAGENTA);
//     lcd->fillRect(barWidth * 6, 0, barWidth, 60, TFT_WHITE);

//     // Text drawing
//     lcd->setTextSize(2);
//     lcd->setTextColor(TFT_WHITE, TFT_BLACK);
//     lcd->setCursor(10, 80);
//     lcd->print("LovyanGFX Test");

//     lcd->setCursor(10, 110);
//     lcd->setTextColor(TFT_GREEN, TFT_BLACK);
//     lcd->print("Display OK!");

//     // Circle drawing
//     lcd->drawCircle(128, 180, 40, TFT_YELLOW);
//     lcd->fillCircle(128, 180, 35, TFT_BLUE);

//     printf("Test pattern complete!\n");
//     printf("Display size: %d x %d\n", lcd->width(), lcd->height());
//     printf("Press Ctrl+C to exit\n");

//     setup_done = true;
// }

// void loop(void)
// {
//     // Update display
//     if (lcd) {
//         lcd->display();
//     }
//     // Add small delay to reduce CPU usage
//     lgfx::delay(16); // ~60fps
// }

// int user_func(bool* running)
// {
//     setup();
//     while (*running) {
//         loop();
//     }

//     // Cleanup
//     printf("Cleaning up...\n");
//     if (lcd) {
//         delete lcd;
//         lcd = nullptr;
//     }

//     return 0;
// }

// extern "C" int app_main(void)
// {
//     return lgfx::Panel_sdl::main(user_func);
// }
