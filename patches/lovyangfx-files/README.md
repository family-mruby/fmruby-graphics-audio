# LovyanGFX Modified Files for ESP-IDF Linux Support

This directory contains modified LovyanGFX files to enable ESP-IDF Linux target support.

## Files

- `esp-idf.cmake` → `components/LovyanGFX/boards.cmake/esp-idf.cmake`
- `common.hpp` → `components/LovyanGFX/src/lgfx/v1/platforms/common.hpp`
- `device.hpp` → `components/LovyanGFX/src/lgfx/v1/platforms/device.hpp`
- `Panel_sdl.cpp` → `components/LovyanGFX/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp`

## Usage

These files are automatically copied by the `apply_patches` Rake task when building for ESP32 or ESP-IDF Linux targets.
