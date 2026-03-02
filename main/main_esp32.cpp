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
  ESP_LOGI(TAG, "Starting on core %d", xPortGetCoreID());

  BaseType_t ret;

  // Graphics task
  ESP_LOGI(TAG, "Creating graphics_task (prio=5, core=0)...");
  ret = xTaskCreatePinnedToCore(
      graphics_task,
      "graphics_task",
      8192,
      NULL,
      5,
      NULL,
      0
  );
  ESP_LOGI(TAG, "graphics_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  // Audio task (highest priority - hard real-time 60Hz constraint)
  ESP_LOGI(TAG, "Creating audio_task (prio=7, core=0)...");
  ret = xTaskCreatePinnedToCore(
      audio_task,
      "audio_task",
      8192,
      NULL,
      7,
      NULL,
      0
  );
  ESP_LOGI(TAG, "audio_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  // Communication task (SPI slave)
  ESP_LOGI(TAG, "Creating comm_task (prio=6, core=0)...");
  ret = xTaskCreatePinnedToCore(
      comm_task,
      "comm_task",
      8192,
      NULL,
      6,
      NULL,
      0
  );
  ESP_LOGI(TAG, "comm_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  ESP_LOGI(TAG, "All tasks created.");

  int count = 0;
  while (1) {
    count++;
    if (count % 10 == 0) {
        ESP_LOGI(TAG, "running... count=%d (core %d)", count, xPortGetCoreID());
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
