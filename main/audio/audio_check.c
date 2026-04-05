#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_system.h"

#include "apu_if.h"
#include "nsf_player.h"

#define NTSC_SAMPLE 262
#define NSF_FILE_PATH "/flash/data/test.nsf"

static nsf_player_t *g_nsf_player = NULL;

static void update_audio(void)
{
    /* Execute NSF PLAY routine */
    if (g_nsf_player && g_nsf_player->playing) {
        nsf_player_tick(g_nsf_player);
    }

    /* Generate audio samples from APU */
    static int16_t abuffer[(NTSC_SAMPLE + 1) * 2];
    memset(abuffer, 0, sizeof(abuffer));
    int sample_count = apuif_process(abuffer, sizeof(abuffer));

    if (sample_count <= 0 || sample_count > (NTSC_SAMPLE + 1) * 2) {
        printf("[AUDIO_ERROR] Invalid sample count: %d\n", sample_count);
        return;
    }

    apuif_audio_write(abuffer, sample_count, 1);
}


static int load_nsf_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("NSF: Cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)apuemu_malloc(fsize);
    if (!data) {
        fclose(f);
        printf("NSF: Failed to allocate %ld bytes for file\n", fsize);
        return -1;
    }

    if ((long)fread(data, 1, fsize, f) != fsize) {
        apuemu_free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Allocate NSF player via proxy (PSRAM on ESP32) */
    g_nsf_player = (nsf_player_t *)apuemu_malloc(sizeof(nsf_player_t));
    if (!g_nsf_player) {
        printf("NSF: Failed to allocate player (%zu bytes)\n", sizeof(nsf_player_t));
        apuemu_free(data);
        return -1;
    }

    int ret = nsf_player_load_mem(g_nsf_player, data, (uint32_t)fsize);
    apuemu_free(data);

    if (ret != 0) {
        apuemu_free(g_nsf_player);
        g_nsf_player = NULL;
        return -1;
    }

    return 0;
}

void audio_check_impl(void)
{
    printf("audio_check_impl on core %d\n", xPortGetCoreID());

    /* APU and audio output hardware init */
    apuif_init();

    /* LittleFS is mounted by app_main before tasks start */

    if (load_nsf_from_file(NSF_FILE_PATH) == 0) {
        nsf_player_start(g_nsf_player, 0);
        printf("NSF loaded: %d songs\n", g_nsf_player->header.total_songs);
    } else {
        printf("NSF: No file at %s, APU will be silent\n", NSF_FILE_PATH);
    }

    /* 60Hz timing */
    const uint64_t target_frame_time_us = 16667;
    uint64_t next_frame_time = esp_timer_get_time();
    uint32_t frame_count = 0;

    while (true) {
        uint64_t frame_start = esp_timer_get_time();

        update_audio();

        frame_count++;

        /* Calculate next frame time */
        next_frame_time += target_frame_time_us;

        /* Sleep until next frame */
        uint64_t frame_end = esp_timer_get_time();
        int64_t sleep_time_us = next_frame_time - frame_end;

        if (sleep_time_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS(sleep_time_us / 1000));
        } else if (sleep_time_us < 0) {
            next_frame_time = esp_timer_get_time();
        }
    }
}
