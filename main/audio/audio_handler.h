#pragma once

#include <stdint.h>
#include <stddef.h>
#include "audio_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize audio handler
 * @return 0 on success, -1 on error
 */
int audio_handler_init(void);

/**
 * @brief Cleanup audio handler
 */
void audio_handler_cleanup(void);

/**
 * @brief Process audio command
 * @param data Command data
 * @param size Data size
 * @return 0 on success, -1 on error
 */
int audio_handler_process_command(const uint8_t *data, size_t size);

/**
 * @brief Get current audio status
 * @return Current audio status
 */
fmrb_audio_status_t audio_handler_get_status(void);

/**
 * @brief Set audio volume
 * @param volume Volume level (0-255)
 */
void audio_handler_set_volume(uint8_t volume);

void audio_check_impl(void);

/**
 * @brief Push APU samples to shared memory ring buffer (Linux SHM mode only).
 *        Called from audio_task's 60Hz loop to transfer samples to SDL2 process.
 *        No-op on ESP32 (SDL2 audio callback handles this directly).
 */
void audio_handler_push_samples(void);

/**
 * @brief Flush audio output buffer to minimize latency.
 *        Called on note_on/play to discard buffered silence so new sounds
 *        are heard immediately. No-op on ESP32.
 */
void audio_handler_flush(void);

/**
 * @brief Find loaded music track by ID
 * @param music_id Track ID to find
 * @param out_data Pointer to receive data pointer (not owned by caller)
 * @param out_size Pointer to receive data size
 * @return 0 on success, -1 if not found
 */
int audio_handler_get_track(uint32_t music_id, const uint8_t **out_data, uint32_t *out_size);

/**
 * @brief Start NSF playback from file path (called from audio command handler)
 * @param path NSF file path (relative to flash, e.g. "/data/test.nsf")
 * @return 0 on success, -1 on error
 */
int audio_task_nsf_play(const char *path, int track);

/**
 * @brief Stop NSF playback
 */
void audio_task_nsf_stop(void);

/**
 * @brief Play FMSQ from a loaded slot
 * @param music_id Slot ID to play
 * @param instance APU instance (0=MAIN, shares with NSF; 1=SUB, shares
 *                 with note_on/off SE). Defaults to MAIN when callers
 *                 pass 0.
 * @return 0 on success, -1 on error
 */
int audio_task_fmsq_play_slot(uint32_t music_id, uint8_t instance);

/**
 * @brief Start a note on an APU channel
 * @param channel Channel (0=pulse1, 1=pulse2, 2=triangle, 3=noise)
 * @param freq Frequency in Hz
 * @param volume Volume (0-15)
 * @param duty Duty cycle (0-3, pulse only)
 * @return 0 on success, -1 on error
 */
int audio_task_note_on(uint8_t channel, uint16_t freq, uint8_t volume, uint8_t duty, uint8_t sweep);

/**
 * @brief Stop a note on an APU channel
 * @param channel Channel (0=pulse1, 1=pulse2, 2=triangle, 3=noise)
 * @return 0 on success, -1 on error
 */
int audio_task_note_off(uint8_t channel);

/**
 * @brief Play a WAV file on top of the APU
 *
 * PCM 16-bit mono, 8-48 kHz, up to FMRB_WAV_MAX_BYTES, read whole from this
 * side's filesystem and resampled to the APU's mono rate. Starting one while
 * another plays replaces it. Every refusal (missing file, wrong format, too
 * large) logs one line and leaves the current sound alone.
 *
 * @param path Path on this side's filesystem
 * @return 0 when playback started, -1 otherwise
 */
int audio_task_play_wav(const char *path);

/**
 * @brief Stop the WAV started by audio_task_play_wav
 * @return 0 (stopping when nothing plays is not an error)
 */
int audio_task_stop_wav(void);

#ifdef __cplusplus
}
#endif