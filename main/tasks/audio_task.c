#include "audio_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_handler.h"
#include "apu_if.h"
#include "nsf_player.h"
#include "fmsq_player.h"

#include <string.h>

static const char *TAG = "audio_task";
static volatile int task_running = 1;

void audio_task_stop(void) {
    task_running = 0;
}

#define NTSC_SAMPLE 262

/* NES APU CPU clock (NTSC) */
#define APU_CPU_CLOCK 1789773

/*
 * Shared state: NSF and FMSQ players.
 * These are accessed from audio_task (60Hz tick) and from
 * audio_handler commands (note_on, nsf_play, etc.) which run
 * on the message_handler_task. Since commands only set flags
 * or pointers atomically, no mutex is needed.
 */
static nsf_player_t *g_nsf_player = NULL;
static fmsq_player_t *g_fmsq_player = NULL;

/* ------------------------------------------------------------------ */
/* Common functions (platform-independent APU operations)              */
/* ------------------------------------------------------------------ */

int audio_task_nsf_play(const char *path) {
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "NSF play: empty path");
        return -1;
    }

#ifdef CONFIG_IDF_TARGET_LINUX
    /* Linux: paths are relative to project root */
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "flash%s", path);
#else
    /* ESP32: paths are absolute via LittleFS */
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/flash%s", path);
#endif

    ESP_LOGI(TAG, "NSF play: %s", full_path);

    /* Stop current playback */
    if (g_nsf_player && g_nsf_player->playing) {
        g_nsf_player->playing = 0;
    }

    /* Free existing player if loaded from different file */
    if (g_nsf_player) {
        nsf_player_free(g_nsf_player);
    } else {
        g_nsf_player = (nsf_player_t *)apuemu_malloc(sizeof(nsf_player_t));
        if (!g_nsf_player) {
            ESP_LOGE(TAG, "NSF play: malloc failed");
            return -1;
        }
    }

    /* Load and start */
    if (nsf_player_load(g_nsf_player, full_path) != 0) {
        ESP_LOGE(TAG, "NSF play: failed to load %s", full_path);
        apuemu_free(g_nsf_player);
        g_nsf_player = NULL;
        return -1;
    }

    apuif_select(APUIF_INSTANCE_MAIN);
    nsf_player_start(g_nsf_player, 0);
    return 0;
}

void audio_task_nsf_stop(void) {
    if (g_nsf_player) {
        ESP_LOGI(TAG, "NSF stop");
        g_nsf_player->playing = 0;
    }
}

int audio_task_fmsq_play_slot(uint32_t music_id) {
    const uint8_t *data = NULL;
    uint32_t size = 0;

    if (audio_handler_get_track(music_id, &data, &size) != 0) {
        ESP_LOGE(TAG, "FMSQ play_slot: track %lu not found", (unsigned long)music_id);
        return -1;
    }

    if (!g_fmsq_player) {
        g_fmsq_player = (fmsq_player_t *)apuemu_malloc(sizeof(fmsq_player_t));
        if (!g_fmsq_player) {
            ESP_LOGE(TAG, "FMSQ play_slot: malloc failed");
            return -1;
        }
        memset(g_fmsq_player, 0, sizeof(fmsq_player_t));
    }

    /* Stop current playback */
    g_fmsq_player->playing = 0;

    apuif_select(APUIF_INSTANCE_SUB);

#ifdef __linux__
    /* Flush ring buffer to minimize playback latency (Linux/SDL2 only) */
    apuif_ring_flush();
#endif

    if (fmsq_player_load_from_memory(g_fmsq_player, data, size) != 0) {
        ESP_LOGE(TAG, "FMSQ play_slot: load failed for track %lu", (unsigned long)music_id);
        return -1;
    }

    fmsq_player_reset(g_fmsq_player);
    ESP_LOGI(TAG, "FMSQ play_slot: playing track %lu", (unsigned long)music_id);
    return 0;
}

int audio_task_note_on(uint8_t channel, uint16_t freq, uint8_t volume, uint8_t duty, uint8_t sweep) {
    if (freq == 0) return -1;

    apuif_select(APUIF_INSTANCE_SUB);

#ifdef __linux__
    /* Flush ring buffer to minimize latency (Linux/SDL2 only) */
    apuif_ring_flush();
#endif

    /* Stop FMSQ playback if running (avoid conflicts on SUB instance) */
    if (g_fmsq_player && g_fmsq_player->playing) {
        g_fmsq_player->playing = 0;
    }

    uint8_t vol = volume & 0x0F;

    switch (channel) {
    case FMRB_APU_CH_PULSE1:
    case FMRB_APU_CH_PULSE2: {
        uint16_t timer = APU_CPU_CLOCK / (16 * freq) - 1;
        uint16_t base = (channel == FMRB_APU_CH_PULSE1) ? 0x4000 : 0x4004;
        /* Enable channel in status register */
        uint8_t status_bit = (channel == FMRB_APU_CH_PULSE1) ? 0x01 : 0x02;
        apuif_write_reg(0x4015, apuif_read_reg(0x4015) | status_bit);
        /* VOL: duty(2) + length halt(1) + constant vol(1) + vol(4) */
        apuif_write_reg(base + 0, ((duty & 0x03) << 6) | 0x30 | vol);
        /* SWEEP */
        apuif_write_reg(base + 1, sweep);
        /* TIMER_LO */
        apuif_write_reg(base + 2, timer & 0xFF);
        /* TIMER_HI + length counter load */
        apuif_write_reg(base + 3, 0xF8 | ((timer >> 8) & 0x07));
        break;
    }
    case FMRB_APU_CH_TRIANGLE: {
        uint16_t timer = APU_CPU_CLOCK / (32 * freq) - 1;
        /* Enable triangle */
        apuif_write_reg(0x4015, apuif_read_reg(0x4015) | 0x04);
        /* LINEAR: halt(1) + counter(7) = 0xFF for sustained */
        apuif_write_reg(0x4008, 0xFF);
        /* TIMER_LO */
        apuif_write_reg(0x400A, timer & 0xFF);
        /* TIMER_HI + length counter load */
        apuif_write_reg(0x400B, 0xF8 | ((timer >> 8) & 0x07));
        break;
    }
    case FMRB_APU_CH_NOISE: {
        /* Enable noise */
        apuif_write_reg(0x4015, apuif_read_reg(0x4015) | 0x08);
        /* VOL: length halt + constant vol + volume */
        apuif_write_reg(0x400C, 0x30 | vol);
        /* PERIOD: low byte = period(0-15), bit7 = short mode */
        uint8_t mode = (freq & 0x80);   /* short-cycle noise flag */
        uint8_t period = freq & 0x0F;
        apuif_write_reg(0x400E, mode | period);
        /* LENGTH */
        apuif_write_reg(0x400F, 0xF8);
        break;
    }
    default:
        return -1;
    }

    ESP_LOGD(TAG, "note_on: ch=%d freq=%d vol=%d duty=%d", channel, freq, vol, duty);
    return 0;
}

int audio_task_note_off(uint8_t channel) {
    apuif_select(APUIF_INSTANCE_SUB);

    switch (channel) {
    case FMRB_APU_CH_PULSE1:
        apuif_write_reg(0x4000, 0x30); /* vol=0 */
        break;
    case FMRB_APU_CH_PULSE2:
        apuif_write_reg(0x4004, 0x30); /* vol=0 */
        break;
    case FMRB_APU_CH_TRIANGLE:
        /* Disable triangle channel */
        apuif_write_reg(0x4015, apuif_read_reg(0x4015) & ~0x04);
        break;
    case FMRB_APU_CH_NOISE:
        apuif_write_reg(0x400C, 0x30); /* vol=0 */
        break;
    default:
        return -1;
    }

    ESP_LOGD(TAG, "note_off: ch=%d", channel);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Platform-specific audio_task() main loop                           */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_IDF_TARGET_LINUX

#define NSF_FILE_PATH  "/project/flash/data/test.nsf"
#define FMSQ_FILE_PATH "/project/flash/data/test.fmsq"

void audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started (Linux)");

    /* Initialize SDL2 audio handler */
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed");
        return;
    }

    /* Initialize main APU (instance 0: NSF) */
    apuif_init();

    /* Load NSF file on main APU instance (playback starts on command) */
    g_nsf_player = (nsf_player_t *)apuemu_malloc(sizeof(nsf_player_t));
    if (g_nsf_player) {
        if (nsf_player_load(g_nsf_player, NSF_FILE_PATH) == 0) {
            ESP_LOGI(TAG, "NSF loaded: %d songs (waiting for play command)", g_nsf_player->header.total_songs);
        } else {
            ESP_LOGW(TAG, "No NSF file at %s", NSF_FILE_PATH);
            apuemu_free(g_nsf_player);
            g_nsf_player = NULL;
        }
    }

    /* Initialize sub APU (instance 1: FMSQ + note_on/off) */
    apuif_init_sub();

    g_fmsq_player = (fmsq_player_t *)apuemu_malloc(sizeof(fmsq_player_t));
    if (g_fmsq_player) {
        if (fmsq_player_load(g_fmsq_player, FMSQ_FILE_PATH) == 0) {
            fmsq_player_reset(g_fmsq_player);
            g_fmsq_player->playing = 0;  /* Wait for play command */
            ESP_LOGI(TAG, "FMSQ loaded: %d frames (waiting for play command)", g_fmsq_player->frame_count);
        } else {
            ESP_LOGW(TAG, "No FMSQ file at %s", FMSQ_FILE_PATH);
            apuemu_free(g_fmsq_player);
            g_fmsq_player = NULL;
        }
    }

    /* 60Hz audio processing loop */
    const uint32_t frame_interval_ms = 16;

    while (task_running) {
        /* Tick NSF player on main APU instance */
        if (g_nsf_player && g_nsf_player->playing) {
            apuif_select(APUIF_INSTANCE_MAIN);
            nsf_player_tick(g_nsf_player);
        }

        /* Tick FMSQ player on sub APU instance */
        if (g_fmsq_player && g_fmsq_player->playing) {
            apuif_select(APUIF_INSTANCE_SUB);
            fmsq_player_tick(g_fmsq_player);
        }

        /* Process both APU instances and mix output */
        int16_t buffer[(NTSC_SAMPLE + 1) * 2];
        memset(buffer, 0, sizeof(buffer));
        int count = apuif_process_mix(buffer, sizeof(buffer) / sizeof(buffer[0]));
        if (count > 0) {
            apuif_audio_write(buffer, count, 1);
        }

        vTaskDelay(pdMS_TO_TICKS(frame_interval_ms));
    }

    if (g_fmsq_player) {
        fmsq_player_free(g_fmsq_player);
        apuemu_free(g_fmsq_player);
        g_fmsq_player = NULL;
    }
    if (g_nsf_player) {
        nsf_player_free(g_nsf_player);
        apuemu_free(g_nsf_player);
        g_nsf_player = NULL;
    }
    audio_handler_cleanup();

    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}

#else /* ESP32 */

#include "esp_timer.h"

#define NSF_FILE_PATH "/flash/data/test.nsf"

void audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());

    /* Initialize main APU (instance 0: NSF) */
    apuif_init();

    /* Initialize sub APU (instance 1: FMSQ + note_on/off) */
    apuif_init_sub();

    /* Load default NSF file (playback starts on command from Core) */
    g_nsf_player = (nsf_player_t *)apuemu_malloc(sizeof(nsf_player_t));
    if (g_nsf_player) {
        if (nsf_player_load(g_nsf_player, NSF_FILE_PATH) == 0) {
            ESP_LOGI(TAG, "NSF loaded: %d songs (waiting for play command)",
                     g_nsf_player->header.total_songs);
        } else {
            ESP_LOGW(TAG, "No NSF file at %s", NSF_FILE_PATH);
            apuemu_free(g_nsf_player);
            g_nsf_player = NULL;
        }
    }

    /* 60Hz timing */
    const uint64_t target_frame_time_us = 16667;
    uint64_t next_frame_time = esp_timer_get_time();

    while (task_running) {
        /* Tick NSF player on main APU instance */
        if (g_nsf_player && g_nsf_player->playing) {
            apuif_select(APUIF_INSTANCE_MAIN);
            nsf_player_tick(g_nsf_player);
        }

        /* Tick FMSQ player on sub APU instance */
        if (g_fmsq_player && g_fmsq_player->playing) {
            apuif_select(APUIF_INSTANCE_SUB);
            fmsq_player_tick(g_fmsq_player);
        }

        /* Process both APU instances and mix output */
        int16_t buffer[(NTSC_SAMPLE + 1) * 2];
        memset(buffer, 0, sizeof(buffer));
        int count = apuif_process_mix(buffer, sizeof(buffer) / sizeof(buffer[0]));
        if (count > 0) {
            apuif_audio_write(buffer, count, 1);
        }

        /* Frame timing */
        next_frame_time += target_frame_time_us;
        uint64_t now = esp_timer_get_time();
        int64_t sleep_time_us = next_frame_time - now;

        if (sleep_time_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS(sleep_time_us / 1000));
        } else if (sleep_time_us < 0) {
            next_frame_time = now;
        }
    }

    if (g_fmsq_player) {
        fmsq_player_free(g_fmsq_player);
        apuemu_free(g_fmsq_player);
        g_fmsq_player = NULL;
    }
    if (g_nsf_player) {
        nsf_player_free(g_nsf_player);
        apuemu_free(g_nsf_player);
        g_nsf_player = NULL;
    }

    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}

#endif /* CONFIG_IDF_TARGET_LINUX */
