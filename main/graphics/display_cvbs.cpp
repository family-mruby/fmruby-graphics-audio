#ifndef CONFIG_IDF_TARGET_LINUX

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32/Panel_CVBS.hpp>

extern "C" {
#include "display_interface.h"
#include "esp_log.h"
}

static const char *TAG = "display_esp32";

// LGFX class for ESP32 with CVBS output
class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Panel_CVBS _panel_instance;

  LGFX(uint16_t width, uint16_t height)
  {
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = width;
      cfg.memory_height = height;
      cfg.panel_width  = width - 32;
      cfg.panel_height = height - 32;
      cfg.offset_x = 16;
      cfg.offset_y = 16;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config_detail();
      cfg.signal_type = cfg.signal_type_t::NTSC_J;
      cfg.pin_dac = 25;
      cfg.use_psram = 1;
      cfg.output_level = 128;
      _panel_instance.config_detail(cfg);
    }

    setPanel(&_panel_instance);
  }
};

// Global LGFX instance (as base class pointer for compatibility)
lgfx::LGFX_Device* g_lgfx = nullptr;

// ESP32/CVBS implementation functions
static int esp32_display_init(uint16_t width, uint16_t height, uint8_t color_depth) {
    ESP_LOGI(TAG, "Initializing ESP32/CVBS display: %dx%d, %d-bit color", width, height, color_depth);

    // Create LovyanGFX instance with CVBS output
    LGFX* lgfx_instance = new LGFX(width, height);
    if (!lgfx_instance) {
        ESP_LOGE(TAG, "Failed to create LovyanGFX instance");
        return -1;
    }

    lgfx_instance->init();
    lgfx_instance->setColorDepth(color_depth);
    lgfx_instance->fillScreen(0x00);  // Black

    lgfx_instance->setAutoDisplay(false);  // Manual display control

    // Store as base class pointer
    g_lgfx = lgfx_instance;

    ESP_LOGI(TAG, "ESP32/CVBS display initialized successfully");
    return 0;
}

static void* esp32_display_get_lgfx(void) {
    return (void*)g_lgfx;
}

static int esp32_display_process_events(void) {
    // ESP32 doesn't have window events like SDL2
    // This is a no-op for ESP32
    return 0;  // Continue normally
}

static void esp32_display_display(void) {
    if (g_lgfx) {
        g_lgfx->display();
    }
}

static void esp32_display_cleanup(void) {
    ESP_LOGI(TAG, "Cleaning up ESP32/CVBS display");
    if (g_lgfx) {
        delete g_lgfx;
        g_lgfx = nullptr;
    }
    ESP_LOGI(TAG, "ESP32/CVBS display cleanup complete");
}

// Display interface structure for ESP32/CVBS
static const display_interface_t esp32_display_interface = {
    .init = esp32_display_init,
    .get_lgfx = esp32_display_get_lgfx,
    .process_events = esp32_display_process_events,
    .display = esp32_display_display,
    .cleanup = esp32_display_cleanup,
};

// Get the display interface
extern "C" const display_interface_t* display_get_interface(void) {
    return &esp32_display_interface;
}

#endif // !CONFIG_IDF_TARGET_LINUX
