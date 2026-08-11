/**
 * @file main.c
 * @brief SDL2 display process for Family mruby.
 *        Receives framebuffer from FreeRTOS process via shared memory,
 *        renders to SDL2 window, captures input events and sends via socket,
 *        plays audio from shared memory ring buffer.
 *
 *        This process has NO FreeRTOS dependency and NO SIGALRM interference.
 */
#define _POSIX_C_SOURCE 200112L
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <semaphore.h>

/* Shared header with FreeRTOS process */
#include "shm_display.h"
/* HID event definitions */
#include "fmrb_hid_event.h"

static volatile int g_running = 1;
static fmrb_shm_t *g_shm = NULL;
static int g_shm_fd = -1;
static sem_t *g_sem_frame = SEM_FAILED;
static int g_input_fd = -1;  /* Socket to send input events */

static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_texture = NULL;
static SDL_AudioDeviceID g_audio_device = 0;

/* RGB332 to RGB888 lookup table */
static uint8_t g_rgb332_to_rgb888[256 * 3];

static void init_rgb332_lut(void) {
    for (int i = 0; i < 256; i++) {
        int r = (i >> 5) & 0x07;
        int g = (i >> 2) & 0x07;
        int b = (i >> 0) & 0x03;
        g_rgb332_to_rgb888[i * 3 + 0] = (r * 255) / 7;
        g_rgb332_to_rgb888[i * 3 + 1] = (g * 255) / 7;
        g_rgb332_to_rgb888[i * 3 + 2] = (b * 255) / 3;
    }
}

/* ----- Signal handler ----- */
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ----- Shared memory setup ----- */
static int setup_shm(void) {
    /* Clean up stale SHM and semaphores from previous runs.
     * With ipc:host, /dev/shm persists across container restarts. */
    shm_unlink(FMRB_SHM_NAME);
    sem_unlink(FMRB_SEM_FRAME_NAME);

    /* Wait for FreeRTOS process to create shared memory */
    printf("[sdl2-display] Waiting for shared memory...\n");
    while (g_running) {
        g_shm_fd = shm_open(FMRB_SHM_NAME, O_RDWR, 0666);
        if (g_shm_fd >= 0) break;
        usleep(200000); /* 200ms */
    }
    if (!g_running) return -1;

    g_shm = (fmrb_shm_t *)mmap(NULL, sizeof(fmrb_shm_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        fprintf(stderr, "[sdl2-display] mmap failed: %s\n", strerror(errno));
        return -1;
    }

    /* Wait for FreeRTOS process to freshly initialize SHM (magic value).
     * This avoids reading stale data from a previous run. */
    printf("[sdl2-display] Waiting for FreeRTOS initialization...\n");
    while (g_running && g_shm->ready_magic != FMRB_SHM_READY_MAGIC) {
        usleep(100000);
    }
    if (!g_running) return -1;

    /* Open frame semaphore */
    while (g_running) {
        g_sem_frame = sem_open(FMRB_SEM_FRAME_NAME, 0);
        if (g_sem_frame != SEM_FAILED) break;
        usleep(100000);
    }
    if (!g_running) return -1;

    printf("[sdl2-display] Shared memory connected: %dx%d, %d-bit\n",
           g_shm->display_width, g_shm->display_height, g_shm->color_depth);
    return 0;
}

/* ----- Input socket setup (non-blocking, single attempt) ----- */
static int try_connect_input_socket(void) {
    if (g_input_fd >= 0) return 0; /* Already connected */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FMRB_INPUT_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        g_input_fd = fd;
        printf("[sdl2-display] Input socket connected\n");
        return 0;
    }

    close(fd);
    return -1;
}

/* Send an already-framed packet ([type][len16][payload]) to the input socket */
static void send_raw_input(const uint8_t *packet, size_t n) {
    if (g_input_fd < 0) return;

    ssize_t sent;
    do {
        sent = send(g_input_fd, packet, n, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        printf("[sdl2-display] Input socket disconnected\n");
        close(g_input_fd);
        g_input_fd = -1;
    }
}

static void send_input_event(uint8_t type, const void *data, uint16_t len) {
    uint8_t packet[256];
    if (3 + (size_t)len > sizeof(packet)) return;

    packet[0] = type;
    packet[1] = (uint8_t)(len & 0xFF);
    packet[2] = (uint8_t)((len >> 8) & 0xFF);
    if (data && len > 0) {
        memcpy(packet + 3, data, len);
    }
    send_raw_input(packet, 3 + len);
}

/* ----- Synthetic input injection (agent/CI) -----
 * A Unix DGRAM socket accepts pre-framed HID packets (same wire format as
 * send_input_event) and forwards them into the normal input stream, so
 * synthetic events are serialized with real SDL input. Sender:
 * family-mruby/tools/fmrb_input.py */
static int g_inject_fd = -1;

static void setup_inject_socket(void) {
    g_inject_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (g_inject_fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FMRB_INJECT_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(FMRB_INJECT_SOCKET_PATH);
    if (bind(g_inject_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[sdl2-display] inject socket bind failed: %s\n",
                strerror(errno));
        close(g_inject_fd);
        g_inject_fd = -1;
        return;
    }
    printf("[sdl2-display] Inject socket ready: %s\n", FMRB_INJECT_SOCKET_PATH);
}

static void drain_inject_events(void) {
    if (g_inject_fd < 0) return;

    uint8_t packet[256];
    for (int i = 0; i < 32; i++) {  /* bound per loop iteration */
        ssize_t n = recv(g_inject_fd, packet, sizeof(packet), 0);
        if (n < 0) break;  /* EAGAIN: no more datagrams */
        if (n < 3) continue;
        uint16_t len = (uint16_t)packet[1] | ((uint16_t)packet[2] << 8);
        if ((ssize_t)(3 + len) != n) continue;  /* malformed frame */
        send_raw_input(packet, (size_t)n);
    }
}

/* ----- SDL2 audio callback ----- */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    fmrb_shm_t *shm = g_shm;
    if (!shm) {
        memset(stream, 0, len);
        return;
    }

    int16_t *out = (int16_t *)stream;
    int total_samples = len / sizeof(int16_t);
    uint32_t ring_size = FMRB_SHM_AUDIO_RING_SIZE * 2;
    uint32_t rp = shm->audio_read_pos;
    int filled = 0;

    while (filled < total_samples) {
        if (rp == shm->audio_write_pos) {
            /* Ring empty, fill rest with silence */
            memset(&out[filled], 0, (total_samples - filled) * sizeof(int16_t));
            break;
        }
        out[filled++] = shm->audio_ring[rp];
        rp = (rp + 1) % ring_size;
    }

    shm->audio_read_pos = rp;
}

/* ----- SDL2 setup ----- */
static int setup_sdl2(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[sdl2-display] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    uint16_t w = g_shm->display_width;
    uint16_t h = g_shm->display_height;
    uint8_t sx = g_shm->scaling_x > 0 ? g_shm->scaling_x : 2;
    uint8_t sy = g_shm->scaling_y > 0 ? g_shm->scaling_y : 2;

    g_window = SDL_CreateWindow("Family mruby",
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                w * sx, h * sy,
                                SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        fprintf(stderr, "[sdl2-display] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        /* Headless runs (SDL_VIDEODRIVER=dummy, used by the agent/CI
         * screenshot flow) have no accelerated driver; fall back to the
         * software renderer. */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) {
        fprintf(stderr, "[sdl2-display] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB24,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_texture) {
        fprintf(stderr, "[sdl2-display] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Setup audio */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = FMRB_SHM_AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 256;  /* Small buffer for low latency (~16ms at 15720Hz) */
    want.callback = audio_callback;

    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                          SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (g_audio_device == 0) {
        fprintf(stderr, "[sdl2-display] Warning: Audio device failed: %s\n", SDL_GetError());
        /* Continue without audio */
    } else {
        SDL_PauseAudioDevice(g_audio_device, 0);
        printf("[sdl2-display] Audio: %d Hz, %d channels\n", have.freq, have.channels);
    }

    SDL_ShowCursor(SDL_DISABLE);

    printf("[sdl2-display] SDL2 initialized: %dx%d (window %dx%d)\n",
           w, h, w * sx, h * sy);
    return 0;
}

/* X11 hands the JIS mode keys over as LOCKING keys, and the firmware log plus
 * the SDL event log together spell out exactly what that means:
 *
 *   mode key sc=53 down repeat=0     <- press 1: the lock engages
 *   mode key sc=53 down repeat=1     <- ... and X11 believes the key is now
 *   mode key sc=53 down repeat=1        held, so auto-repeat runs forever
 *   ...
 *   mode key sc=53 up   repeat=0     <- press 2: the lock disengages
 *   mode key sc=53 down repeat=0     <- press 3, and around it goes
 *
 * So a physical press is either the first key-down or the key-up that ends the
 * hold, and everything in between is noise. A USB keyboard on the device sends
 * an ordinary press/release pair, and the firmware follows the USB rule (one
 * press is one key_down), so the simulator translates rather than the firmware
 * carrying an X11 quirk.
 *
 * Auto-repeat is what tells the two apart, with no timing guesswork: a normal
 * short press produces no repeats, so its release is just a release and is
 * dropped. (Holding one of these keys past the repeat delay counts as two
 * presses. On a key that selects an input mode, two presses cancel out.)
 *
 * Injected events (drain_inject_events) never come through here and are
 * already well-formed pairs.
 */
static int is_jis_mode_key(int scancode) {
    return scancode == SDL_SCANCODE_GRAVE ||          /* 0x35 half/full-width */
           scancode == SDL_SCANCODE_INTERNATIONAL2;   /* 0x88 katakana */
}

static void send_key(int type, const SDL_Keysym *keysym) {
    hid_keyboard_event_t kbd;
    kbd.scancode = (uint8_t)keysym->scancode;
    kbd.keycode = (uint8_t)(keysym->sym & 0xFF);
    kbd.modifier = (uint8_t)(keysym->mod & 0xFF);
    send_input_event(type, &kbd, sizeof(kbd));
}

static void send_key_tap(const SDL_Keysym *keysym) {
    send_key(HID_EVENT_KEY_DOWN, keysym);
    send_key(HID_EVENT_KEY_UP, keysym);
}

/* ----- Process SDL events and send as HID ----- */
static void process_sdl_events(uint8_t sx, uint8_t sy) {
    /* Auto-repeat seen since each mode key went down (GRAVE, INTERNATIONAL2). */
    static int mode_key_held[2];
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            g_running = 0;
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (is_jis_mode_key(event.key.keysym.scancode)) {
                /* Auto-repeat while the lock is engaged: not a press, and the
                 * flag is what makes the eventual key-up one. */
                int i = (event.key.keysym.scancode == SDL_SCANCODE_GRAVE) ? 0 : 1;
                if (event.type == SDL_KEYDOWN && event.key.repeat) {
                    mode_key_held[i] = 1;
                    break;
                }
                if (event.type == SDL_KEYUP && !mode_key_held[i]) {
                    break;  /* ordinary release of an ordinary press */
                }
                mode_key_held[i] = 0;
                fprintf(stderr, "[sdl2-display] mode key sc=%d %s -> press\n",
                        event.key.keysym.scancode,
                        event.type == SDL_KEYDOWN ? "down" : "up");
                send_key_tap(&event.key.keysym);
                break;
            }
            if (event.type == SDL_KEYDOWN && event.key.repeat) {
                break;
            }
            send_key(event.type == SDL_KEYDOWN ? HID_EVENT_KEY_DOWN
                                               : HID_EVENT_KEY_UP,
                     &event.key.keysym);
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            hid_mouse_button_event_t mouse;
            mouse.button = event.button.button;
            mouse.state = (event.type == SDL_MOUSEBUTTONDOWN) ? 1 : 0;
            mouse.x = (uint16_t)(event.button.x / sx);
            mouse.y = (uint16_t)(event.button.y / sy);
            send_input_event(HID_EVENT_MOUSE_BUTTON, &mouse, sizeof(mouse));
            break;
        }

        case SDL_MOUSEMOTION: {
            static uint32_t last_motion_ms = 0;
            uint32_t now = SDL_GetTicks();
            if (now - last_motion_ms >= 66) { /* ~15 Hz throttle */
                last_motion_ms = now;
                hid_mouse_motion_event_t motion;
                motion.x = (uint16_t)(event.motion.x / sx);
                motion.y = (uint16_t)(event.motion.y / sy);
                send_input_event(HID_EVENT_MOUSE_MOTION, &motion, sizeof(motion));
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ----- Render one frame from SHM to SDL2 ----- */
static void render_frame(uint16_t w, uint16_t h) {
    /* Read from the buffer that was just written (write_index was swapped) */
    uint32_t read_buf = g_shm->write_index & 1;
    /* Actually we read from the opposite of write_index (the completed buffer) */
    read_buf = read_buf ^ 1;

    const uint8_t *src = g_shm->framebuf[read_buf];

    /* Convert RGB332 to RGB888 and update texture */
    uint8_t *pixels;
    int pitch;
    if (SDL_LockTexture(g_texture, NULL, (void **)&pixels, &pitch) == 0) {
        for (int y = 0; y < h; y++) {
            uint8_t *dst_row = pixels + y * pitch;
            const uint8_t *src_row = src + y * w;
            for (int x = 0; x < w; x++) {
                uint8_t c = src_row[x];
                dst_row[x * 3 + 0] = g_rgb332_to_rgb888[c * 3 + 0];
                dst_row[x * 3 + 1] = g_rgb332_to_rgb888[c * 3 + 1];
                dst_row[x * 3 + 2] = g_rgb332_to_rgb888[c * 3 + 2];
            }
        }
        SDL_UnlockTexture(g_texture);
    }

    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

/* ----- Main ----- */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Disable stdout buffering for Docker log visibility */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("[sdl2-display] Starting SDL2 display process\n");

    init_rgb332_lut();

    /* Step 1: Connect to shared memory */
    if (setup_shm() < 0) {
        fprintf(stderr, "[sdl2-display] Failed to setup shared memory\n");
        return 1;
    }

    /* Step 2: Initialize SDL2 */
    if (setup_sdl2() < 0) {
        fprintf(stderr, "[sdl2-display] Failed to initialize SDL2\n");
        return 1;
    }

    /* Signal FreeRTOS process that we're ready (before input socket,
     * which is created by FreeRTOS side after display init completes) */
    g_shm->display_initialized = 1;
    printf("[sdl2-display] Ready - signaled FreeRTOS process\n");

    /* Step 3: Try to connect input socket (will retry in main loop) */
    try_connect_input_socket();

    /* Step 4: Open the synthetic-input injection socket (agent/CI) */
    setup_inject_socket();

    uint16_t w = g_shm->display_width;
    uint16_t h = g_shm->display_height;
    uint8_t sx = g_shm->scaling_x > 0 ? g_shm->scaling_x : 2;
    uint8_t sy = g_shm->scaling_y > 0 ? g_shm->scaling_y : 2;

    /* Main loop */
    while (g_running) {
        /* Check for shutdown from FreeRTOS side */
        if (g_shm->shutdown_requested) {
            g_running = 0;
            break;
        }

        /* Process SDL events */
        process_sdl_events(sx, sy);

        /* Forward injected synthetic events */
        drain_inject_events();

        /* Wait for new frame (with timeout to keep event processing responsive) */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 16000000; /* 16ms timeout */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }

        if (sem_timedwait(g_sem_frame, &ts) == 0) {
            /* New frame available */
            render_frame(w, h);
        }

        /* Try to connect/reconnect input socket if not connected */
        try_connect_input_socket();
    }

    /* Cleanup */
    printf("[sdl2-display] Shutting down...\n");

    if (g_shm) g_shm->shutdown_requested = 1;

    if (g_audio_device) {
        SDL_PauseAudioDevice(g_audio_device, 1);
        SDL_CloseAudioDevice(g_audio_device);
    }
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();

    if (g_input_fd >= 0) close(g_input_fd);
    if (g_inject_fd >= 0) {
        close(g_inject_fd);
        unlink(FMRB_INJECT_SOCKET_PATH);
    }
    if (g_sem_frame != SEM_FAILED) sem_close(g_sem_frame);
    if (g_shm) munmap(g_shm, sizeof(fmrb_shm_t));
    if (g_shm_fd >= 0) close(g_shm_fd);

    printf("[sdl2-display] Stopped.\n");
    return 0;
}
