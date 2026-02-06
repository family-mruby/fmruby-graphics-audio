#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)

#include <SDL2/SDL.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

extern "C" {
#include "display_interface.h"
#include "esp_log.h"
}

static const char *TAG = "display_linux";

// Linux用SDL設定（手動設定 - LGFX_AUTODETECTは使用しない）
class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_sdl* _panel_instance;

    bool init_impl(bool use_reset, bool use_clear) override
    {
        return lgfx::LGFX_Device::init_impl(false, use_clear);
    }

public:
    LGFX(int width = 256, int height = 240, uint_fast8_t scaling_x = 2, uint_fast8_t scaling_y = 2)
        : _panel_instance(nullptr)
    {
        _panel_instance = new lgfx::Panel_sdl();
        auto cfg = _panel_instance->config();
        cfg.memory_width = width;
        cfg.panel_width = width;
        cfg.memory_height = height;
        cfg.panel_height = height;
        _panel_instance->config(cfg);

        if (scaling_x == 0) { scaling_x = 1; }
        if (scaling_y == 0) { scaling_y = scaling_x; }
        _panel_instance->setScaling(scaling_x, scaling_y);

        setPanel(_panel_instance);
        _board = lgfx::board_t::board_SDL;
    }

    ~LGFX()
    {
        // Don't delete _panel_instance here - let Panel_sdl::main() clean up SDL
        // Just detach it
        setPanel(nullptr);
        _panel_instance = nullptr;
    }
};

// Global LGFX instance
LGFX* g_lgfx = nullptr;

// Linux/SDL2 implementation functions
static int linux_display_init(uint16_t width, uint16_t height, uint8_t color_depth) {
    ESP_LOGI(TAG, "Initializing Linux/SDL2 display: %dx%d, %d-bit color", width, height, color_depth);

    // SDL2 must already be initialized by Panel_sdl::main() before this is called
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        ESP_LOGE(TAG, "SDL2 not initialized! Panel_sdl::main() must be called first.");
        return -1;
    }

    // Create LovyanGFX instance with specified resolution
    // Default scaling: 2x for better visibility on modern displays
    g_lgfx = new LGFX(width, height, 2, 2);
    if (!g_lgfx) {
        ESP_LOGE(TAG, "Failed to create LovyanGFX instance");
        return -1;
    }

    // Initialize LGFX (this creates the SDL2 window via Panel_sdl)
    if (!g_lgfx->init()) {
        ESP_LOGE(TAG, "LGFX init() failed");
        delete g_lgfx;
        g_lgfx = nullptr;
        return -1;
    }

    g_lgfx->setColorDepth(color_depth);
    g_lgfx->fillScreen(0x00);  // Black

    // Disable L/R key rotation shortcut by requiring Ctrl modifier
    auto panel = (lgfx::Panel_sdl*)g_lgfx->getPanel();
    if (panel) {
        panel->setShortcutKeymod(static_cast<SDL_Keymod>(KMOD_CTRL));  // Require Ctrl key for L/R rotation
    }

    g_lgfx->setAutoDisplay(false);  // Manual display control

    ESP_LOGI(TAG, "Linux/SDL2 display initialized successfully");
    return 0;
}

static void* linux_display_get_lgfx(void) {
    return (void*)g_lgfx;
}

static int linux_display_process_events(void) {
    // SDL2 event processing is handled by input_handler
    // This function is called to process display-specific events (like window close)
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            ESP_LOGI(TAG, "SDL_QUIT event received");
            return 1;  // Signal quit
        }
        // Other events are handled by input_handler, so push back
        SDL_PushEvent(&event);
        break;  // Only check one event per call to avoid infinite loop
    }
    return 0;  // Continue normally
}

static void linux_display_display(void) {
    if (g_lgfx) {
        g_lgfx->display();
    }
}

static void linux_display_cleanup(void) {
    ESP_LOGI(TAG, "Cleaning up Linux/SDL2 display");
    if (g_lgfx) {
        delete g_lgfx;
        g_lgfx = nullptr;
    }
    ESP_LOGI(TAG, "Linux/SDL2 display cleanup complete");
}

// Display interface structure for Linux/SDL2
static const display_interface_t linux_display_interface = {
    .init = linux_display_init,
    .get_lgfx = linux_display_get_lgfx,
    .process_events = linux_display_process_events,
    .display = linux_display_display,
    .cleanup = linux_display_cleanup,
};

// Get the display interface
extern "C" const display_interface_t* display_get_interface(void) {
    return &linux_display_interface;
}

#endif // CONFIG_IDF_TARGET_LINUX || LGFX_USE_SDL
