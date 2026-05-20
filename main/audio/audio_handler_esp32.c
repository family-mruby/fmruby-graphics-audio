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

static int process_play_command(const fmrb_audio_play_cmd_t *cmd, size_t total_size) {
    if (total_size < sizeof(fmrb_audio_play_cmd_t) + cmd->path_len) {
        ESP_LOGE(TAG, "Play command too short");
        return -1;
    }

    char path[128];
    int len = cmd->path_len < sizeof(path) - 1 ? cmd->path_len : sizeof(path) - 1;
    memcpy(path, cmd->path, len);
    path[len] = '\0';

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

/* Load an FMSQ slot by reading its bytes from LittleFS, then storing them in
 * the same music_tracks[] table that LOAD_BINARY uses. Lets us bypass the
 * inline IPC payload limit (~150 B) for larger BGM files. */
static int process_load_fmsq_file_command(const fmrb_audio_load_fmsq_file_cmd_t *cmd, size_t total_size) {
    if (total_size < sizeof(fmrb_audio_load_fmsq_file_cmd_t) + cmd->path_len) {
        ESP_LOGE(TAG, "Load FMSQ file command too short");
        return -1;
    }

    char path[128];
    int len = cmd->path_len < sizeof(path) - 1 ? (int)cmd->path_len : (int)(sizeof(path) - 1);
    memcpy(path, cmd->path, len);
    path[len] = '\0';

    /* Same prefix scheme as audio_task_nsf_play. */
    char full_path[256];
    const char *sep = path[0] == '/' ? "" : "/";
#ifdef CONFIG_IDF_TARGET_LINUX
    snprintf(full_path, sizeof(full_path), "flash%s%s", sep, path);
#else
    snprintf(full_path, sizeof(full_path), "/flash%s%s", sep, path);
#endif

    ESP_LOGI(TAG, "Load FMSQ file: music_id=%lu path=%s",
             (unsigned long)cmd->music_id, full_path);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open FMSQ %s", full_path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 65536) {
        ESP_LOGE(TAG, "Invalid FMSQ size %ld for %s", sz, full_path);
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    int track_idx = -1;
    for (int i = 0; i < track_count; i++) {
        if (music_tracks[i].music_id == cmd->music_id) {
            track_idx = i;
            break;
        }
    }
    if (track_idx == -1) {
        if (track_count >= FMRB_MAX_MUSIC_TRACKS) {
            ESP_LOGE(TAG, "Maximum music tracks reached");
            fclose(f);
            return -1;
        }
        track_idx = track_count++;
    } else if (music_tracks[track_idx].data) {
        free(music_tracks[track_idx].data);
        music_tracks[track_idx].data = NULL;
    }

    music_tracks[track_idx].music_id = cmd->music_id;
    music_tracks[track_idx].size = (uint32_t)sz;
    music_tracks[track_idx].data = malloc((size_t)sz);
    if (!music_tracks[track_idx].data) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for FMSQ", sz);
        fclose(f);
        return -1;
    }

    size_t read_bytes = fread(music_tracks[track_idx].data, 1, (size_t)sz, f);
    fclose(f);
    if (read_bytes != (size_t)sz) {
        ESP_LOGE(TAG, "FMSQ short read: %zu of %ld bytes", read_bytes, sz);
        free(music_tracks[track_idx].data);
        music_tracks[track_idx].data = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "Loaded FMSQ slot %lu from %s (%ld bytes)",
             (unsigned long)cmd->music_id, full_path, sz);
    return 0;
}

static int process_stop_command(void) {
    ESP_LOGI(TAG, "Stopping audio playback");
    audio_task_nsf_stop();
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
                ESP_LOGI(TAG, "Play slot command: music_id=%lu (ESP32 stub)", (unsigned long)cmd->music_id);
                return audio_task_fmsq_play_slot(cmd->music_id);
            }
            break;

        case FMRB_AUDIO_CMD_LOAD_FMSQ_FILE:
            if (size >= sizeof(fmrb_audio_load_fmsq_file_cmd_t)) {
                const fmrb_audio_load_fmsq_file_cmd_t *cmd = (const fmrb_audio_load_fmsq_file_cmd_t*)data;
                return process_load_fmsq_file_command(cmd, size);
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

void audio_handler_push_samples(void) {
    /* No-op on ESP32: I2S DMA handles audio output */
}

void audio_handler_flush(void) {
    /* No-op on ESP32 */
}
