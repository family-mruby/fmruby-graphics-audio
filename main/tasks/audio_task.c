#include "audio_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_handler.h"
#include "audio_latency.h"
#include "apu_if.h"
#include "apu_helper.h"
#include "nsf_player.h"
#include "fmsq_player.h"
#include "fmrb_wav.h"
#include "freertos/semphr.h"

#include <stdio.h>
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
/* One FMSQ player per APU instance: g_fmsq_players[0]=MAIN (mixed with
 * NSF), g_fmsq_players[1]=SUB (mixed with note_on/off SE). The two run
 * independently so a BGM on MAIN can play concurrently with a short
 * FMSQ SE on SUB. */
static fmsq_player_t *g_fmsq_players[2] = { NULL, NULL };

/* ------------------------------------------------------------------ */
/* WAV playback (play_wav): one clip mixed on top of the APU           */
/* ------------------------------------------------------------------ */
/*
 * Unlike the players above, this one does need a mutex. They are only ever
 * handed a pointer or a flag, but starting a clip FREES the buffer the mix
 * loop is reading from, so the two have to be kept apart. The lock is held
 * only around the swap and the per-frame mix, both of which are short.
 *
 * The clip is read whole rather than streamed: these are notification sounds
 * and short spoken lines, and a reader task feeding a ring would be a second
 * timing loop to keep in step with the APU's. FMRB_WAV_MAX_BYTES (2 MB) is
 * about 60 s at 16 kHz.
 */
static int16_t *g_wav_pcm = NULL;      /* owns the samples */
static fmrb_wav_stream_t g_wav_stream; /* reads g_wav_pcm */
static SemaphoreHandle_t g_wav_lock = NULL;

/* Created once, before the task loop starts feeding frames. Until then the
 * lock helpers are no-ops, which is correct: nothing can be playing yet. */
static void wav_init(void) {
    if (!g_wav_lock) g_wav_lock = xSemaphoreCreateMutex();
}

static void wav_lock(void) {
    if (g_wav_lock) xSemaphoreTake(g_wav_lock, portMAX_DELAY);
}

static void wav_unlock(void) {
    if (g_wav_lock) xSemaphoreGive(g_wav_lock);
}

static void wav_release_locked(void) {
    fmrb_wav_stream_stop(&g_wav_stream);
    if (g_wav_pcm) {
        apuemu_free(g_wav_pcm);
        g_wav_pcm = NULL;
    }
}

/* Add the playing clip to one frame the APU just produced. Called from every
 * place a frame is written out, so a clip keeps its shape whether the frame
 * came from the 60 Hz loop or from one of the immediate pushes. */
static void wav_mix_frame(int16_t *buf, int count) {
    if (count <= 0) return;
    /* Nothing playing is the normal state, and it costs nothing to say so:
     * the read is a plain load, and losing a race with a play_wav that has
     * not published yet only delays the clip by one 16 ms frame. */
    if (!g_wav_stream.playing) return;
    wav_lock();
    fmrb_wav_stream_mix(&g_wav_stream, buf, count);
    wav_unlock();
}

int audio_task_play_wav(const char *path) {
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "play_wav: empty path");
        return -1;
    }

    char full_path[256];
    const char *sep = path[0] == '/' ? "" : "/";
#ifdef CONFIG_IDF_TARGET_LINUX
    snprintf(full_path, sizeof(full_path), "flash%s%s", sep, path);
#else
    snprintf(full_path, sizeof(full_path), "/flash%s%s", sep, path);
#endif

    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "play_wav: cannot open %s", full_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* Checked before reading: a file too big to play is also too big to hold. */
    if (fsize <= 0 || (uint32_t)fsize > FMRB_WAV_MAX_BYTES) {
        ESP_LOGW(TAG, "play_wav: %s is %ld bytes (limit %u)",
                 full_path, fsize, (unsigned)FMRB_WAV_MAX_BYTES);
        fclose(fp);
        return -1;
    }

    uint8_t *raw = (uint8_t *)apuemu_malloc((uint32_t)fsize);
    if (!raw) {
        ESP_LOGE(TAG, "play_wav: out of memory for %ld bytes", fsize);
        fclose(fp);
        return -1;
    }
    size_t rd = fread(raw, 1, (size_t)fsize, fp);
    fclose(fp);
    if (rd != (size_t)fsize) {
        ESP_LOGW(TAG, "play_wav: short read %zu/%ld on %s", rd, fsize, full_path);
        apuemu_free(raw);
        return -1;
    }

    fmrb_wav_info_t info;
    fmrb_wav_err_t err = fmrb_wav_parse(raw, (size_t)fsize, &info);
    if (err != FMRB_WAV_OK) {
        ESP_LOGW(TAG, "play_wav: %s rejected: %s", full_path, fmrb_wav_strerror(err));
        apuemu_free(raw);
        return -1;
    }

    /* Move the samples to the front of the allocation so they are aligned for
     * int16 reads regardless of where the data chunk happened to start. */
    memmove(raw, raw + info.data_offset, (size_t)info.frames * 2u);

    wav_lock();
    wav_release_locked();
    g_wav_pcm = (int16_t *)raw;
    fmrb_wav_stream_start(&g_wav_stream, g_wav_pcm, info.frames,
                          info.sample_rate, FMRB_APU_MIX_RATE);
    wav_unlock();

    ESP_LOGI(TAG, "play_wav: %s (%lu frames @ %lu Hz)", full_path,
             (unsigned long)info.frames, (unsigned long)info.sample_rate);
    return 0;
}

int audio_task_stop_wav(void) {
    wav_lock();
    wav_release_locked();
    wav_unlock();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Common functions (platform-independent APU operations)              */
/* ------------------------------------------------------------------ */

int audio_task_nsf_play(const char *path, int track) {
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
    /* NSF takes MAIN, so whatever FMSQ was on MAIN has to give way -- the
     * mirror of play_slot(MAIN) stopping the NSF below. Two players writing
     * one instance is not a mix, it is one set of registers fought over. */
    if (g_fmsq_players[0] && g_fmsq_players[0]->playing) {
        g_fmsq_players[0]->playing = 0;
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
    nsf_player_start(g_nsf_player, track);
    return 0;
}

/* "stop" means the caller wants the sound to end, whichever player is making
 * it. Stopping only the NSF used to be enough because an FMSQ could only be
 * on SUB and an effect killed it; now a BGM can hold MAIN, and one left
 * running outlives the app that started it -- the app's own stop, its Ctrl+Q
 * and the kernel's cleanup after a crash all arrive here, and all three did
 * nothing to it. Nothing at all once the boot-time NSF preload went away:
 * with no NSF ever played there is no player to stop and the silencing write
 * never ran either. */
void audio_task_stop_all(void) {
    ESP_LOGI(TAG, "audio stop");

    if (g_nsf_player) {
        g_nsf_player->playing = 0;
    }
    if (g_fmsq_players[0]) {
        g_fmsq_players[0]->playing = 0;
    }
    if (g_fmsq_players[1]) {
        g_fmsq_players[1]->playing = 0;
    }

    /* Silence the channels of both instances. Unconditional: the players
     * above hold notes in the APU, so clearing their flags alone leaves the
     * last one sounding. */
    apuif_select(APUIF_INSTANCE_MAIN);
    apuif_write_reg(0x4015, 0x00);
    apuif_select(APUIF_INSTANCE_SUB);
    apuif_write_reg(0x4015, 0x00);
}

int audio_task_fmsq_play_slot(uint32_t music_id, uint8_t instance) {
    const uint8_t *data = NULL;
    uint32_t size = 0;

    if (instance > 1) {
        ESP_LOGE(TAG, "FMSQ play_slot: invalid instance %u", instance);
        return -1;
    }

    if (audio_handler_get_track(music_id, &data, &size) != 0) {
        ESP_LOGE(TAG, "FMSQ play_slot: track %lu not found", (unsigned long)music_id);
        return -1;
    }

    fmsq_player_t *player = g_fmsq_players[instance];
    if (!player) {
        player = (fmsq_player_t *)apuemu_malloc(sizeof(fmsq_player_t));
        if (!player) {
            ESP_LOGE(TAG, "FMSQ play_slot: malloc failed");
            return -1;
        }
        memset(player, 0, sizeof(fmsq_player_t));
        g_fmsq_players[instance] = player;
    }

    /* Stop current playback on this instance */
    player->playing = 0;

    int apu_inst = (instance == 0) ? APUIF_INSTANCE_MAIN : APUIF_INSTANCE_SUB;

    /* NSF lives on MAIN as well; stop it if we're about to overwrite MAIN.
     * SUB has no other shared user (note_on/off only writes when called),
     * so we don't need to stop anything when targeting SUB. */
    if (instance == 0 && g_nsf_player && g_nsf_player->playing) {
        g_nsf_player->playing = 0;
    }

    apuif_select(apu_inst);

#ifdef __linux__
    /* Flush ring buffers to minimize playback latency (Linux only) */
    apuif_ring_flush();
    audio_handler_flush();
#endif

    if (fmsq_player_load_from_memory(player, data, size) != 0) {
        ESP_LOGE(TAG, "FMSQ play_slot: load failed for track %lu", (unsigned long)music_id);
        return -1;
    }

    fmsq_player_reset(player);
    ESP_LOGI(TAG, "FMSQ play_slot: track %lu on %s",
             (unsigned long)music_id, instance == 0 ? "MAIN" : "SUB");

#ifdef __linux__
    /* Immediately generate and push first frame to minimize latency */
    {
        int16_t buf[(NTSC_SAMPLE + 1) * 2];
        memset(buf, 0, sizeof(buf));
        int count = apuif_process_mix(buf, sizeof(buf) / sizeof(buf[0]));
        wav_mix_frame(buf, count);
        if (count > 0) {
            apuif_audio_write(buf, count, 1);
        }
        audio_handler_push_samples();
    }
#endif

    return 0;
}

int audio_task_note_on(uint8_t channel, uint16_t freq, uint8_t volume, uint8_t duty, uint8_t sweep) {
    /* freq is a pitch in Hz for the tone channels, where 0 is meaningless,
     * but on noise it is a period index (0..15, plus the mode bit) where 0 is
     * the shortest period - a legal note. Rejecting 0 for every channel
     * silently dropped every noise hit at period 0. */
    if (freq == 0 && channel != FMRB_APU_CH_NOISE) return -1;

    /* note_on uses SUB; FMSQ/NSF stay on MAIN. Both APU instances are
     * mixed by apuif_process_mix, so BGM keeps playing during SE. */
    apuif_select(APUIF_INSTANCE_SUB);

#ifdef __linux__
    apuif_ring_flush();
    audio_handler_flush();  /* Also flush SHM ring buffer */
#endif

    switch (channel) {
    case FMRB_APU_CH_PULSE1:
    case FMRB_APU_CH_PULSE2:
        apu_pulse_note_on(channel, freq, volume, duty, sweep);
        break;
    case FMRB_APU_CH_TRIANGLE:
        apu_triangle_note_on(freq);
        break;
    case FMRB_APU_CH_NOISE:
        apu_noise_note_on(freq & 0x0F, (freq & 0x80) ? 1 : 0, volume);
        break;
    default:
        return -1;
    }

    ESP_LOGD(TAG, "note_on: ch=%d freq=%d vol=%d duty=%d", channel, freq, volume & 0x0F, duty);

#ifdef __linux__
    /* Generate one frame of samples and push immediately to SHM
     * to avoid waiting up to 16ms for the next 60Hz push cycle */
    {
        int16_t buf[(NTSC_SAMPLE + 1) * 2];
        memset(buf, 0, sizeof(buf));
        int count = apuif_process_mix(buf, sizeof(buf) / sizeof(buf[0]));
        wav_mix_frame(buf, count);
        if (count > 0) {
            apuif_audio_write(buf, count, 1);
        }
        audio_handler_push_samples();
    }
#endif

    return 0;
}

int audio_task_note_off(uint8_t channel) {
    apuif_select(APUIF_INSTANCE_SUB);

    switch (channel) {
    case FMRB_APU_CH_PULSE1:
    case FMRB_APU_CH_PULSE2:
        apu_pulse_note_off(channel);
        break;
    case FMRB_APU_CH_TRIANGLE:
        apu_triangle_note_off();
        break;
    case FMRB_APU_CH_NOISE:
        apu_noise_note_off();
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

#define FMSQ_FILE_PATH "/project/flash/data/test.fmsq"

void audio_task(void *pvParameters) {
    wav_init();
    ESP_LOGI(TAG, "Audio task started (Linux)");

    /* Initialize SDL2 audio handler */
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed");
        return;
    }

    /* Initialize main APU (instance 0: NSF) */
    apuif_init();

    /* No NSF is loaded here: a play command always names the file it wants,
     * and the play path allocates the player itself. */

    /* Initialize sub APU (instance 1: note_on/off SE and optional SE-FMSQ) */
    apuif_init_sub();

    /* Preload the default test FMSQ into the MAIN slot. SUB slot stays
     * NULL until an app calls play_slot with instance=1. */
    g_fmsq_players[0] = (fmsq_player_t *)apuemu_malloc(sizeof(fmsq_player_t));
    if (g_fmsq_players[0]) {
        if (fmsq_player_load(g_fmsq_players[0], FMSQ_FILE_PATH) == 0) {
            fmsq_player_reset(g_fmsq_players[0]);
            g_fmsq_players[0]->playing = 0;  /* Wait for play command */
            ESP_LOGI(TAG, "FMSQ loaded: %d frames (waiting for play command)", g_fmsq_players[0]->frame_count);
        } else {
            ESP_LOGW(TAG, "No FMSQ file at %s", FMSQ_FILE_PATH);
            apuemu_free(g_fmsq_players[0]);
            g_fmsq_players[0] = NULL;
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

        /* Tick FMSQ players on whichever APU instances they were started
         * on. instance 0 = MAIN (with NSF), instance 1 = SUB (with SE). */
        for (int i = 0; i < 2; i++) {
            if (g_fmsq_players[i] && g_fmsq_players[i]->playing) {
                apuif_select(i == 0 ? APUIF_INSTANCE_MAIN : APUIF_INSTANCE_SUB);
                fmsq_player_tick(g_fmsq_players[i]);
            }
        }

        /* Process both APU instances and mix output */
        int16_t buffer[(NTSC_SAMPLE + 1) * 2];
        memset(buffer, 0, sizeof(buffer));
        int count = apuif_process_mix(buffer, sizeof(buffer) / sizeof(buffer[0]));
        wav_mix_frame(buffer, count);
        if (count > 0) {
            apuif_audio_write(buffer, count, 1);
        }

#ifdef CONFIG_IDF_TARGET_LINUX
        /* Transfer APU ring buffer samples to SHM for SDL2 process */
        audio_handler_push_samples();
        audio_latency_flush();
#endif

        vTaskDelay(pdMS_TO_TICKS(frame_interval_ms));
    }

    for (int i = 0; i < 2; i++) {
        if (g_fmsq_players[i]) {
            fmsq_player_free(g_fmsq_players[i]);
            apuemu_free(g_fmsq_players[i]);
            g_fmsq_players[i] = NULL;
        }
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

void audio_task(void *pvParameters) {
    wav_init();
    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());

    /* Initialize main APU (instance 0: NSF) */
    apuif_init();

    /* Initialize sub APU (instance 1: FMSQ + note_on/off) */
    apuif_init_sub();

    /* No NSF is loaded here: a play command always names the file it wants,
     * and the play path allocates the player itself. */

    /* 60Hz timing */
    const uint64_t target_frame_time_us = 16667;
    uint64_t next_frame_time = esp_timer_get_time();

    while (task_running) {
        /* Tick NSF player on main APU instance */
        if (g_nsf_player && g_nsf_player->playing) {
            apuif_select(APUIF_INSTANCE_MAIN);
            nsf_player_tick(g_nsf_player);
        }

        /* Tick FMSQ players on whichever APU instances they were started
         * on. instance 0 = MAIN (with NSF), instance 1 = SUB (with SE). */
        for (int i = 0; i < 2; i++) {
            if (g_fmsq_players[i] && g_fmsq_players[i]->playing) {
                apuif_select(i == 0 ? APUIF_INSTANCE_MAIN : APUIF_INSTANCE_SUB);
                fmsq_player_tick(g_fmsq_players[i]);
            }
        }

        /* Process both APU instances and mix output */
        int16_t buffer[(NTSC_SAMPLE + 1) * 2];
        memset(buffer, 0, sizeof(buffer));
        int count = apuif_process_mix(buffer, sizeof(buffer) / sizeof(buffer[0]));
        wav_mix_frame(buffer, count);
        if (count > 0) {
            apuif_audio_write(buffer, count, 1);
        }

        audio_latency_flush();

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

    for (int i = 0; i < 2; i++) {
        if (g_fmsq_players[i]) {
            fmsq_player_free(g_fmsq_players[i]);
            apuemu_free(g_fmsq_players[i]);
            g_fmsq_players[i] = NULL;
        }
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
