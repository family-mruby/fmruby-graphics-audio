#include <stdio.h>
#include <stdlib.h>
#include "sdkconfig.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

// Linux向けSDL設定（手動設定 - LGFX_AUTODETECTは使用しない）
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

static LGFX* lcd = nullptr;
static bool setup_done = false;

void setup(void)
{
    printf("=== LovyanGFX Test (main_linux.cpp) ===\n");
    printf("DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "not set");
    printf("WAYLAND_DISPLAY=%s\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "not set");

    // Allocate LCD object dynamically to avoid stack overflow
    printf("Allocating LGFX object (size: %zu bytes)...\n", sizeof(LGFX));
    lcd = new LGFX();
    printf("LGFX object allocated at: %p\n", (void*)lcd);

    printf("Initializing display...\n");
    lcd->init();

    printf("Setting rotation...\n");
    lcd->setRotation(0);

    printf("Filling screen with black...\n");
    lcd->fillScreen(TFT_BLACK);

    printf("Drawing test patterns...\n");

    // Color bars
    int barWidth = lcd->width() / 7;
    lcd->fillRect(barWidth * 0, 0, barWidth, 60, TFT_RED);
    lcd->fillRect(barWidth * 1, 0, barWidth, 60, TFT_GREEN);
    lcd->fillRect(barWidth * 2, 0, barWidth, 60, TFT_BLUE);
    lcd->fillRect(barWidth * 3, 0, barWidth, 60, TFT_YELLOW);
    lcd->fillRect(barWidth * 4, 0, barWidth, 60, TFT_CYAN);
    lcd->fillRect(barWidth * 5, 0, barWidth, 60, TFT_MAGENTA);
    lcd->fillRect(barWidth * 6, 0, barWidth, 60, TFT_WHITE);

    // Text drawing
    lcd->setTextSize(2);
    lcd->setTextColor(TFT_WHITE, TFT_BLACK);
    lcd->setCursor(10, 80);
    lcd->print("LovyanGFX Test");

    lcd->setCursor(10, 110);
    lcd->setTextColor(TFT_GREEN, TFT_BLACK);
    lcd->print("Display OK!");

    // Circle drawing
    lcd->drawCircle(128, 180, 40, TFT_YELLOW);
    lcd->fillCircle(128, 180, 35, TFT_BLUE);

    printf("Test pattern complete!\n");
    printf("Display size: %d x %d\n", lcd->width(), lcd->height());
    printf("Press Ctrl+C to exit\n");

    setup_done = true;
}

void loop(void)
{
    // Update display
    if (lcd) {
        lcd->display();
    }
    // Add small delay to reduce CPU usage
    lgfx::delay(16); // ~60fps
}

int user_func(bool* running)
{
    setup();
    while (*running) {
        loop();
    }

    // Cleanup
    printf("Cleaning up...\n");
    if (lcd) {
        delete lcd;
        lcd = nullptr;
    }

    return 0;
}

extern "C" int app_main(void)
{
    return lgfx::Panel_sdl::main(user_func);
}
