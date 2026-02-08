#pragma once

#ifndef CONFIG_IDF_TARGET_LINUX
#include "driver/gpio.h"

// ========================================
// Family mruby Graphics-Audio
// Hardware Pin Assignment Definitions
// Target: ESP32-WROVER-E/IE
// ========================================

// ========================================
// SPI Communication Pins (ESP32-S3 Master Interface)
// ========================================
#define FMRB_PIN_SPI_MISO     18   // Slave Output (to Master Input)
#define FMRB_PIN_SPI_MOSI     21   // Slave Input (from Master Output)
#define FMRB_PIN_SPI_CLK      19   // SPI Clock
#define FMRB_PIN_SPI_CS       22   // SPI Chip Select

// ========================================
// I2S Audio Output Pins (apu_emu)
// ========================================
#define FMRB_PIN_I2S_BCK      GPIO_NUM_32  // I2S Bit Clock
#define FMRB_PIN_I2S_WS       GPIO_NUM_33  // I2S Word Select
#define FMRB_PIN_I2S_DOUT     GPIO_NUM_27  // I2S Data Out

// ========================================
// PWM Audio Output Pin (Fallback)
// ========================================
#define FMRB_PIN_AUDIO_PWM    26           // PWM Audio Output (when USE_I2S is not defined)

// ========================================
// CVBS Video Output Pin (LovyanGFX)
// ========================================
#define FMRB_PIN_CVBS_DAC     25           // NTSC-J DAC Output (GPIO 25 or 26 only)

#else
// Linux環境ではPIN定義不要
#endif
