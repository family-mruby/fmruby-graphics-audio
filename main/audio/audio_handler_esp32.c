#include "audio_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "audio_handler";

// ESP32 audio handler stub implementation
// TODO: Implement audio playback using apu_emu or I2S

typedef struct {
    uint32_t music_id;
    uint8_t *data;
    uint32_t size;
} music_track_t;

static fmrb_audio_status_t current_status = FMRB_AUDIO_STATUS_STOPPED;
static uint8_t current_volume = 128;
static music_track_t music_tracks[FMRB_MAX_MUSIC_TRACKS];
static int track_count = 0;

int audio_handler_init(void) {
    // Clear music tracks
    memset(music_tracks, 0, sizeof(music_tracks));
    track_count = 0;

    ESP_LOGI(TAG, "Audio handler initialized (ESP32 stub)");
    return 0;
}

void audio_handler_cleanup(void) {
    // Free music tracks
    for (int i = 0; i < track_count; i++) {
        if (music_tracks[i].data) {
            free(music_tracks[i].data);
            music_tracks[i].data = NULL;
        }
    }
    track_count = 0;

    ESP_LOGI(TAG, "Audio handler cleaned up (ESP32 stub)");
}

static int process_load_command(const fmrb_audio_load_cmd_t *cmd, const uint8_t *music_data) {
    if (track_count >= FMRB_MAX_MUSIC_TRACKS) {
        ESP_LOGE(TAG, "Maximum music tracks reached");
        return -1;
    }

    // Find existing track or create new one
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
        // Free existing data
        if (music_tracks[track_idx].data) {
            free(music_tracks[track_idx].data);
        }
    }

    // Store music data
    music_tracks[track_idx].music_id = cmd->music_id;
    music_tracks[track_idx].size = cmd->data_size;
    music_tracks[track_idx].data = malloc(cmd->data_size);

    if (!music_tracks[track_idx].data) {
        ESP_LOGE(TAG, "Failed to allocate music data");
        return -1;
    }

    memcpy(music_tracks[track_idx].data, music_data, cmd->data_size);

    ESP_LOGI(TAG, "Loaded music track %lu (%lu bytes) (ESP32 stub)", (unsigned long)cmd->music_id, (unsigned long)cmd->data_size);
    return 0;
}

static int process_play_command(const fmrb_audio_play_cmd_t *cmd) {
    // Find music track
    for (int i = 0; i < track_count; i++) {
        if (music_tracks[i].music_id == cmd->music_id) {
            ESP_LOGI(TAG, "Playing music track %lu (ESP32 stub)", (unsigned long)cmd->music_id);
            current_status = FMRB_AUDIO_STATUS_PLAYING;
            return 0;
        }
    }

    ESP_LOGE(TAG, "Music track %lu not found", (unsigned long)cmd->music_id);
    return -1;
}

static int process_stop_command(void) {
    ESP_LOGI(TAG, "Stopping audio playback (ESP32 stub)");
    current_status = FMRB_AUDIO_STATUS_STOPPED;
    return 0;
}

static int process_pause_command(void) {
    ESP_LOGI(TAG, "Pausing audio playback (ESP32 stub)");
    current_status = FMRB_AUDIO_STATUS_PAUSED;
    return 0;
}

static int process_resume_command(void) {
    ESP_LOGI(TAG, "Resuming audio playback (ESP32 stub)");
    current_status = FMRB_AUDIO_STATUS_PLAYING;
    return 0;
}

static int process_volume_command(const fmrb_audio_volume_cmd_t *cmd) {
    current_volume = cmd->volume;
    ESP_LOGI(TAG, "Set volume to %u (ESP32 stub)", cmd->volume);
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
                return process_play_command((const fmrb_audio_play_cmd_t*)data);
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
