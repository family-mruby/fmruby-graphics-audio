#ifndef _APU_C_H_
#define _APU_C_H_

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

#ifndef __linux__
#include "fmrb_pin_assign.h"

#define USE_I2S

#ifdef USE_I2S
#define PIN_BCK   FMRB_PIN_I2S_BCK
#define PIN_WS    FMRB_PIN_I2S_WS
#define PIN_DOUT  FMRB_PIN_I2S_DOUT
#else
#define AUDIO_PIN   FMRB_PIN_AUDIO_PWM
#endif
#endif /* !__linux__ */

/* APU event types */
typedef enum {
    APU_EVENT_WRITE = 0,
    APU_EVENT_INIT_START,
    APU_EVENT_INIT_END,
    APU_EVENT_PLAY_START,
    APU_EVENT_PLAY_END
} apu_event_type_t;

/* Binary file format header */
typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint32_t frame_count;
    uint32_t reserved[3];
} apu_log_header_t;

/* APU register write event */
typedef struct {
    int32_t time;
    uint16_t addr;
    uint8_t data;
    uint8_t event_type;
    uint32_t frame_number;
} apu_log_entry_t;

apu_log_entry_t* apuif_read_entries(const char* filename, apu_log_header_t* header);
int apuif_parse_apu_log(const char* filename);

void apuif_init();
int apuif_frame_sample_count();
int apuif_process(int16_t* buff, int len);
void apuif_write_reg(uint32_t address, uint8_t value);
uint8_t apuif_read_reg(uint32_t address);

void apuif_audio_write(const int16_t* s, int len, int channels);
int apuif_use_external_process();
void apuif_set_external_process(int flag);

#ifdef __linux__
/* Read samples from ring buffer (Linux/SDL2 only) */
int apuif_ring_read(int16_t* out, int count);
#endif

/*
 * Dual APU instance support for simultaneous playback.
 * Instance 0: default (NSF/main music)
 * Instance 1: secondary (FMSQ/effects)
 *
 * Before writing to an APU instance, call apuif_select(n).
 * apuif_process_mix() processes both instances and mixes the output.
 */
#define APUIF_INSTANCE_MAIN   0
#define APUIF_INSTANCE_SUB    1
#define APUIF_INSTANCE_MAX    2

/* Initialize secondary APU instance */
void apuif_init_sub(void);

/* Select which APU instance receives write_reg calls */
void apuif_select(int instance);

/* Process both APU instances and mix into output buffer.
 * Returns number of samples written. */
int apuif_process_mix(int16_t* buff, int len);

/*
 * Memory allocation proxy for apu_emu component.
 * On ESP32, these map to heap_caps_malloc (PSRAM).
 * On Linux, these map to standard malloc/free.
 * Implemented in apu_if_linux.c / apu_if.cpp respectively.
 */
void *apuemu_malloc(uint32_t size);
void  apuemu_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif //_APU_C_H_

