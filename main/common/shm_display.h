#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shared memory and semaphore names
#define FMRB_SHM_NAME          "/fmrb_display"
#define FMRB_SEM_FRAME_NAME    "/fmrb_frame_ready"
#define FMRB_SEM_AUDIO_NAME    "/fmrb_audio_ready"

// Display constants
#define FMRB_SHM_MAX_WIDTH     480
#define FMRB_SHM_MAX_HEIGHT    320
#define FMRB_SHM_FRAMEBUF_SIZE (FMRB_SHM_MAX_WIDTH * FMRB_SHM_MAX_HEIGHT)

// Audio constants
#define FMRB_SHM_AUDIO_RING_SIZE  2048  // stereo int16_t samples (~130ms at 15720Hz)
#define FMRB_SHM_AUDIO_SAMPLE_RATE 15720

// Magic value set by FreeRTOS side when SHM is freshly initialized
#define FMRB_SHM_READY_MAGIC  0x464D5242  /* "FMRB" */

// Input event socket path
#define FMRB_INPUT_SOCKET_PATH "/var/run/fmrb/fmrb_sdl_input"

// Synthetic input injection socket (Unix DGRAM, bound by the SDL2 display
// process). Accepts pre-framed HID packets ([type][len16][payload]) and
// forwards them into the normal input stream. Used by tools/fmrb_input.py
// for agent/CI-driven input.
#define FMRB_INJECT_SOCKET_PATH "/var/run/fmrb/fmrb_inject"

// Control commands (via input socket, alongside HID events)
#define FMRB_CTRL_DISPLAY_INIT  0xF0
#define FMRB_CTRL_SHUTDOWN      0xFF

// Display init parameters (sent as FMRB_CTRL_DISPLAY_INIT payload)
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t color_depth;
    uint8_t scaling_x;
    uint8_t scaling_y;
} __attribute__((packed)) fmrb_display_init_t;

// Shared memory layout
typedef struct {
    // -- Handshake --
    volatile uint32_t ready_magic;      // Set to FMRB_SHM_READY_MAGIC by FreeRTOS after init
    volatile uint8_t display_initialized;  // Set by SDL2 side when ready
    volatile uint8_t shutdown_requested;   // Set by either side to request shutdown

    // -- Display section --
    uint16_t display_width;
    uint16_t display_height;
    uint8_t color_depth;
    uint8_t scaling_x;
    uint8_t scaling_y;

    uint8_t framebuf[2][FMRB_SHM_FRAMEBUF_SIZE];  // Double buffer RGB332
    volatile uint32_t write_index;      // Writer's current buffer (0 or 1)
    volatile uint32_t read_index;       // Reader's current buffer (0 or 1)

    // -- Audio section --
    int16_t audio_ring[FMRB_SHM_AUDIO_RING_SIZE * 2];  // stereo (L,R interleaved)
    volatile uint32_t audio_write_pos;   // Write position (FreeRTOS side)
    volatile uint32_t audio_read_pos;    // Read position (SDL2 side)
} fmrb_shm_t;

// Helper: calculate available audio samples for reading
static inline uint32_t fmrb_shm_audio_available(const fmrb_shm_t *shm) {
    uint32_t w = shm->audio_write_pos;
    uint32_t r = shm->audio_read_pos;
    uint32_t size = FMRB_SHM_AUDIO_RING_SIZE * 2;
    return (w - r + size) % size;
}

// Helper: calculate free audio space for writing
static inline uint32_t fmrb_shm_audio_free(const fmrb_shm_t *shm) {
    return (FMRB_SHM_AUDIO_RING_SIZE * 2) - 1 - fmrb_shm_audio_available(shm);
}

#ifdef __cplusplus
}
#endif
