#pragma once

/**
 * @file fmrb_time.h
 * @brief Monotonic microsecond clock, usable from both builds.
 *
 * The ESP32 build has esp_timer; the Linux simulation build does not pull it
 * in, so it falls back to CLOCK_MONOTONIC. Both are monotonic and start at an
 * arbitrary point, so only differences are meaningful.
 */

#include <stdint.h>
/* CONFIG_* come from sdkconfig.h; include it here so this header works no
 * matter where it lands in an include list. */
#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_LINUX
#include <time.h>
#else
#include "esp_timer.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t fmrb_now_us(void) {
#ifdef CONFIG_IDF_TARGET_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
#else
    return (uint64_t)esp_timer_get_time();
#endif
}

#ifdef __cplusplus
}
#endif
