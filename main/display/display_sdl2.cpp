#include "display_interface.h"

#ifdef CONFIG_IDF_TARGET_LINUX

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <SDL2/SDL.h>

#include <cstdio>

extern "C" {
#include "../host/common/fmrb_gfx.h"
}

static LGFX* g_lgfx = nullptr;
static uint16_t g_width = 480;
static uint16_t g_height = 320;
static uint8_t g_color_depth = 16;

extern "C" {

static int sdl2_init(uint16_t width, uint16_t height, uint8_t color_depth) {
    if (g_lgfx) {
        // Already initialized
        return 0;
    }

    g_width = width;
    g_height = height;
    g_color_depth = color_depth;

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
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));
    }

    // Disable SDL2 hardware cursor (we'll draw our own if needed)
    SDL_ShowCursor(SDL_DISABLE);

    printf("SDL2 display initialized: %dx%d, %d-bit\n", width, height, color_depth);
    return 0;
}

static void* sdl2_get_lgfx(void) {
    return (void*)g_lgfx;
}

static int sdl2_process_events(void) {
    if (!g_lgfx) {
        return -1;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            return 1; // Quit requested
        }
        // TODO: Handle other events (keyboard, mouse) if needed
    }

    return 0;
}

static void sdl2_display(void) {
    if (g_lgfx) {
        g_lgfx->display();
    }
}

static void sdl2_cleanup(void) {
    if (g_lgfx) {
        delete g_lgfx;
        g_lgfx = nullptr;
    }
    printf("SDL2 display cleaned up\n");
}

static const display_interface_t sdl2_display_impl = {
    .init = sdl2_init,
    .get_lgfx = sdl2_get_lgfx,
    .process_events = sdl2_process_events,
    .display = sdl2_display,
    .cleanup = sdl2_cleanup,
};

const display_interface_t* display_get_interface(void) {
    return &sdl2_display_impl;
}

} // extern "C"

#endif // CONFIG_IDF_TARGET_LINUX
