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

#ifdef CONFIG_IDF_TARGET_LINUX

#define NSF_FILE_PATH  "/project/flash/data/test.nsf"
#define FMSQ_FILE_PATH "/project/flash/data/test.fmsq"

static nsf_player_t *g_nsf_player = NULL;
static fmsq_player_t *g_fmsq_player = NULL;

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

    /* Initialize sub APU (instance 1: FMSQ) and load FMSQ file */
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

int audio_task_nsf_play(const char *path) {
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "NSF play: empty path");
        return -1;
    }

    // Build full path: "flash" + path (path starts with /)
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "flash%s", path);

    ESP_LOGI(TAG, "NSF play: %s", full_path);

    // Stop current playback
    if (g_nsf_player && g_nsf_player->playing) {
        g_nsf_player->playing = 0;
    }

    // Free existing player if loaded from different file
    if (g_nsf_player) {
        nsf_player_free(g_nsf_player);
    } else {
        g_nsf_player = (nsf_player_t *)apuemu_malloc(sizeof(nsf_player_t));
        if (!g_nsf_player) {
            ESP_LOGE(TAG, "NSF play: malloc failed");
            return -1;
        }
    }

    // Load and start
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

#else /* ESP32 */

void audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());
    audio_check_impl();
}

int audio_task_nsf_play(const char *path) {
    // TODO: Implement for ESP32
    return -1;
}

void audio_task_nsf_stop(void) {
    // TODO: Implement for ESP32
}

#endif /* CONFIG_IDF_TARGET_LINUX */
