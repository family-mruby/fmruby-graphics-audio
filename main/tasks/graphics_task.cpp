#include "graphics_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "display_interface.h"
#include "graphics_handler.h"

extern "C" {
#include "input_handler.h"
#include "input_socket.h"
#include "fmrb_link_protocol.h"
#include "fmrb_ga_version.h"
#include "fmrb_gfx.h"
#include "../mempool/fmrb_mempool.h"
#include "audio_handler.h"
#include "comm_interface.h"
#include "fmrb_time.h"
#include "esp_app_desc.h"
#ifndef CONFIG_IDF_TARGET_LINUX
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#endif
}

// LGFX test drawing (lgfx_test.cpp) - kept for future use
extern void lgfx_test(void);

static const char *TAG = "graphics_task";
static volatile int task_running = 0;

static volatile int display_initialized = 0;
static uint16_t display_width = 320;
static uint16_t display_height = 240;

// Boot config: received INIT_DISPLAY params (set by message_handler_task)
static volatile int init_display_received = 0;
static fmrb_control_init_display_t received_display_config;

// Deferred ACK: saved from INIT_DISPLAY message, sent after full_display_init
static volatile int deferred_ack_pending = 0;
static uint8_t deferred_ack_type = 0;
static uint8_t deferred_ack_seq = 0;

// Default display config
#define BOOT_DEFAULT_WIDTH     320
#define BOOT_DEFAULT_HEIGHT    240
#define BOOT_DEFAULT_DEPTH     8
#ifdef CONFIG_IDF_TARGET_LINUX
#define BOOT_DEFAULT_MARGIN_X  0
#define BOOT_DEFAULT_MARGIN_Y  0
#else
// ESP32: margins for NTSC overscan
#define BOOT_DEFAULT_MARGIN_X  2
#define BOOT_DEFAULT_MARGIN_Y  16
#endif

// Display config file path (platform-specific)
#ifdef CONFIG_IDF_TARGET_LINUX
#define DISPLAY_CONF_PATH "flash/etc/display_conf_linux.txt"
#else
#define DISPLAY_CONF_PATH "/flash/etc/display_conf_esp32.txt"
#endif

// ---- Display config file I/O ----

static void set_default_config(fmrb_control_init_display_t *cfg) {
    cfg->width = BOOT_DEFAULT_WIDTH;
    cfg->height = BOOT_DEFAULT_HEIGHT;
    cfg->color_depth = BOOT_DEFAULT_DEPTH;
    cfg->margin_x = BOOT_DEFAULT_MARGIN_X;
    cfg->margin_y = BOOT_DEFAULT_MARGIN_Y;
}

static bool save_display_config(const fmrb_control_init_display_t *cfg) {
    FILE *f = fopen(DISPLAY_CONF_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open config file for writing: %s", DISPLAY_CONF_PATH);
        return false;
    }

    fprintf(f, "width=%d\n", cfg->width);
    fprintf(f, "height=%d\n", cfg->height);
    fprintf(f, "color_depth=%d\n", cfg->color_depth);
    fprintf(f, "margin_x=%d\n", cfg->margin_x);
    fprintf(f, "margin_y=%d\n", cfg->margin_y);
    fclose(f);

    ESP_LOGI(TAG, "Saved display config: %dx%d, depth=%d, margin=%d,%d",
             cfg->width, cfg->height, cfg->color_depth, cfg->margin_x, cfg->margin_y);
    return true;
}

static bool read_config_file(const char *path, fmrb_control_init_display_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, "width=%d", &val) == 1) cfg->width = (uint16_t)val;
        else if (sscanf(line, "height=%d", &val) == 1) cfg->height = (uint16_t)val;
        else if (sscanf(line, "color_depth=%d", &val) == 1) cfg->color_depth = (uint8_t)val;
        else if (sscanf(line, "margin_x=%d", &val) == 1) cfg->margin_x = (uint8_t)val;
        else if (sscanf(line, "margin_y=%d", &val) == 1) cfg->margin_y = (uint8_t)val;
    }
    fclose(f);
    return true;
}

static bool load_display_config(fmrb_control_init_display_t *cfg) {
    set_default_config(cfg);

    if (read_config_file(DISPLAY_CONF_PATH, cfg)) {
        ESP_LOGI(TAG, "Loaded display config from %s: %dx%d, depth=%d, margin=%d,%d",
                 DISPLAY_CONF_PATH, cfg->width, cfg->height, cfg->color_depth,
                 cfg->margin_x, cfg->margin_y);
        return true;
    }

    // No config file: create from defaults
    ESP_LOGI(TAG, "No config file found, creating default at %s", DISPLAY_CONF_PATH);
    save_display_config(cfg);
    return true;
}

static bool config_matches(const fmrb_control_init_display_t *a, const fmrb_control_init_display_t *b) {
    return a->width == b->width &&
           a->height == b->height &&
           a->color_depth == b->color_depth &&
           a->margin_x == b->margin_x &&
           a->margin_y == b->margin_y;
}

// ---- Boot beep ----

static void play_boot_beep(void) {
    // PC-98 style "piko!" - short high-pitched pulse
    // channel 0 = PULSE1, freq ~880Hz, volume 8, duty 2 (25%), no sweep
    audio_task_note_on(0, 880, 8, 2, 0);
    lgfx::delay(80);
    audio_task_note_off(0);
    lgfx::delay(30);
    // Second higher tone
    audio_task_note_on(0, 1760, 6, 2, 0);
    lgfx::delay(60);
    audio_task_note_off(0);
}

// ---- Boot screen drawing (PC-98 style) ----

#define BOOT_CHAR_W 6
#define BOOT_CHAR_H 8
#define BOOT_LINE_H 10
#define BOOT_MARGIN_X 4
#define BOOT_MARGIN_Y 4

static int boot_line_y = 0;  // Current line position

static void boot_screen_init(lgfx::LGFX_Device *gfx) {
    gfx->fillScreen(0x00);  // Black
    gfx->setTextColor(0xFFFFFFU, 0x000000U);  // White on black (RGB888)
    boot_line_y = BOOT_MARGIN_Y;
}

static void boot_print_line(lgfx::LGFX_Device *gfx, const char *text) {
    gfx->setCursor(BOOT_MARGIN_X, boot_line_y);
    gfx->print(text);
    boot_line_y += BOOT_LINE_H;
    DISPLAY_INTERFACE->display();
}

static void boot_print_blank(lgfx::LGFX_Device *gfx) {
    boot_line_y += BOOT_LINE_H;
}

static void draw_boot_info(lgfx::LGFX_Device *gfx, const fmrb_control_init_display_t *cfg) {
    char buf[64];

    boot_screen_init(gfx);

    boot_print_line(gfx, "Family mruby Graphics-Audio System");
    boot_print_line(gfx, "==================================");
    boot_print_blank(gfx);

    // Which build is on screen. The app description is written by the build at
    // link time, so unlike __DATE__ / __TIME__ in this file it cannot linger as
    // a stale value through an incremental build. Its version is git describe
    // (the project sets no PROJECT_VER), so it carries the commit and whether
    // the tree was dirty -- more than FMRB_GA_FW_VERSION says on its own.
    const esp_app_desc_t *app = esp_app_get_description();
    // 28 characters of version leave the fixed parts room on a 320 dot line.
    snprintf(buf, sizeof(buf), "GA: %.28s  link v%d", app->version, FMRB_LINK_VERSION);
    boot_print_line(gfx, buf);
    snprintf(buf, sizeof(buf), "Built: %s %s", app->date, app->time);
    boot_print_line(gfx, buf);
    // Whole idf_ver on a line of its own: cutting it at the first dash would
    // turn "v5.5.4-1242-g10ca0dff2f4" (a master build 1242 commits past the
    // tag) into "v5.5.4", which reads as the release and is not the same
    // toolchain.
    snprintf(buf, sizeof(buf), "IDF: %.40s", app->idf_ver);
    boot_print_line(gfx, buf);
    boot_print_blank(gfx);

#ifndef CONFIG_IDF_TARGET_LINUX
    // Chip info
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *chip_name = "ESP32";
    if (chip.model == CHIP_ESP32) chip_name = "ESP32";
    else if (chip.model == CHIP_ESP32S3) chip_name = "ESP32-S3";
    else if (chip.model == CHIP_ESP32C3) chip_name = "ESP32-C3";

    snprintf(buf, sizeof(buf), "CPU: %s rev%d %d cores",
             chip_name, chip.revision, chip.cores);
    boot_print_line(gfx, buf);

    // Flash size
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(buf, sizeof(buf), "Flash: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    boot_print_line(gfx, buf);

    // Memory
    snprintf(buf, sizeof(buf), "Free heap:  %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    boot_print_line(gfx, buf);

    snprintf(buf, sizeof(buf), "Free PSRAM: %zu bytes",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    boot_print_line(gfx, buf);
#else
    boot_print_line(gfx, "Platform: Linux (simulation)");
#endif

    boot_print_blank(gfx);

    snprintf(buf, sizeof(buf), "Display: %dx%d %dbit margin(%d,%d)",
             cfg->width, cfg->height, cfg->color_depth, cfg->margin_x, cfg->margin_y);
    boot_print_line(gfx, buf);

    boot_print_line(gfx, "Audio: APU initialized");
    boot_print_blank(gfx);
    boot_print_line(gfx, "Waiting for Core...");
}

static void draw_boot_cursor(lgfx::LGFX_Device *gfx, bool visible) {
    // Blinking cursor after "Waiting for Core..."
    int cursor_x = BOOT_MARGIN_X + 19 * BOOT_CHAR_W + 2;
    int cursor_y = boot_line_y - BOOT_LINE_H;
    if (visible) {
        gfx->fillRect(cursor_x, cursor_y, BOOT_CHAR_W, BOOT_CHAR_H, 0xFFFFFFU);
    } else {
        gfx->fillRect(cursor_x, cursor_y, BOOT_CHAR_W, BOOT_CHAR_H, 0x00);
    }
    DISPLAY_INTERFACE->display();
}

static void draw_reboot_screen(lgfx::LGFX_Device *gfx, uint16_t w, uint16_t h) {
    boot_print_blank(gfx);
    gfx->setTextColor(0xFFFF00U, 0x000000U);  // Yellow on black (RGB888)
    boot_print_line(gfx, "** Config updated. Please reboot. **");

    DISPLAY_INTERFACE->display();
}

// ---- Full display initialization (creates canvas pool, graphics handler, etc.) ----

static int full_display_init(uint16_t width, uint16_t height, uint8_t color_depth,
                             uint8_t margin_x, uint8_t margin_y) {
    // Display hardware is already initialized during boot screen phase.
    // Now initialize the canvas memory pool and graphics handler.

    display_width = width;
    display_height = height;

    if (fmrb_mempool_canvas_init(width, height, color_depth) != 0) {
        ESP_LOGE(TAG, "Failed to initialize canvas memory pool");
        return -1;
    }

    ESP_LOGI(TAG, "Graphics initialized with LovyanGFX (%dx%d, %d-bit RGB)", width, height, color_depth);

    if (graphics_handler_init() < 0) {
        ESP_LOGE(TAG, "Graphics handler initialization failed");
        fmrb_mempool_canvas_deinit();
        return -1;
    }

#ifdef CONFIG_IDF_TARGET_LINUX
    if (input_handler_init() < 0) {
        ESP_LOGE(TAG, "Input handler initialization failed");
        graphics_handler_cleanup();
        return -1;
    }
#endif

    display_initialized = 1;
    ESP_LOGI(TAG, "Display initialization complete");
    return 0;
}

// Callback function called by message_handler_task when INIT_DISPLAY is received
// ACK is NOT sent here - it is deferred until full_display_init completes
extern "C" int init_display_callback(uint16_t width, uint16_t height, uint8_t color_depth,
                                     uint8_t margin_x, uint8_t margin_y,
                                     uint8_t msg_type, uint8_t msg_seq) {
    ESP_LOGI(TAG, "INIT_DISPLAY received: %dx%d, %d-bit color, margin=%d,%d",
             width, height, color_depth, margin_x, margin_y);

    if (display_initialized) {
        ESP_LOGW(TAG, "Display already initialized, ignoring re-init request");
        return 1;  // Return 1 = already initialized, caller should ACK immediately
    }

    // Store received params for comparison by graphics_task
    received_display_config.width = width;
    received_display_config.height = height;
    received_display_config.color_depth = color_depth;
    received_display_config.margin_x = margin_x;
    received_display_config.margin_y = margin_y;

    // Save ACK info for deferred sending
    deferred_ack_type = msg_type;
    deferred_ack_seq = msg_seq;
    deferred_ack_pending = 1;

    init_display_received = 1;

    return 0;  // Return 0 = deferred, caller should NOT ACK
}


void graphics_task_stop(void) {
    task_running = 0;
}

void graphics_task(void *pvParameters) {
    task_running = 1;
    ESP_LOGI(TAG, "Graphics task started on core %d", xPortGetCoreID());

#ifdef CONFIG_IDF_TARGET_LINUX
    if (input_socket_start() < 0) {
        ESP_LOGE(TAG, "Input socket server start failed");
        return;
    }
#endif

    // ---- Phase 1: Load config and initialize display for boot screen ----

    fmrb_control_init_display_t boot_config;
    load_display_config(&boot_config);

    // Initialize display hardware (but not canvas pool / graphics handler yet)
    if (DISPLAY_INTERFACE->init(boot_config.width, boot_config.height,
                                boot_config.color_depth,
                                boot_config.margin_x, boot_config.margin_y) < 0) {
        ESP_LOGE(TAG, "Display initialization failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Display initialized suuccessfully");

    lgfx::LGFX_Device *gfx = (lgfx::LGFX_Device *)DISPLAY_INTERFACE->get_lgfx();
    if (!gfx) {
        ESP_LOGE(TAG, "Failed to get LGFX instance");
        vTaskDelete(NULL);
        return;
    }

    // ---- Phase 2: Boot beep + boot screen (PC-98 style) ----

    ESP_LOGI(TAG, "Play boot beep and show boot screen");
    play_boot_beep();

    // Draw boot info lines (appears line by line)
    draw_boot_info(gfx, &boot_config);

    ESP_LOGI(TAG, "Showing boot screen, waiting for Core...");

    bool cursor_on = true;
    int blink_counter = 0;

    while (!init_display_received && task_running) {
        // Blink cursor
        draw_boot_cursor(gfx, cursor_on);

        int ev = DISPLAY_INTERFACE->process_events();
        if (ev == 1) {
            task_running = 0;
            break;
        }

        lgfx::delay(100);
        blink_counter++;
        if (blink_counter >= 5) {  // Toggle every 500ms
            cursor_on = !cursor_on;
            blink_counter = 0;
        }
    }

    ESP_LOGI(TAG, "init_display_received");


    if (!task_running) {
        DISPLAY_INTERFACE->cleanup();
        vTaskDelete(NULL);
        return;
    }

    // ---- Phase 3: Compare INIT_DISPLAY with boot config ----

    if (!config_matches(&boot_config, &received_display_config)) {
        ESP_LOGW(TAG, "Display config mismatch! Saved: %dx%d margin=%d,%d, Received: %dx%d margin=%d,%d",
                 boot_config.width, boot_config.height, boot_config.margin_x, boot_config.margin_y,
                 received_display_config.width, received_display_config.height,
                 received_display_config.margin_x, received_display_config.margin_y);

        // Save new config
        save_display_config(&received_display_config);

        // Show reboot message
        draw_reboot_screen(gfx, boot_config.width, boot_config.height);

        // Wait indefinitely (user must reboot)
        while (task_running) {
            int ev = DISPLAY_INTERFACE->process_events();
            if (ev == 1) break;
            lgfx::delay(500);
        }

        DISPLAY_INTERFACE->cleanup();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Display config matches, proceeding to main loop");

    // ---- Phase 4: Full initialization ----

    if (full_display_init(boot_config.width, boot_config.height,
                          boot_config.color_depth,
                          boot_config.margin_x, boot_config.margin_y) < 0) {
        ESP_LOGE(TAG, "Full display initialization failed");
        DISPLAY_INTERFACE->cleanup();
        vTaskDelete(NULL);
        return;
    }

    // Reset LGFX text state before the canvas system takes over. The boot
    // screen is deliberately left on the display: the core does not draw
    // anything for several seconds after this ACK, and clearing here left the
    // screen black for that whole stretch. graphics_handler wipes it in the
    // frame where the core's first present() arrives instead.
    gfx->setTextColor(0xFFFFFFU);
    gfx->setCursor(0, 0);

    // Send deferred ACK for INIT_DISPLAY now that everything is ready
    // Must include response data (like VERSION does) for transport layer ACK matching
    if (deferred_ack_pending) {
        const comm_interface_t *comm = comm_get_interface();
        if (comm && (deferred_ack_type & FMRB_LINK_FLAG_ACK_REQUIRED)) {
            uint8_t ack_status = 0;  // 0 = success
            comm->send_ack(deferred_ack_type, deferred_ack_seq, &ack_status, sizeof(ack_status));
            ESP_LOGI(TAG, "Deferred INIT_DISPLAY ACK sent (seq=%d)", deferred_ack_seq);
        }
        deferred_ack_pending = 0;
    }

    ESP_LOGI(TAG, "Host server running. Ready to receive commands.");

    // Target frame period: 30 FPS (33ms). Frame deadline tracked across
    // iterations so jitter in render time does not accumulate.
    const uint32_t TARGET_FRAME_PERIOD_MS = 33;
    // Minimum sleep per loop, even when behind schedule. Without this, a
    // render time >= TARGET_FRAME_PERIOD_MS (e.g. heavy composite scenes)
    // would leave zero time for other tasks on this core (msg_handler_task,
    // audio_task) and the uart_slave MessageBuffer would back up. 10ms is
    // chosen so that at any reasonable FreeRTOS tick rate the delay actually
    // yields at least one full tick.
    const uint32_t MIN_YIELD_MS = 10;

    // Main loop timing stats. Render time is measured in microseconds: at 30fps
    // a composite costs a few ms, which the FreeRTOS tick (10ms on the Linux
    // build) cannot resolve at all -- it reported render_avg=0ms.
    uint32_t loop_count = 0;
    uint64_t total_render_us = 0;
    uint32_t max_render_us = 0;
    uint64_t total_disp_us = 0;
    uint32_t max_disp_us = 0;
    uint32_t late_frames = 0;
    uint32_t stats_last_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t next_frame_ms = stats_last_ms + TARGET_FRAME_PERIOD_MS;

    // Main loop
    while (task_running) {
        int display_result = DISPLAY_INTERFACE->process_events();
        if (display_result == 1) {
            task_running = 0;
            break;
        }

#ifdef CONFIG_IDF_TARGET_LINUX
        int input_result = input_handler_process_events();
        if (input_result == 1) {
            task_running = 0;
            break;
        }
#endif

        // One frame of drawing cost, split in two so a blocking handoff cannot
        // be mistaken for composite work: render_us is the Z-order composite,
        // disp_us is the panel/SHM handoff.
        uint64_t render_start_us = fmrb_now_us();
        graphics_handler_render_frame();
        uint64_t render_done_us = fmrb_now_us();

        DISPLAY_INTERFACE->display();
        uint32_t render_us = (uint32_t)(render_done_us - render_start_us);
        uint32_t disp_us = (uint32_t)(fmrb_now_us() - render_done_us);

        loop_count++;
        total_render_us += render_us;
        if (render_us > max_render_us) max_render_us = render_us;
        total_disp_us += disp_us;
        if (disp_us > max_disp_us) max_disp_us = disp_us;

        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - stats_last_ms >= 5000) {
            uint32_t elapsed_ms = now - stats_last_ms;
            uint32_t avg_us = loop_count > 0 ? (uint32_t)(total_render_us / loop_count) : 0;
            // fps with one decimal place using fixed-point arithmetic.
            uint32_t fps_x10 = elapsed_ms > 0 ? (loop_count * 10000UL) / elapsed_ms : 0;
            uint32_t disp_avg_us = loop_count > 0 ? (uint32_t)(total_disp_us / loop_count) : 0;
            ESP_LOGI(TAG, "loop: fps=%lu.%lu render_avg=%luus render_max=%luus disp_avg=%luus disp_max=%luus late=%lu (count=%lu/%lums)",
                     fps_x10 / 10, fps_x10 % 10, avg_us, max_render_us,
                     disp_avg_us, max_disp_us,
                     late_frames, loop_count, elapsed_ms);
            loop_count = 0;
            total_render_us = 0;
            max_render_us = 0;
            total_disp_us = 0;
            max_disp_us = 0;
            late_frames = 0;
            stats_last_ms = now;
        }

        // Elapsed-time-based 30fps pacing with cooperative yield floor.
        // - When render is light, sleep the remainder of the 33ms window.
        // - When render is heavy (sleep_ms < MIN_YIELD_MS), still sleep the
        //   minimum so msg_handler_task / audio_task on this core can run.
        //   The deadline is resynced from the post-sleep moment so we do not
        //   accumulate an ever-growing backlog.
        int32_t sleep_ms = (int32_t)(next_frame_ms - now);
        if (sleep_ms < (int32_t)MIN_YIELD_MS) {
            if (sleep_ms <= 0) late_frames++;
            sleep_ms = (int32_t)MIN_YIELD_MS;
            next_frame_ms = now + MIN_YIELD_MS + TARGET_FRAME_PERIOD_MS;
        } else {
            next_frame_ms += TARGET_FRAME_PERIOD_MS;
        }
        lgfx::delay((uint32_t)sleep_ms);
    }

    ESP_LOGI(TAG, "Shutting down...");

#ifdef CONFIG_IDF_TARGET_LINUX
    input_handler_cleanup();
#endif
    graphics_handler_cleanup();
    fmrb_mempool_canvas_deinit();
    DISPLAY_INTERFACE->cleanup();

    ESP_LOGI(TAG, "Family mruby graphics system stopped.");

    vTaskDelete(NULL);
}
