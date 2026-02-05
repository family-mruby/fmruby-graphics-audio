#ifndef CONFIG_IDF_TARGET_LINUX

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32/Panel_CVBS.hpp>

#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "display_cvbs";

// LGFX class for ESP32 with CVBS output
class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Panel_CVBS _panel_instance;

  LGFX(uint16_t width, uint16_t height)
  {
    {
      auto cfg = _panel_instance.config();

      // Set output resolution
      cfg.memory_width  = width;
      cfg.memory_height = height;

      // Set actual usable resolution (slightly smaller for margins)
      cfg.panel_width  = width - 32;
      cfg.panel_height = height - 32;

      // Set display offset
      cfg.offset_x = 16;
      cfg.offset_y = 16;

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config_detail();

      // NTSC-J output for Japan
      cfg.signal_type = cfg.signal_type_t::NTSC_J;

      // GPIO pin for DAC output (25 or 26 only)
      cfg.pin_dac = 25;

      // PSRAM usage: 0=no PSRAM, 1=half PSRAM/SRAM, 2=full PSRAM
      cfg.use_psram = 1;

      // Output signal strength
      cfg.output_level = 128;

      // Chroma signal strength
      cfg.chroma_level = 128;

      // Background task priority
      cfg.task_priority = 25;

      // Pin background task to Core 1
      cfg.task_pinned_core = 1;

      _panel_instance.config_detail(cfg);
    }

    setPanel(&_panel_instance);
  }
};

// Include display_interface.h after LGFX class definition
#include "display_interface.h"

LGFX* g_lgfx = nullptr;

extern "C" {

static int esp32_init(uint16_t width, uint16_t height, uint8_t color_depth) {
    if (g_lgfx) {
        // Already initialized
        return 0;
    }

    // Create LovyanGFX instance for ESP32 CVBS
    g_lgfx = new LGFX(width, height);
    if (!g_lgfx) {
        ESP_LOGE(TAG, "Failed to create LovyanGFX instance");
        return -1;
    }

    g_lgfx->init();
    g_lgfx->setColorDepth(color_depth);
    g_lgfx->fillScreen(0x0000);  // Black

    ESP_LOGI(TAG, "ESP32 CVBS display initialized: %dx%d, %d-bit", width, height, color_depth);
    return 0;
}

static void* esp32_get_lgfx(void) {
    return (void*)g_lgfx;
}

static int esp32_process_events(void) {
    // ESP32 has no events to process (no SDL2)
    return 0;
}

static void esp32_display(void) {
    if (g_lgfx) {
        g_lgfx->display();
    }
}

static void esp32_cleanup(void) {
    if (g_lgfx) {
        delete g_lgfx;
        g_lgfx = nullptr;
    }
    ESP_LOGI(TAG, "ESP32 display cleaned up");
}

static const display_interface_t esp32_display_impl = {
    .init = esp32_init,
    .get_lgfx = esp32_get_lgfx,
    .process_events = esp32_process_events,
    .display = esp32_display,
    .cleanup = esp32_cleanup,
};

const display_interface_t* display_get_interface(void) {
    return &esp32_display_impl;
}

} // extern "C"

#endif // !CONFIG_IDF_TARGET_LINUX
