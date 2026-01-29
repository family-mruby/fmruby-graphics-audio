#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

extern "C" {
#include "comm_interface.h"
#include "graphics_handler.h"
#include "audio_handler.h"
#include "input_handler.h"
#include "input_socket.h"
#include "fmrb_link_protocol.h"
#include "fmrb_gfx.h"
}

// Create a custom LGFX class for SDL2
class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_sdl _panel_instance;
public:
    LGFX(uint16_t width, uint16_t height) {
        auto cfg = _panel_instance.config();
        cfg.memory_width  = width;
        cfg.memory_height = height;
        cfg.panel_width  = width;
        cfg.panel_height = height;
        _panel_instance.config(cfg);
        setPanel(&_panel_instance);
    }

    lgfx::Panel_sdl* getPanel_sdl() { return &_panel_instance; }
};

static volatile int running = 1;
static volatile int display_initialized = 0;
static uint16_t display_width = 480;   // Default values
static uint16_t display_height = 320;
LGFX* g_lgfx = nullptr; // Global LGFX instance (shared with graphics_handler.cpp)

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
    auto panel = g_lgfx->getPanel_sdl();
    if (panel) {
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));
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
    printf("Family mruby Graphics-Audio Host (SDL2 + LovyanGFX) starting...\n");

    // Disable SDL2 hardware cursor (we'll draw our own)
    SDL_ShowCursor(SDL_DISABLE);

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Get communication interface
    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        fprintf(stderr, "Failed to get communication interface\n");
        return 1;
    }

    // Start communication interface (socket server)
    if (comm->init() < 0) {
        fprintf(stderr, "Communication interface init failed\n");
        return 1;
    }

    // Start input socket server (separate from GFX socket)
    if (input_socket_start() < 0) {
        fprintf(stderr, "Input socket server start failed\n");
        comm->cleanup();
        return 1;
    }

    printf("Communication interface started. Waiting for display initialization command...\n");

    // Wait for display initialization message from main
    // The init_display_callback() will be called when the message arrives
    int timeout_count = 0;
    while (!display_initialized && running && *thread_running) {
        comm->process();
        lgfx::delay(100);

        timeout_count++;
        if (timeout_count > 600) {  // 60 second timeout
            fprintf(stderr, "Timeout waiting for display initialization\n");
            comm->cleanup();
            return 1;
        }
    }

    if (!display_initialized) {
        fprintf(stderr, "Display not initialized\n");
        comm->cleanup();
        return 1;
    }

    printf("Host server running. Ready to receive commands.\n");

    // Main loop
    while (running && *thread_running) {
        // Process input events (keyboard, mouse)
        int input_result = input_handler_process_events();
        if (input_result == 1) {
            // Quit requested
            running = 0;
            break;
        }

        // Process communication messages
        comm->process();

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
    comm->cleanup();
    audio_handler_cleanup();
    graphics_handler_cleanup();
    delete g_lgfx;
    g_lgfx = nullptr;

    printf("Family mruby Graphics-Audio Host stopped.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    // Run the user function with LovyanGFX event loop
    input_handler_set_log_level(INPUT_LOG_DEBUG);
    graphics_handler_set_log_level(GFX_LOG_INFO);
    return lgfx::Panel_sdl::main(user_func);
}
