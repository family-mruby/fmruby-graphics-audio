#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "communication/comm_interface.h"
#include "esp_log.h"
#include "tasks/graphics_task.h"
#include "tasks/audio_task.h"
#include "tasks/comm_task.h"

static const char *TAG = "main_linux";

extern "C" void app_main(void)
{
  ESP_LOGI(TAG,"Starting app_main\n");

  // Core0でタスク1を起動
  printf("Creating gfx task on Core0...\n");
  xTaskCreatePinnedToCore(
      graphics_task,        // タスク関数
      "graphics_task",      // タスク名
      8192,              // スタックサイズ
      NULL,              // パラメータ
      5,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  // Core0でタスク2を起動
  ESP_LOGI(TAG,"Creating Audio task on Core0...\n");
  xTaskCreatePinnedToCore(
      audio_task,           // タスク関数
      "audio_task",         // タスク名
      8192,              // スタックサイズ
      NULL,              // パラメータ
      6,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  // Core0でSPIタスクを起動
  ESP_LOGI(TAG,"Creating SPI task on Core1...\n");
  xTaskCreatePinnedToCore(
      comm_task,             // タスク関数
      "comm_task",           // タスク名
      4096,              // スタックサイズ
      NULL,              // パラメータ
      7,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  // Core1 はLovyanGFXのメモリアクセス専用

  ESP_LOGI(TAG,"All tasks created successfully!\n");

  int count=0;
  while(1) {
    count++;
    if (count % 10 == 0) {  // 10秒ごとに出力
        ESP_LOGI(TAG,"UI task running... count=%u (core %d)\n", count, xPortGetCoreID());
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
