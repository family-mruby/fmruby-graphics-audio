#pragma once

// Linux (SHM) display only: the screen size can change at run time, for the
// fullscreen high-resolution mode (SET_SCREEN_MODE). The ESP32 display
// (display_cvbs.cpp) has no such call; graphics_handler uses these only in
// its CONFIG_IDF_TARGET_LINUX code.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Change the size of the screen sprite (the LGFX instance get_lgfx returns).
 * The pointer stays the same; its width()/height() follow. 0x0 is the base
 * screen (INIT_DISPLAY). Call from the render task only.
 * @return 0 on success, -1 if the size does not fit the screen buffer
 */
int shm_display_set_screen_size(uint16_t width, uint16_t height);

/** The base screen size (INIT_DISPLAY). */
void shm_display_get_base_size(uint16_t *width, uint16_t *height);

#ifdef __cplusplus
}
#endif
