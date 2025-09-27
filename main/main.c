#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// C++のLGFX関数を呼び出すためのラッパー関数を宣言
void lgfx_init(void);
void lgfx_draw_test_pattern(void);
void lgfx_draw_moving_circles(void);
void lgfx_draw_physics_simulation(void);
void lgfx_draw_random_rect(void);
void lgfx_update_fps(void);
void lgfx_set_test_mode(int mode);
int lgfx_get_test_mode(void);
void lgfx_test_resolution(int width, int height);
void lgfx_print_memory_info(void);
void lgfx_draw_test(void);
void lgfx_print_detailed_memory_info(void);

#ifdef __cplusplus
}
#endif

void gfx_test()
{
  // 初期テストモードの設定
  int current_mode = 2;
  uint64_t last_mode_change = esp_timer_get_time();
  const uint64_t mode_change_interval = 10000000*3; // 10秒間隔

  printf("Starting test modes cycle...\n");

  // メモリ情報を定期的に出力
  lgfx_print_memory_info();

  int cnt=0;
  while(cnt < 100){
    lgfx_draw_test();
    vTaskDelay(pdMS_TO_TICKS(1));
    cnt++;
  }

  // メインループ：異なるテストモードをサイクル実行
  while(1) {
    uint64_t current_time = esp_timer_get_time();

    // 10秒ごとにモード切り替え
    if (current_time - last_mode_change >= mode_change_interval) {
      current_mode = (current_mode + 1) % 3; // 0、1、2を切り替え
      lgfx_set_test_mode(current_mode);
      last_mode_change = current_time;

      switch (current_mode) {
        case 0:
          printf("Switching to Color Bar Test Pattern\n");
          break;
        case 1:
          printf("Switching to Moving Circles Animation\n");
          break;
        case 2:
          printf("Switching to Physics Simulation\n");
          break;
      }
    }

    // 現在のモードに応じて描画
    switch (current_mode) {
      case 0:
        lgfx_draw_test_pattern();
        break;
      case 1:
        lgfx_draw_moving_circles();
        break;
      case 2:
        lgfx_draw_physics_simulation();
        break;
      default:
        lgfx_draw_test_pattern();
        break;
    }

    // FPS更新
    lgfx_update_fps();

    // フレームレート制御（約60FPS）
    vTaskDelay(pdMS_TO_TICKS(1)); // 16ms待機 ≈ 62.5FPS
  }
}

// Core0で動作するタスク1: Windowシステム管理用
void gfx_task(void* arg) {
  printf("Window task started on core %d\n", xPortGetCoreID());

  gfx_test();
}

// Core0で動作するタスク2: ユーザーインターフェース処理用
void audio_task(void* arg) {
    printf("Audio task started on core %d\n", xPortGetCoreID());
    uint32_t count = 0;

    while(1) {
        // UIイベント処理をシミュレート
        count++;
        if (count % 1000 == 0) {  // 10秒ごとに出力
            printf("Audio task running... count=%lu (core %d)\n", count, xPortGetCoreID());
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // 100Hz実行
    }
}

void app_main(void)
{
  printf("Starting NTSC output with memory diagnosis...\n");

  // 最優先でメモリ診断を実行
  printf("=== Memory state before LGFX init ===\n");
  lgfx_print_detailed_memory_info();

  // LovyanGFXの初期化
  lgfx_init();
  printf("LGFX initialized with sprite buffers\n");

  // 初期化後の状態も確認
  printf("=== Memory state after LGFX init ===\n");
  lgfx_print_detailed_memory_info();

  //gfx_test();

  // Core0でタスク1を起動
  printf("Creating gfx task on Core0...\n");
  xTaskCreatePinnedToCore(
      gfx_task,        // タスク関数
      "gfx_task",      // タスク名
      4096,              // スタックサイズ
      NULL,              // パラメータ
      5,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  // Core0でタスク2を起動
  printf("Creating Audio task on Core0...\n");
  xTaskCreatePinnedToCore(
      audio_task,           // タスク関数
      "audio_task",         // タスク名
      2048,              // スタックサイズ
      NULL,              // パラメータ
      4,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  printf("Both tasks created successfully!\n");

  int count=0;
  while(1) {
    // UIイベント処理をシミュレート
    count++;
    if (count % 10 == 0) {  // 10秒ごとに出力
        printf("UI task running... count=%u (core %d)\n", count, xPortGetCoreID());
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
