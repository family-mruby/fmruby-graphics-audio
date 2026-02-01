# fmruby-graphics-audio

Targeting ESP32-WROVER-E/IE, provides the following two functions:
- Video
  - NTSC-J output using LovyanGFX
- Audio
  - I2S output using apu_emu

These functions are used via SPI RPC from Family murby core running on a separate ESP32-S3.

API specification is under consideration.

## Build Instructions

### Prerequisites

1. Initialize git submodules:
   ```bash
   git submodule update --init --recursive
   ```

2. Docker (for ESP-IDF build environment)

### Building

#### For Linux (SDL2 simulation):
```bash
rake build:linux
./build/fmruby-graphics-audio.elf
```

#### For ESP32:
```bash
rake build:esp32
rake flash:esp32
```

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

For third-party software licenses, see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).