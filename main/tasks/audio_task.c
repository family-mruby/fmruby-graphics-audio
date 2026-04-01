#include "audio_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_handler.h"
#include "apu_if.h"

#include <string.h>

static const char *TAG = "audio_task";
static volatile int task_running = 1;

void audio_task_stop(void) {
    task_running = 0;
}

#ifdef CONFIG_IDF_TARGET_LINUX

/* Replay state for reglog playback on Linux */
static apu_log_header_t _replay_header;
static apu_log_entry_t* _replay_entries = NULL;
static int _replay_play_head = 0;
static int _replay_entry_idx = 0;
static int _replay_initialized = 0;

#define NTSC_SAMPLE 262
#define REGLOG_FILE_PATH "/project/flash/data/sample.reglog"

static int replay_seek_play_head(void)
{
    for (uint32_t i = 0; i < _replay_header.entry_count; i++) {
        if (_replay_entries[i].event_type == APU_EVENT_INIT_END) {
            return i + 1;
        }
    }
    return -1;
}

static void replay_exec_init(void)
{
    _replay_play_head = replay_seek_play_head();
    if (_replay_play_head < 0) {
        ESP_LOGE(TAG, "PLAY entry not found in reglog");
        return;
    }
    for (int i = 0; i < _replay_play_head; i++) {
        const apu_log_entry_t* entry = &_replay_entries[i];
        if (entry->event_type == APU_EVENT_WRITE) {
            apuif_write_reg(entry->addr, entry->data);
        }
    }
    _replay_entry_idx = _replay_play_head;
    _replay_initialized = 1;
    ESP_LOGI(TAG, "Replay init done, play_head=%d", _replay_play_head);
}

static void replay_exec_frame(void)
{
    for (uint32_t i = _replay_entry_idx + 1; i < _replay_header.entry_count; i++) {
        const apu_log_entry_t* entry = &_replay_entries[i];
        switch (entry->event_type) {
            case APU_EVENT_WRITE:
                apuif_write_reg(entry->addr, entry->data);
                break;
            case APU_EVENT_PLAY_START:
                _replay_entry_idx = i;
                return;
            case APU_EVENT_PLAY_END:
                _replay_entry_idx = i + 1;
                return;
            default:
                break;
        }
    }
    /* Loop: restart from play_head */
    _replay_entry_idx = _replay_play_head;
}

void audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started (Linux)");

    /* Initialize SDL2 audio handler */
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed");
        return;
    }

    /* Initialize APU emulator */
    apuif_init();

    /* Try to load reglog file */
    _replay_entries = apuif_read_entries(REGLOG_FILE_PATH, &_replay_header);
    if (_replay_entries) {
        ESP_LOGI(TAG, "Loaded reglog: %u entries, %u frames",
                 _replay_header.entry_count, _replay_header.frame_count);
        replay_exec_init();
    } else {
        ESP_LOGW(TAG, "No reglog file found at %s, APU will be silent until RPC commands arrive",
                 REGLOG_FILE_PATH);
    }

    /* 60Hz audio processing loop */
    const uint32_t frame_interval_ms = 16; /* ~60Hz */

    while (task_running) {
        if (_replay_entries && _replay_initialized) {
            replay_exec_frame();
        }

        int16_t buffer[(NTSC_SAMPLE + 1) * 2];
        memset(buffer, 0, sizeof(buffer));
        int count = apuif_process(buffer, sizeof(buffer));
        if (count > 0) {
            apuif_audio_write(buffer, count, 1);
        }

        vTaskDelay(pdMS_TO_TICKS(frame_interval_ms));
    }

    if (_replay_entries) {
        free(_replay_entries);
        _replay_entries = NULL;
    }
    audio_handler_cleanup();

    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}

#else /* ESP32 */

void audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started on core %d", xPortGetCoreID());
    audio_check_impl();
}

#endif /* CONFIG_IDF_TARGET_LINUX */
