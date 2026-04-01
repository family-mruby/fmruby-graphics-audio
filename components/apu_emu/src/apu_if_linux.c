/*
 * APU interface for Linux/SDL2 simulation
 *
 * Provides the same apu_if.h API as the ESP32 version (apu_if.cpp),
 * but outputs audio samples to a ring buffer instead of I2S hardware.
 * The ring buffer is consumed by the SDL2 audio callback.
 */

#include "apu_if.h"
#include "noftypes.h"
#include "nes_apu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static apu_t* _apu = NULL;
static int _audio_frequency = 0;
static int _audio_frame_samples = 0;
static int _audio_fraction = 0;
static int _initialized = 0;
static volatile int _use_external_process = 0;

/* Ring buffer for audio samples (shared with SDL2 callback) */
#define AUDIO_RING_SIZE 8192
static int16_t _ring_buffer[AUDIO_RING_SIZE];
static volatile uint32_t _ring_read = 0;
static volatile uint32_t _ring_write = 0;

void apuif_init(void)
{
    if (_initialized) return;

    _audio_frequency = 15720; /* NTSC */
    _audio_frame_samples = (_audio_frequency << 16) / 60; /* fixed point */
    _audio_fraction = 0;

    _apu = apu_create(0, _audio_frequency, 60, 8);

    _ring_read = 0;
    _ring_write = 0;

    _initialized = 1;
    printf("APU initialized for Linux: freq=%d\n", _audio_frequency);
}

int apuif_frame_sample_count(void)
{
    int n = _audio_frame_samples + _audio_fraction;
    _audio_fraction = n & 0xFFFF;
    return n >> 16;
}

int apuif_process(int16_t* buff, int len)
{
    int n = apuif_frame_sample_count();
    if (n > len) {
        printf("bad buffer size %d > %d\n", n, len);
        return -1;
    }

    apu_process(buff, n);

    /* Convert unsigned 8-bit samples to signed 16-bit */
    uint8_t* b8 = (uint8_t*)buff;
    for (int i = n - 1; i >= 0; i--) {
        buff[i] = (b8[i] ^ 0x80) << 8;
    }
    return n;
}

void apuif_write_reg(uint32_t address, uint8_t value)
{
    apu_write(address, value);
}

uint8_t apuif_read_reg(uint32_t address)
{
    return apu_read(address);
}

void apuif_audio_write(const int16_t* s, int len, int channels)
{
    /*
     * Write mono samples to ring buffer.
     * SDL2 audio callback reads from this buffer.
     * No lock needed here because single producer (audio_task) and
     * single consumer (SDL2 callback), with power-of-2 buffer size.
     */
    for (int i = 0; i < len; i++) {
        int16_t sample;
        if (channels == 2) {
            sample = (s[i * 2] + s[i * 2 + 1]) / 2;
        } else {
            sample = s[i];
        }

        uint32_t next_w = (_ring_write + 1) & (AUDIO_RING_SIZE - 1);
        if (next_w == (_ring_read & (AUDIO_RING_SIZE - 1))) {
            /* Buffer full, drop sample */
            break;
        }
        _ring_buffer[_ring_write & (AUDIO_RING_SIZE - 1)] = sample;
        _ring_write++;
    }
}

/*
 * Called by SDL2 audio callback to read samples from ring buffer.
 * Returns number of samples read.
 */
int apuif_ring_read(int16_t* out, int count)
{
    int read = 0;
    for (int i = 0; i < count; i++) {
        if (_ring_read == _ring_write) {
            /* Buffer empty, output silence */
            out[i] = 0;
        } else {
            out[i] = _ring_buffer[_ring_read & (AUDIO_RING_SIZE - 1)];
            _ring_read++;
            read++;
        }
    }
    return read;
}

int apuif_use_external_process(void)
{
    return _use_external_process;
}

void apuif_set_external_process(int flag)
{
    _use_external_process = flag;
}

apu_log_entry_t* apuif_read_entries(const char* filename, apu_log_header_t* header)
{
    FILE* file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return NULL;
    }

    if (fread(header, sizeof(apu_log_header_t), 1, file) != 1) {
        fprintf(stderr, "Error: Failed to read header\n");
        fclose(file);
        return NULL;
    }

    if (memcmp(header->magic, "APULOG\0\0", 8) != 0) {
        fprintf(stderr, "Error: Invalid file format (bad magic)\n");
        fclose(file);
        return NULL;
    }

    printf("=== APU Binary Log File ===\n");
    printf("File: %s\n", filename);
    printf("Format version: %u\n", header->version);
    printf("Entry count: %u\n", header->entry_count);
    printf("Frame count: %u\n", header->frame_count);
    printf("\n");

    if (header->entry_count == 0) {
        printf("No entries in log file.\n");
        fclose(file);
        return NULL;
    }

    apu_log_entry_t* entries = (apu_log_entry_t*)malloc(
        header->entry_count * sizeof(apu_log_entry_t));
    if (!entries) {
        fprintf(stderr, "Error: Failed to allocate memory for entries\n");
        fclose(file);
        return NULL;
    }

    size_t entries_read = fread(entries, sizeof(apu_log_entry_t),
                                header->entry_count, file);
    if (entries_read != header->entry_count) {
        fprintf(stderr, "Error: Expected %u entries, read %zu\n",
                header->entry_count, entries_read);
        free(entries);
        fclose(file);
        return NULL;
    }

    fclose(file);
    return entries;
}

int apuif_parse_apu_log(const char* filename)
{
    /* Minimal implementation for Linux - just validate the file */
    apu_log_header_t header;
    apu_log_entry_t* entries = apuif_read_entries(filename, &header);
    if (!entries) return -1;
    free(entries);
    return 0;
}
