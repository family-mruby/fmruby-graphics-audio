#pragma once

/**
 * @file audio_latency.h
 * @brief Latency statistics for real-time note commands.
 *
 * Measures how long a note_on / note_off takes from arriving at this chip to
 * being written into the APU. That covers the wait in the message buffer and
 * the handler itself; the core side of the link (app VM, kernel queue, UART
 * transit) is not visible from here and has to be measured separately.
 *
 * Reported in the same shape as the input latency counters on the core side,
 * so the same reading habits apply:
 *
 *   audio_note_lat: n=1000 avg_us=210 max_us=3120 sum_ms=210 ge1=12 ge5=0
 *
 * n counts the notes in the window, ge1 and ge5 how many took at least 1 ms
 * and 5 ms. A MIDI transport wants the bulk of them well under a millisecond
 * and, more importantly, wants max_us to stay bounded: a single late note is
 * audible where a slightly higher average is not.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record one note command.
 * @param rx_us   When the message arrived (fmrb_now_us() at decode time)
 * @param note_on true for note_on, false for note_off
 *
 * Emits the periodic summary when the window is full. Cheap enough to call
 * on every note.
 */
void audio_latency_record(uint64_t rx_us, bool note_on);

/**
 * @brief Emit the summary now, if anything has been recorded.
 *
 * Called from the audio task so a burst that stops before filling a window
 * still gets reported.
 */
void audio_latency_flush(void);

#ifdef __cplusplus
}
#endif
