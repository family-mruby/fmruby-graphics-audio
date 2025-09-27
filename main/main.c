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

#ifdef __cplusplus
}
#endif

void app_main(void)
{
  printf("Starting NTSC output with Sprite API test...\n");

  // LovyanGFXの初期化
  lgfx_init();
  printf("LGFX initialized with sprite buffers\n");

  // 初期テストモードの設定
  int current_mode = 0;
  uint64_t last_mode_change = esp_timer_get_time();
  const uint64_t mode_change_interval = 10000000; // 10秒間隔

  printf("Starting test modes cycle...\n");

  // メモリ情報を定期的に出力
  lgfx_print_memory_info();

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
    vTaskDelay(pdMS_TO_TICKS(33)); // 16ms待機 ≈ 62.5FPS
  }
}
