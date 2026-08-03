#include "audio_latency.h"
#include "fmrb_time.h"
#include "esp_log.h"

static const char *TAG = "audio_lat";

/* Report every WINDOW notes, or sooner if a burst ends and the audio task
 * flushes. A window of 1000 matches the input latency counters on the core
 * side, so a busy passage reports a few times per second and an idle system
 * stays quiet. */
#define WINDOW_SAMPLES 1000
/* Do not report more often than this even if windows fill quickly. */
#define MIN_REPORT_INTERVAL_US (2 * 1000 * 1000)

static uint32_t s_count;
static uint32_t s_note_on;
static uint64_t s_sum_us;
static uint32_t s_max_us;
static uint32_t s_ge1;
static uint32_t s_ge5;
static uint64_t s_last_report_us;

static void report(uint64_t now_us) {
    if (s_count == 0) {
        return;
    }

    uint32_t avg_us = (uint32_t)(s_sum_us / s_count);
    ESP_LOGI(TAG, "audio_note_lat: n=%lu on=%lu avg_us=%lu max_us=%lu sum_ms=%lu ge1=%lu ge5=%lu",
             (unsigned long)s_count, (unsigned long)s_note_on,
             (unsigned long)avg_us, (unsigned long)s_max_us,
             (unsigned long)(s_sum_us / 1000ULL),
             (unsigned long)s_ge1, (unsigned long)s_ge5);

    s_count = 0;
    s_note_on = 0;
    s_sum_us = 0;
    s_max_us = 0;
    s_ge1 = 0;
    s_ge5 = 0;
    s_last_report_us = now_us;
}

void audio_latency_record(uint64_t rx_us, bool note_on) {
    uint64_t now = fmrb_now_us();
    /* A zero timestamp means the sender did not stamp the message (an older
     * comm path); counting it would report a latency of "since boot". */
    if (rx_us == 0 || now < rx_us) {
        return;
    }

    uint32_t elapsed = (uint32_t)(now - rx_us);
    s_count++;
    if (note_on) {
        s_note_on++;
    }
    s_sum_us += elapsed;
    if (elapsed > s_max_us) {
        s_max_us = elapsed;
    }
    if (elapsed >= 1000) {
        s_ge1++;
    }
    if (elapsed >= 5000) {
        s_ge5++;
    }

    if (s_count >= WINDOW_SAMPLES && (now - s_last_report_us) >= MIN_REPORT_INTERVAL_US) {
        report(now);
    }
}

void audio_latency_flush(void) {
    uint64_t now = fmrb_now_us();
    if (s_count > 0 && (now - s_last_report_us) >= MIN_REPORT_INTERVAL_US) {
        report(now);
    }
}
