#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_timer.h"
#include "communication/comm_interface.h"
#include "communication/comm_message.h"
#include "common/fmrb_task_config.h"
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
  MessageBufferHandle_t msg_buffer = xMessageBufferCreate((sizeof(message_data_t) + 4) * MSG_BUFFER_NUM_MESSAGES);
  if (msg_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to create message buffer");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
  }
  ESP_LOGI(TAG, "Message buffer created for inter-task communication");

  // Graphics task
  ret = xTaskCreatePinnedToCore(
      graphics_task,
      "graphics_task",
      GRAPHICS_TASK_STACK_SIZE,
      NULL,
      GRAPHICS_TASK_PRIORITY,
      NULL,
      GRAPHICS_TASK_CORE
  );
  ESP_LOGI(TAG, "graphics_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  // Audio task
  ret = xTaskCreatePinnedToCore(
      audio_task,
      "audio_task",
      AUDIO_TASK_STACK_SIZE,
      NULL,
      AUDIO_TASK_PRIORITY,
      NULL,
      AUDIO_TASK_CORE
  );
  ESP_LOGI(TAG, "audio_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  // Communication task
  ret = xTaskCreatePinnedToCore(
      comm_task,
      "comm_task",
      COMM_TASK_STACK_SIZE,
      (void*)msg_buffer,
      COMM_TASK_PRIORITY,
      NULL,
      COMM_TASK_CORE
  );
  ESP_LOGI(TAG, "comm_task create: %s", (ret == pdPASS) ? "OK" : "FAILED");

  // Message handler task
  ret = xTaskCreatePinnedToCore(
      message_handler_task,
      "message_handler_task",
      MESSAGE_HANDLER_TASK_STACK_SIZE,
      (void*)msg_buffer,
      MESSAGE_HANDLER_TASK_PRIORITY,
      NULL,
      MESSAGE_HANDLER_TASK_CORE
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
