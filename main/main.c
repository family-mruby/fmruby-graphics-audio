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
void lgfx_draw_random_rect(void);

#ifdef __cplusplus
}
#endif

void app_main(void)
{
  printf("Starting NTSC output test...\n");

  // LovyanGFXの初期化
  lgfx_init();
  printf("LGFX initialized\n");

  // テストパターンを描画
  lgfx_draw_test_pattern();
  printf("Test pattern drawn\n");

  // メインループ：ランダムな矩形を描画し続ける
  while(1) {
    lgfx_draw_random_rect();
    vTaskDelay(pdMS_TO_TICKS(100)); // 100ms待機
  }
}
