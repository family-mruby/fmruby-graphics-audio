#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_timer.h"
#include "communication/comm_interface.h"
#include "communication/comm_message.h"
#include "esp_log.h"
#include "tasks/graphics_task.h"
#include "tasks/audio_task.h"
#include "tasks/comm_task.h"
#include "tasks/message_handler_task.h"

static const char *TAG = "main";

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "Starting on core %d", xPortGetCoreID());

  BaseType_t ret;

  // Create MessageBuffer for comm_task -> message_handler_task communication
  // Max message = sizeof(message_data_t) = ~261 bytes + 4 bytes FreeRTOS overhead
  // Buffer holds ~8 full-size messages
  MessageBufferHandle_t msg_buffer = xMessageBufferCreate((sizeof(message_data_t) + 4) * 8);
  if (msg_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to create message buffer");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
  }
  ESP_LOGI(TAG, "Message buffer created for inter-task communication");

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

  // Communication task (SPI slave) - high priority for responsiveness
  ESP_LOGI(TAG, "Creating comm_task (prio=6, core=0)...");
  ret = xTaskCreatePinnedToCore(
      comm_task,
      "comm_task",
      8192,
      (void*)msg_buffer,      // Pass MessageBuffer handle
      6,
      NULL,
      0
  );
  ESP_LOGI(TAG, "comm_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  // Message handler task - lower priority for application processing
  ESP_LOGI(TAG, "Creating message_handler_task (prio=5, core=0)...");
  ret = xTaskCreatePinnedToCore(
      message_handler_task,
      "message_handler_task",
      8192,
      (void*)msg_buffer,      // Pass MessageBuffer handle
      5,
      NULL,
      0
  );
  ESP_LOGI(TAG, "message_handler_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

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
