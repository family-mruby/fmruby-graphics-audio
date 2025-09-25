#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32/Panel_CVBS.hpp>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <math.h>
#include <inttypes.h>

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

// スプライト用構造体（ダブルバッファ）
static LGFX_Sprite sprites[2];
static int current_sprite = 0;
static uint32_t frame_count = 0;
static uint32_t fps = 0;
static uint32_t last_time = 0;

// 動的オブジェクト用構造体
struct MovingObject {
  float x, y;      // 位置（浮動小数点で滑らかな移動）
  float dx, dy;    // 移動ベクトル
  int radius;      // 半径
  uint32_t color;  // 色

  void move(int width, int height) {
    x += dx;
    y += dy;

    // 壁との衝突判定
    if (x - radius < 0 || x + radius >= width) {
      dx = -dx;
      x = (x - radius < 0) ? radius : width - radius - 1;
    }
    if (y - radius < 0 || y + radius >= height) {
      dy = -dy;
      y = (y - radius < 0) ? radius : height - radius - 1;
    }
  }
};

static const int MAX_OBJECTS = 20;
static MovingObject objects[MAX_OBJECTS];
static int test_mode = 0; // 0:カラーバー, 1:動く円, 2:物理シミュレーション

extern "C" {
  void lgfx_init(void) {
    // 色数の指定 (省略時は rgb332_1Byte)
    gfx.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);   // RGB332 256色
    gfx.init();
    gfx.startWrite();

    // スプライトの初期化（ダブルバッファ用）
    for (int i = 0; i < 2; i++) {
      sprites[i].setColorDepth(gfx.getColorDepth());
      sprites[i].createSprite(gfx.width(), gfx.height());
      sprites[i].setFont(&lgfx::fonts::Font2);
    }

    // オブジェクトの初期化
    for (int i = 0; i < MAX_OBJECTS; i++) {
      objects[i].x = rand() % (gfx.width() - 20) + 10;
      objects[i].y = rand() % (gfx.height() - 20) + 10;
      objects[i].dx = (rand() % 400 - 200) / 100.0f; // -2.0 to 2.0
      objects[i].dy = (rand() % 400 - 200) / 100.0f;
      objects[i].radius = 3 + (rand() % 8);
      objects[i].color = rand() | 0x808080; // 明るい色になるようビット操作
    }

    last_time = esp_timer_get_time() / 1000;
  }

  void lgfx_draw_test_pattern(void) {
    // グラデーションテストパターンを描画
    LGFX_Sprite* sprite = &sprites[current_sprite];
    sprite->clear();

    for (int x = 0; x < gfx.width(); ++x) {
      int v = x * 256 / gfx.width();
      sprite->fillRect(x, 0 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, v, v));  // グレースケール
      sprite->fillRect(x, 1 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, 0, 0));  // 赤
      sprite->fillRect(x, 2 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(0, v, 0));  // 緑
      sprite->fillRect(x, 3 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(0, 0, v));  // 青
      sprite->fillRect(x, 4 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, v, 0));  // 黄
      sprite->fillRect(x, 5 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, 0, v));  // マゼンタ
      sprite->fillRect(x, 6 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(0, v, v));  // シアン
      sprite->fillRect(x, 7 * gfx.height() >> 3, 1, gfx.height() >> 3, gfx.color888(v, v, v));  // 白
    }

    // FPS表示
    sprite->setCursor(2, 2);
    sprite->setTextColor(0x000000); // 黒（影）
    sprite->printf("MODE:%d FPS:%" PRIu32, test_mode, fps);
    sprite->setCursor(1, 1);
    sprite->setTextColor(0xFFFFFF); // 白
    sprite->printf("MODE:%d FPS:%" PRIu32, test_mode, fps);

    sprite->pushSprite(&gfx, 0, 0);
    current_sprite = (current_sprite + 1) & 1; // 0と1を交互に
  }

  void lgfx_draw_moving_circles(void) {
    LGFX_Sprite* sprite = &sprites[current_sprite];
    sprite->clear(0x001122); // 暗い青背景

    // オブジェクトの移動と描画
    for (int i = 0; i < MAX_OBJECTS; i++) {
      objects[i].move(gfx.width(), gfx.height());
      sprite->fillCircle((int)objects[i].x, (int)objects[i].y, objects[i].radius, objects[i].color);
    }

    // FPS表示
    sprite->setCursor(2, 2);
    sprite->setTextColor(0x000000); // 黒（影）
    sprite->printf("MOVING CIRCLES FPS:%" PRIu32, fps);
    sprite->setCursor(1, 1);
    sprite->setTextColor(0xFFFFFF); // 白
    sprite->printf("MOVING CIRCLES FPS:%" PRIu32, fps);

    sprite->pushSprite(&gfx, 0, 0);
    current_sprite = (current_sprite + 1) & 1;
  }

  void lgfx_update_fps(void) {
    frame_count++;
    uint32_t current_time = esp_timer_get_time() / 1000;

    if (current_time - last_time >= 1000) { // 1秒経過
      fps = frame_count;
      frame_count = 0;
      last_time = current_time;
    }
  }

  void lgfx_draw_physics_simulation(void) {
    LGFX_Sprite* sprite = &sprites[current_sprite];
    sprite->clear(0x000011); // 暗い背景

    // より複雑な物理演算（衝突検出付き）
    for (int i = 0; i < MAX_OBJECTS; i++) {
      MovingObject* obj1 = &objects[i];

      // 重力効果の追加
      obj1->dy += 0.02f; // 重力加速度

      // 空気抵抗
      obj1->dx *= 0.999f;
      obj1->dy *= 0.999f;

      // 位置更新
      obj1->move(gfx.width(), gfx.height());

      // 他のオブジェクトとの衝突検出
      for (int j = i + 1; j < MAX_OBJECTS; j++) {
        MovingObject* obj2 = &objects[j];

        float dx = obj1->x - obj2->x;
        float dy = obj1->y - obj2->y;
        float distance = sqrtf(dx * dx + dy * dy);
        float min_distance = obj1->radius + obj2->radius;

        if (distance < min_distance && distance > 0) {
          // 衝突発生：弾性衝突の計算
          float overlap = min_distance - distance;
          dx /= distance;
          dy /= distance;

          // オブジェクトを離す
          obj1->x += dx * overlap * 0.5f;
          obj1->y += dy * overlap * 0.5f;
          obj2->x -= dx * overlap * 0.5f;
          obj2->y -= dy * overlap * 0.5f;

          // 速度交換（簡単な弾性衝突）
          float temp_dx = obj1->dx;
          float temp_dy = obj1->dy;
          obj1->dx = obj2->dx * 0.8f; // エネルギー損失
          obj1->dy = obj2->dy * 0.8f;
          obj2->dx = temp_dx * 0.8f;
          obj2->dy = temp_dy * 0.8f;
        }
      }

      // 描画（速度に応じて色を変化）
      float speed = sqrtf(obj1->dx * obj1->dx + obj1->dy * obj1->dy);
      uint32_t speed_color = ((uint32_t)(speed * 50) & 0xFF) << 16; // 赤成分を速度で変化
      uint32_t color = (obj1->color & 0x00FFFF) | speed_color;

      sprite->fillCircle((int)obj1->x, (int)obj1->y, obj1->radius, color);

      // 軌跡の表示（前フレームとの線を描画）
      static float prev_x[MAX_OBJECTS] = {0};
      static float prev_y[MAX_OBJECTS] = {0};
      sprite->drawLine((int)prev_x[i], (int)prev_y[i], (int)obj1->x, (int)obj1->y,
                      (color & 0xFFFFFF) | 0x404040);
      prev_x[i] = obj1->x;
      prev_y[i] = obj1->y;
    }

    // FPS表示
    sprite->setCursor(2, 2);
    sprite->setTextColor(0x000000); // 黒（影）
    sprite->printf("PHYSICS SIM FPS:%" PRIu32, fps);
    sprite->setCursor(1, 1);
    sprite->setTextColor(0xFFFFFF); // 白
    sprite->printf("PHYSICS SIM FPS:%" PRIu32, fps);

    sprite->pushSprite(&gfx, 0, 0);
    current_sprite = (current_sprite + 1) & 1;
  }

  void lgfx_set_test_mode(int mode) {
    test_mode = mode;

    // モード変更時にオブジェクトをリセット
    if (mode == 2) { // 物理シミュレーションモード
      for (int i = 0; i < MAX_OBJECTS; i++) {
        objects[i].x = rand() % (gfx.width() - 20) + 10;
        objects[i].y = rand() % (gfx.height() / 2) + 10; // 上半分に配置
        objects[i].dx = (rand() % 200 - 100) / 100.0f; // -1.0 to 1.0
        objects[i].dy = (rand() % 100) / 100.0f; // 0 to 1.0（下向き）
        objects[i].radius = 2 + (rand() % 6);
        objects[i].color = rand() | 0x404040;
      }
    }
  }

  int lgfx_get_test_mode(void) {
    return test_mode;
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