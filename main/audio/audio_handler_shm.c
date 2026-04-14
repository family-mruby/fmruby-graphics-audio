/**
 * @file audio_handler_shm.c
 * @brief Audio handler using shared memory ring buffer for Linux headless builds.
 *        APU samples are written to SHM; SDL2 display process reads and plays them.
 */
#include "audio_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "apu_if.h"
#include "shm_display.h"

static const char *TAG = "audio_handler";

typedef struct {
    uint32_t music_id;
    uint8_t *data;
    uint32_t size;
} music_track_t;

static fmrb_shm_t* g_shm = NULL;
static int g_shm_fd = -1;
static fmrb_audio_status_t current_status = FMRB_AUDIO_STATUS_STOPPED;
static uint8_t current_volume = 128;
static music_track_t music_tracks[FMRB_MAX_MUSIC_TRACKS];
static int track_count = 0;

int audio_handler_init(void) {
    /* Wait for shared memory to be created by display_shm.
     * Use vTaskDelay instead of usleep because FreeRTOS SIGALRM
     * interrupts usleep with EINTR. Also retry shm_open on EINTR. */
    ESP_LOGI(TAG, "Waiting for shared memory...");
    for (int i = 0; i < 300; i++) { /* Up to 30 seconds */
        do {
            g_shm_fd = shm_open(FMRB_SHM_NAME, O_RDWR, 0666);
        } while (g_shm_fd < 0 && errno == EINTR);
        if (g_shm_fd >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (g_shm_fd < 0) {
        ESP_LOGE(TAG, "shm_open timed out: %s", strerror(errno));
        return -1;
    }

    g_shm = (fmrb_shm_t*)mmap(NULL, sizeof(fmrb_shm_t),
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        ESP_LOGE(TAG, "mmap failed: %s", strerror(errno));
        g_shm = NULL;
        close(g_shm_fd);
        g_shm_fd = -1;
        return -1;
    }

    /* Clear music tracks */
    memset(music_tracks, 0, sizeof(music_tracks));
    track_count = 0;

    /* Initialize audio ring buffer positions */
    g_shm->audio_write_pos = 0;
    g_shm->audio_read_pos = 0;

    ESP_LOGI(TAG, "Audio handler initialized (SHM ring buffer mode)");
    return 0;
}

void audio_handler_cleanup(void) {
    /* Free music tracks */
    for (int i = 0; i < track_count; i++) {
        if (music_tracks[i].data) {
            free(music_tracks[i].data);
            music_tracks[i].data = NULL;
        }
    }
    track_count = 0;

    if (g_shm) {
        munmap(g_shm, sizeof(fmrb_shm_t));
        g_shm = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }

    ESP_LOGI(TAG, "Audio handler cleaned up");
}

/**
 * Called periodically from audio_task to transfer APU samples to SHM ring buffer.
 */
void audio_handler_push_samples(void) {
    if (!g_shm) return;

    /* Read mono samples from APU ring buffer */
    int16_t mono_buf[262];
    int chunk = 262; /* ~1 frame at NTSC rate (15720/60) */
    apuif_ring_read(mono_buf, chunk);

    /* Convert mono to stereo and write to SHM ring buffer */
    uint32_t ring_size = FMRB_SHM_AUDIO_RING_SIZE * 2;
    uint32_t wp = g_shm->audio_write_pos;

    for (int i = 0; i < chunk; i++) {
        uint32_t next_wp = (wp + 2) % ring_size;
        if (next_wp == g_shm->audio_read_pos) {
            break; /* Ring full, drop remaining samples */
        }
        g_shm->audio_ring[wp] = mono_buf[i];       /* L */
        g_shm->audio_ring[wp + 1] = mono_buf[i];   /* R */
        wp = next_wp;
    }
    g_shm->audio_write_pos = wp;
}

/**
 * Flush the SHM audio ring buffer.
 * Called on note_on/play to eliminate buffered silence and reduce latency.
 */
void audio_handler_flush(void) {
    if (!g_shm) return;
    g_shm->audio_read_pos = g_shm->audio_write_pos;
}

static int process_load_command(const fmrb_audio_load_cmd_t *cmd, const uint8_t *music_data) {
    if (track_count >= FMRB_MAX_MUSIC_TRACKS) {
        ESP_LOGE(TAG, "Maximum music tracks reached");
        return -1;
    }

    int track_idx = -1;
    for (int i = 0; i < track_count; i++) {
        if (music_tracks[i].music_id == cmd->music_id) {
            track_idx = i;
            break;
        }
    }

    if (track_idx == -1) {
        track_idx = track_count++;
    } else {
        if (music_tracks[track_idx].data) {
            free(music_tracks[track_idx].data);
        }
    }

    music_tracks[track_idx].music_id = cmd->music_id;
    music_tracks[track_idx].size = cmd->data_size;
    music_tracks[track_idx].data = malloc(cmd->data_size);

    if (!music_tracks[track_idx].data) {
        ESP_LOGE(TAG, "Failed to allocate music data");
        return -1;
    }

    memcpy(music_tracks[track_idx].data, music_data, cmd->data_size);

    ESP_LOGI(TAG, "Loaded music track %u (%u bytes)", cmd->music_id, cmd->data_size);
    return 0;
}

static int process_play_command(const fmrb_audio_play_cmd_t *cmd, size_t total_size) {
    if (total_size < sizeof(fmrb_audio_play_cmd_t) + cmd->path_len) {
        ESP_LOGE(TAG, "Play command too short");
        return -1;
    }

    char path[128];
    int len = cmd->path_len < sizeof(path) - 1 ? cmd->path_len : sizeof(path) - 1;
    memcpy(path, cmd->path, len);
    path[len] = '\0';

    // Track number follows path (1 byte, 0-based). Default 0 if not present.
    int track = 0;
    if (total_size > sizeof(fmrb_audio_play_cmd_t) + cmd->path_len) {
        track = (int)((uint8_t)cmd->path[cmd->path_len]);
    }

    ESP_LOGI(TAG, "Play command: path=%s track=%d", path, track);

    int ret = audio_task_nsf_play(path, track);
    if (ret == 0) {
        current_status = FMRB_AUDIO_STATUS_PLAYING;
    }
    return ret;
}

static int process_stop_command(void) {
    ESP_LOGI(TAG, "Stopping audio playback");
    audio_task_nsf_stop();
    current_status = FMRB_AUDIO_STATUS_STOPPED;
    return 0;
}

static int process_pause_command(void) {
    ESP_LOGI(TAG, "Pausing audio playback");
    current_status = FMRB_AUDIO_STATUS_PAUSED;
    return 0;
}

static int process_resume_command(void) {
    ESP_LOGI(TAG, "Resuming audio playback");
    current_status = FMRB_AUDIO_STATUS_PLAYING;
    return 0;
}

static int process_volume_command(const fmrb_audio_volume_cmd_t *cmd) {
    current_volume = cmd->volume;
    ESP_LOGI(TAG, "Set volume to %u", cmd->volume);
    return 0;
}

int audio_handler_process_command(const uint8_t *data, size_t size) {
    if (!data || size == 0) {
        return -1;
    }

    uint8_t cmd_type = data[0];

    switch (cmd_type) {
        case FMRB_AUDIO_CMD_LOAD_BINARY:
            if (size >= sizeof(fmrb_audio_load_cmd_t)) {
                const fmrb_audio_load_cmd_t *cmd = (const fmrb_audio_load_cmd_t*)data;
                const uint8_t *music_data = data + sizeof(fmrb_audio_load_cmd_t);
                if (size >= sizeof(fmrb_audio_load_cmd_t) + cmd->data_size) {
                    return process_load_command(cmd, music_data);
                }
            }
            break;

        case FMRB_AUDIO_CMD_PLAY:
            if (size >= sizeof(fmrb_audio_play_cmd_t)) {
                return process_play_command((const fmrb_audio_play_cmd_t*)data, size);
            }
            break;

        case FMRB_AUDIO_CMD_STOP:
            return process_stop_command();

        case FMRB_AUDIO_CMD_PAUSE:
            return process_pause_command();

        case FMRB_AUDIO_CMD_RESUME:
            return process_resume_command();

        case FMRB_AUDIO_CMD_SET_VOLUME:
            if (size >= sizeof(fmrb_audio_volume_cmd_t)) {
                return process_volume_command((const fmrb_audio_volume_cmd_t*)data);
            }
            break;

        case FMRB_AUDIO_CMD_PLAY_SLOT:
            if (size >= sizeof(fmrb_audio_play_slot_cmd_t)) {
                const fmrb_audio_play_slot_cmd_t *cmd = (const fmrb_audio_play_slot_cmd_t*)data;
                ESP_LOGI(TAG, "Play slot command: music_id=%lu", (unsigned long)cmd->music_id);
                return audio_task_fmsq_play_slot(cmd->music_id);
            }
            break;

        case FMRB_AUDIO_CMD_NOTE_ON:
            if (size >= sizeof(fmrb_audio_note_on_cmd_t)) {
                const fmrb_audio_note_on_cmd_t *cmd = (const fmrb_audio_note_on_cmd_t*)data;
                return audio_task_note_on(cmd->channel, cmd->freq, cmd->volume, cmd->duty, cmd->sweep);
            }
            break;

        case FMRB_AUDIO_CMD_NOTE_OFF:
            if (size >= sizeof(fmrb_audio_note_off_cmd_t)) {
                const fmrb_audio_note_off_cmd_t *cmd = (const fmrb_audio_note_off_cmd_t*)data;
                return audio_task_note_off(cmd->channel);
            }
            break;

        default:
            ESP_LOGE(TAG, "Unknown audio command: 0x%02x", cmd_type);
            return -1;
    }

    ESP_LOGE(TAG, "Invalid command size for audio type 0x%02x", cmd_type);
    return -1;
}

fmrb_audio_status_t audio_handler_get_status(void) {
    return current_status;
}

void audio_handler_set_volume(uint8_t volume) {
    current_volume = volume;
}

int audio_handler_get_track(uint32_t music_id, const uint8_t **out_data, uint32_t *out_size) {
    for (int i = 0; i < track_count; i++) {
        if (music_tracks[i].music_id == music_id && music_tracks[i].data) {
            *out_data = music_tracks[i].data;
            *out_size = music_tracks[i].size;
            return 0;
        }
    }
    return -1;
}
