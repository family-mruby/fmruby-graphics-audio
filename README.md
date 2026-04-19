# fmruby-graphics-audio

Companion firmware for Family mruby. Targets ESP32-WROVER-E/IE and provides:

- **Video** — NTSC-J output via the CVBS backend of LovyanGFX
- **Audio** — I2S output driven by `apu_emu` (NES-style APU emulator)

These functions are consumed by [fmruby-core](../fmruby-core/) running on a separate ESP32-S3 over a serial link. The default transport is **UART at 921600 bps**; an SPI transport is also available (`-DFMRB_COMM_TRANSPORT=SPI`). The wire protocol is framed with COBS and payloads are encoded with MessagePack.

For local development this repository also ships `sdl2-display/`, a standalone SDL2 host that renders the shared-memory framebuffer and forwards HID input when building for Linux.

## Build Requirements

- **Docker** — the ESP-IDF v5.5.1 image (`ghcr.io/family-mruby/fmruby-esp32-build:latest`) is used for both ESP32 and Linux builds.
- **Ruby** — the build driver is a `Rakefile`.
- **Git submodules** — LovyanGFX and other vendored components:
  ```bash
  git submodule update --init --recursive
  ```

## Building

```bash
# ESP32 (ESP32-WROVER-E/IE) — default UART link
rake build:esp32

# ESP32 with SPI link instead of UART
CMAKE_OPTS="-DFMRB_COMM_TRANSPORT=SPI" rake build:esp32

# Linux simulation build (ESP-IDF with IDF_TARGET=linux)
rake build:linux

# Show all available tasks
rake -T
```

Patches in [patches/](patches/) (LovyanGFX, esp_littlefs) are applied automatically before each build via the `apply_patches` task.

## Flashing and monitoring on ESP32

```bash
rake check-port   # detect & cache the serial port once
rake flash        # flash the current build
rake monitor      # open idf.py monitor
rake menuconfig   # open idf.py menuconfig
```

## Running the Linux simulation

The firmware renders into a shared-memory framebuffer and reads HID events from a Unix socket. A separate SDL2 host process (no FreeRTOS dependency) handles the actual window, input, and audio output.

```bash
# 1. Build and start the SDL2 host
cd sdl2-display
make
./fmrb_sdl2_display &

# 2. Run the firmware in Linux mode
cd ..
./build/fmruby-graphics-audio.elf
```

`fmruby-core` can then connect over the socket-based link that replaces UART/SPI in the Linux simulation. See the repository root [README.md](../README.md) for the `docker compose up` integration that wires core, graphics-audio, and sdl2-display together.

Design notes for the SDL2 split live in [doc/sdl2-process-separation.md](doc/sdl2-process-separation.md); the audio-on-Linux plan is in [doc/audio-linux-plan.md](doc/audio-linux-plan.md).

## Cleaning

```bash
rake clean       # remove build/
rake clean_all   # also remove sdkconfig (forces target reconfigure)
```

## License

This project is licensed under the GNU General Public License v3.0 — see the [LICENSE](LICENSE) file for details.

For third-party software licenses, see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
