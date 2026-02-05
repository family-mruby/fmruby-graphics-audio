#include "sdkconfig.h"
#include "display_interface.h"

#ifdef CONFIG_IDF_TARGET_LINUX

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#include <SDL2/SDL.h>

#include <cstdio>
#include "esp_log.h"

static const char *TAG = "display_sdl2";

extern "C" {
#include "fmrb_gfx.h"
}

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

LGFX* g_lgfx = nullptr;
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
    auto lgfx_sdl = new LGFX_SDL(width, height);
    if (!lgfx_sdl) {
        ESP_LOGE(TAG, "Failed to create LovyanGFX instance");
        return -1;
    }

    lgfx_sdl->init();
    lgfx_sdl->setColorDepth(color_depth);
    lgfx_sdl->fillScreen(FMRB_COLOR_BLACK);

    // Disable L/R key rotation shortcut by requiring Ctrl modifier
    auto panel = lgfx_sdl->getPanel_sdl();
    if (panel) {
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));
    }

    // Disable SDL2 hardware cursor (we'll draw our own if needed)
    SDL_ShowCursor(SDL_DISABLE);

    g_lgfx = lgfx_sdl;

    ESP_LOGI(TAG, "SDL2 display initialized: %dx%d, %d-bit", width, height, color_depth);
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
    ESP_LOGI(TAG, "SDL2 display cleaned up");
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
