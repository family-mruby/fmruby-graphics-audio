#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "communication/comm_interface.h"
#include "esp_log.h"
#include "tasks/graphics_task.h"
#include "tasks/audio_task.h"
#include "tasks/comm_task.h"

static const char *TAG = "main";

extern "C" void app_main(void)
{
  printf("[app_main] Starting on core %d\n", xPortGetCoreID());

  BaseType_t ret;

  // Graphics task
  printf("[app_main] Creating graphics_task (prio=5, core=0)...\n");
  ret = xTaskCreatePinnedToCore(
      graphics_task,
      "graphics_task",
      8192,
      NULL,
      5,
      NULL,
      0
  );
  printf("[app_main] graphics_task create: %s\n", (ret == pdPASS) ? "OK" : "FAILED");

  // Audio task
  printf("[app_main] Creating audio_task (prio=6, core=0)...\n");
  ret = xTaskCreatePinnedToCore(
      audio_task,
      "audio_task",
      8192,
      NULL,
      6,
      NULL,
      0
  );
  printf("[app_main] audio_task create: %s\n", (ret == pdPASS) ? "OK" : "FAILED");

  // Communication task (SPI slave)
  printf("[app_main] Creating comm_task (prio=7, core=0)...\n");
  ret = xTaskCreatePinnedToCore(
      comm_task,
      "comm_task",
      4096,
      NULL,
      7,
      NULL,
      0
  );
  printf("[app_main] comm_task create: %s\n", (ret == pdPASS) ? "OK" : "FAILED");

  printf("[app_main] All tasks created.\n");

  int count = 0;
  while (1) {
    count++;
    if (count % 10 == 0) {
        printf("[app_main] running... count=%d (core %d)\n", count, xPortGetCoreID());
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
