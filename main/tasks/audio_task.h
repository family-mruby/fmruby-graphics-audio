#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Audio processing task
 * Handles audio playback
 */
void audio_task(void *pvParameters);

/**
 * Stop the audio task
 */
void audio_task_stop(void);

#ifdef __cplusplus
}
#endif
