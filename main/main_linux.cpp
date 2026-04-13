/**
 * @file main_linux.cpp
 * @brief Linux headless entry point for fmruby-graphics-audio.
 *        SDL2 display is handled by a separate process.
 *        This process runs only FreeRTOS tasks for graphics composition,
 *        audio emulation, and communication.
 */
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"

#include "esp_log.h"
#include "comm_message.h"
#include "fmrb_task_config.h"
#include "graphics_task.h"
#include "audio_task.h"
#include "comm_task.h"
#include "message_handler_task.h"

static const char *TAG = "main_linux";

static volatile int running = 1;

extern "C" void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);

    running = 0;

    comm_task_stop();
    message_handler_task_stop();
    audio_task_stop();
    graphics_task_stop();

    usleep(50000);  // 50ms wait for tasks to start cleanup
    printf("Signal handler completed, tasks stopping...\n");
}

extern "C" int app_main(void)
{
    ESP_LOGI(TAG, "Family mruby Host (headless, SDL2 in separate process) starting...\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create MessageBuffer for comm_task -> message_handler_task communication
    MessageBufferHandle_t msg_buffer = xMessageBufferCreate((sizeof(message_data_t) + 4) * MSG_BUFFER_NUM_MESSAGES);
    if (msg_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to create message buffer");
        return -1;
    }
    printf("Message buffer created for inter-task communication\n");

    // Audio task
    xTaskCreatePinnedToCore(
        audio_task,
        "audio_task",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        AUDIO_TASK_PRIORITY,
        NULL,
        AUDIO_TASK_CORE
    );

    // Communication task
    xTaskCreatePinnedToCore(
        comm_task,
        "comm_task",
        COMM_TASK_STACK_SIZE,
        (void*)msg_buffer,
        COMM_TASK_PRIORITY,
        NULL,
        COMM_TASK_CORE
    );

    // Message handler task
    xTaskCreatePinnedToCore(
        message_handler_task,
        "message_handler_task",
        MESSAGE_HANDLER_TASK_STACK_SIZE,
        (void*)msg_buffer,
        MESSAGE_HANDLER_TASK_PRIORITY,
        NULL,
        MESSAGE_HANDLER_TASK_CORE
    );

    // Graphics task - now runs as a regular FreeRTOS task (no SDL2 main loop)
    xTaskCreatePinnedToCore(
        graphics_task,
        "graphics_task",
        GRAPHICS_TASK_STACK_SIZE,
        NULL,
        GRAPHICS_TASK_PRIORITY,
        NULL,
        GRAPHICS_TASK_CORE
    );

    // Main thread waits for shutdown signal
    while (running) {
        sleep(1);
    }

    ESP_LOGI(TAG, "Main thread exiting");
    return 0;
}
