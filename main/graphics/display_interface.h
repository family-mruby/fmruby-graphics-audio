#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Display interface abstraction
 * Allows switching between SDL2 (Linux) and direct hardware (ESP32)
 */
typedef struct {
    /**
     * Initialize display
     * @param width Display width in pixels (memory buffer size)
     * @param height Display height in pixels (memory buffer size)
     * @param color_depth Color depth in bits (8 for RGB332)
     * @param margin_x Horizontal margin (left+right) in pixels for overscan
     * @param margin_y Vertical margin (top+bottom) in pixels for overscan
     * @return 0 on success, -1 on error
     */
    int (*init)(uint16_t width, uint16_t height, uint8_t color_depth,
                uint8_t margin_x, uint8_t margin_y);

    /**
     * Get LovyanGFX instance pointer
     * @return Pointer to LGFX instance (void* to avoid C++ dependency)
     */
    void* (*get_lgfx)(void);

    /**
     * Process display events (e.g., SDL2 events on Linux)
     * Returns 0 normally, 1 if quit requested, -1 on error
     * @return Event status
     */
    int (*process_events)(void);

    /**
     * Update display (flush to screen)
     */
    void (*display)(void);

    /**
     * Cleanup and shutdown display
     */
    void (*cleanup)(void);

    /**
     * Get display scaling factors (optional, NULL if not applicable)
     * @param scaling_x Pointer to receive X scaling factor (NULL to skip)
     * @param scaling_y Pointer to receive Y scaling factor (NULL to skip)
     * @return 0 on success, -1 if not supported
     *
     * For SDL2: Returns the scaling factor used to enlarge the display window
     * For ESP32: Not applicable (always 1x), returns -1
     */
    int (*get_scaling)(uint_fast8_t* scaling_x, uint_fast8_t* scaling_y);
} display_interface_t;

// Get the active display interface
const display_interface_t* display_get_interface(void);

#define DISPLAY_INTERFACE (display_get_interface())

#ifdef __cplusplus
}

// Global LGFX instance (defined in display implementation files)
// Note: This is a C++ pointer, so only accessible from C++ code
// - Linux/SDL: defined in display_sdl2.cpp
// - ESP32: defined in display_cvbs.cpp
//
// These are managed by the display interface and should not be
// directly accessed by application code. Use display_interface_t
// functions instead (e.g., DISPLAY_INTERFACE->get_lgfx())
#endif
