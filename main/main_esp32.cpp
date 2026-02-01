#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "communication/comm_interface.h"


// C++のLGFX関数を呼び出すためのラッパー関数を宣言
extern "C" {
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

void audio_task_impl(void);
}

void gfx_test()
{
  // 初期テストモードの設定
  int current_mode = 2;
  uint64_t last_mode_change = esp_timer_get_time();
  const uint64_t mode_change_interval = 10000000; // 10秒間隔

  printf("Starting test modes cycle...\n");

  // メモリ情報を定期的に出力
  lgfx_print_memory_info();

  int cnt=0;
  while(cnt < 1000){
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

    // フレームレート制御
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// Core0で動作するタスク1: Windowシステム管理用
void gfx_task(void* arg) {
  printf("Window task started on core %d\n", xPortGetCoreID());

  gfx_test();
}

// Core1で動作するタスク2: ユーザーインターフェース処理用
void audio_task(void* arg) {
  printf("Audio task started on core %d\n", xPortGetCoreID());
  audio_task_impl();
}

// Core1で動作するSPI通信タスク
void spi_task(void* arg) {
  printf("SPI task started on core %d\n", xPortGetCoreID());

  const comm_interface_t* comm = comm_get_interface();

  // SPI初期化
  if (comm->init() != 0) {
    printf("SPI initialization failed!\n");
    vTaskDelete(NULL);
    return;
  }

  printf("SPI initialized successfully, starting communication loop...\n");

  // テスト用のダミーデータ
  uint8_t test_data[] = {0xAA, 0x55, 0x01, 0x02, 0x03, 0x04};
  int send_count = 0;

  while (1) {
    // 通信処理
    int processed = comm->process();
    if (processed < 0) {
      printf("SPI process error\n");
    }

    // 5秒ごとにテストデータを送信
    send_count++;
    if (send_count >= 500) {  // 10ms * 500 = 5秒
      printf("SPI: Sending test data...\n");
      int sent = comm->send(test_data, sizeof(test_data));
      if (sent > 0) {
        printf("SPI: Sent %d bytes\n", sent);
      } else {
        printf("SPI: Send failed\n");
      }
      send_count = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


extern "C" void app_main(void)
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
      8192,              // スタックサイズ
      NULL,              // パラメータ
      5,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  // Core0でタスク2を起動
  printf("Creating Audio task on Core1...\n");
  xTaskCreatePinnedToCore(
      audio_task,           // タスク関数
      "audio_task",         // タスク名
      8192,              // スタックサイズ
      NULL,              // パラメータ
      6,                 // 優先度
      NULL,              // タスクハンドル
      1                  // Core1に固定
  );

  // Core0でSPIタスクを起動
  printf("Creating SPI task on Core1...\n");
  xTaskCreatePinnedToCore(
      spi_task,             // タスク関数
      "spi_task",           // タスク名
      4096,              // スタックサイズ
      NULL,              // パラメータ
      5,                 // 優先度
      NULL,              // タスクハンドル
      0                  // Core0に固定
  );

  // Core1 はLovyanGFXのメモリアクセス専用

  printf("All tasks created successfully!\n");

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
