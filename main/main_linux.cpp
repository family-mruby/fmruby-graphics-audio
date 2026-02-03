#include "lgfx_linux.h"  // Must be first - defines LGFX class for Linux/SDL

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "graphics_task.h"
#include "audio_task.h"
#include "comm_task.h"

static const char *TAG = "main_linux";

static volatile int running = 1;

extern "C" void signal_handler(int sig) {
    printf("\n\n\n+++++++++++++++++++++++++++++++++++++++");
    printf("\n+++++++++++++++++++++++++++++++++++++++\n");
    printf("Received signal %d, shutting down...\n", sig);
    running = 0;
    comm_task_stop();
    audio_task_stop();
    graphics_task_stop();

    // Post SDL_QUIT event to stop LovyanGFX event loop
    SDL_Event quit_event;
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
}

// User function that runs in a separate thread
int user_func(bool* thread_running) {
    ESP_LOGI(TAG,"Family mruby Host (SDL2 + LovyanGFX) starting...\n");

    graphics_task(NULL);

    return 0;
}

extern "C" int app_main(void)
{
    // Disable SDL2 hardware cursor (we'll draw our own)
    SDL_ShowCursor(SDL_DISABLE);

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Creating Audio task ...\n");
    xTaskCreatePinnedToCore(
        audio_task,           // タスク関数
        "audio_task",         // タスク名
        8192*2,              // スタックサイズ
        NULL,              // パラメータ
        6,                 // 優先度
        NULL,              // タスクハンドル
        0                  // Core0(仮)
    );

    printf("Creating comm task ...\n");
    xTaskCreatePinnedToCore(
        comm_task,             // タスク関数
        "comm_task",           // タスク名
        8192*2,              // スタックサイズ
        NULL,              // パラメータ
        5,                 // 優先度
        NULL,              // タスクハンドル
        0                  // Core0(仮)
    );


    printf("Creating LGFX user_func for SDL2...\n");
    return lgfx::Panel_sdl::main(user_func);
}

