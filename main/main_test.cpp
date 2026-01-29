#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_LINUX

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <SDL2/SDL.h>

#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Create a custom LGFX class for SDL2
class LGFX_SDL : public lgfx::LGFX_Device
{
    lgfx::Panel_sdl _panel_instance;
public:
    LGFX_SDL(uint16_t width, uint16_t height) {
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

extern "C" void app_main(void)
{
    printf("========================================\n");
    printf("LovyanGFX Panel_sdl Minimal Test\n");
    printf("========================================\n");

    // Create LovyanGFX instance with SDL2 backend
    printf("Creating LGFX instance (480x320)...\n");
    auto lgfx = new LGFX_SDL(480, 320);
    if (!lgfx) {
        fprintf(stderr, "Failed to create LovyanGFX instance\n");
        return;
    }

    // Initialize SDL2 explicitly first
    printf("Initializing SDL2...\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        delete lgfx;
        return;
    }
    printf("SDL2 initialized\n");

    // Initialize LovyanGFX
    printf("Initializing LovyanGFX...\n");
    lgfx->init();
    lgfx->setColorDepth(16);

    // Disable L/R key rotation shortcut by requiring Ctrl modifier
    auto panel = lgfx->getPanel_sdl();
    if (panel) {
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));
    }

    // Disable SDL2 hardware cursor
    SDL_ShowCursor(SDL_DISABLE);

    printf("LovyanGFX initialized successfully\n");

    // Fill screen with blue color
    printf("Filling screen with blue...\n");
    lgfx->fillScreen(0x001F);  // Blue (RGB565: 0b0000000000011111)
    lgfx->display();

    printf("\n========================================\n");
    printf("Blue screen should be displayed now!\n");
    printf("Press any key or close window to exit\n");
    printf("========================================\n\n");

    // Event loop
    bool running = true;
    int frame_count = 0;

    while (running) {
        // Process SDL2 events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                printf("SDL_QUIT event received\n");
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                printf("Key pressed, exiting...\n");
                running = false;
            }
        }

        // Update display (in case of any internal buffering)
        lgfx->display();

        // Frame counter for debugging
        frame_count++;
        if (frame_count % 60 == 0) {
            printf("Running... (frame %d)\n", frame_count);
        }

        // Small delay for ~60 FPS
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    printf("\n========================================\n");
    printf("Test completed successfully!\n");
    printf("========================================\n");

    // Cleanup
    delete lgfx;
    SDL_Quit();
}

#else
// For ESP32 builds, provide a dummy app_main
extern "C" void app_main(void)
{
    printf("main_test.cpp: This test is only for Linux target\n");
}
#endif
