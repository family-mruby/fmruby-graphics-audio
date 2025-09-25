#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32/Panel_CVBS.hpp>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"

class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Panel_CVBS _panel_instance;

  LGFX(void)
  {
    { // 表示パネル制御の設定を行います。
      auto cfg = _panel_instance.config();    // 表示パネル設定用の構造体を取得します。

      // 出力解像度を設定
      cfg.memory_width  = 240; // 出力解像度 幅
      cfg.memory_height = 160; // 出力解像度 高さ

      // 実際に利用する解像度を設定
      cfg.panel_width  = 208;  // 実際に使用する幅   (memory_width と同値か小さい値を設定する)
      cfg.panel_height = 128;  // 実際に使用する高さ (memory_heightと同値か小さい値を設定する)

      // 表示位置オフセット量を設定
      cfg.offset_x = 16;       // 表示位置を右にずらす量 (初期値 0)
      cfg.offset_y = 16;       // 表示位置を下にずらす量 (初期値 0)

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config_detail();

      // NTSC-J出力（日本向け）を設定
      cfg.signal_type = cfg.signal_type_t::NTSC_J;

      // 出力先のGPIO番号を設定（DACを使用するため、 25 または 26 のみが選択可能）
      cfg.pin_dac = 26;

      // PSRAMメモリ割当の設定（ESP32-WROVER-Eの場合）
      cfg.use_psram = 1;      // 0=PSRAM不使用 / 1=PSRAMとSRAMを半々使用 / 2=全部PSRAM使用

      // 出力信号の振幅の強さを設定
      cfg.output_level = 128; // 初期値128

      // 彩度信号の振幅の強さを設定
      cfg.chroma_level = 128; // 初期値128

      // バックグラウンドでPSRAMの読出しを行うタスクの優先度を設定
      cfg.task_priority = 25;

      // バックグラウンドでPSRAMの読出しを行うタスクを実行するCPUを選択
      cfg.task_pinned_core = 0; // PRO_CPU_NUM

      _panel_instance.config_detail(cfg);
    }

    setPanel(&_panel_instance);
  }
};

static LGFX gfx;

extern "C" {
  void lgfx_init(void) {
    // 色数の指定 (省略時は rgb332_1Byte)
    gfx.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);   // RGB332 256色

    gfx.init();
  }

  void lgfx_draw_test_pattern(void) {
    // グラデーションテストパターンを描画
    for (int x = 0; x < gfx.width(); ++x) {
      int v = x * 256 / gfx.width();
      gfx.fillRect(x, 0 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, v, v));  // グレースケール
      gfx.fillRect(x, 1 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, 0, 0));  // 赤
      gfx.fillRect(x, 2 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(0, v, 0));  // 緑
      gfx.fillRect(x, 3 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(0, 0, v));  // 青
      gfx.fillRect(x, 4 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, v, 0));  // 黄
      gfx.fillRect(x, 5 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, 0, v));  // マゼンタ
      gfx.fillRect(x, 6 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(0, v, v));  // シアン
      gfx.fillRect(x, 7 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, v, v));  // 白
    }
  }

  void lgfx_draw_random_rect(void) {
    // ランダムな位置とサイズで矩形を描画
    int x = rand() % (gfx.width() - 16);
    int y = rand() % (gfx.height() - 16);
    int w = 8 + (rand() % 16);
    int h = 8 + (rand() % 16);
    uint32_t color = rand();

    gfx.fillRect(x, y, w, h, color);
  }
}