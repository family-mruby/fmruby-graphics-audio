#ifndef LGFX_LINUX_H
#define LGFX_LINUX_H

#if defined(CONFIG_IDF_TARGET_LINUX) || defined(LGFX_USE_SDL)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

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

extern LGFX* g_lgfx;

#endif // CONFIG_IDF_TARGET_LINUX || LGFX_USE_SDL

#endif // LGFX_LINUX_H
