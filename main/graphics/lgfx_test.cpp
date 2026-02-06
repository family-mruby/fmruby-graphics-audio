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
#include "esp_heap_caps.h"

// PSRAMメモリ詳細診断関数
extern "C" void lgfx_print_detailed_memory_info(void) {
    printf("=== PSRAM Memory Diagnosis ===\n");

    // 基本統計情報
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    printf("PSRAM Total: %u bytes\n", (unsigned)total_size);
    printf("PSRAM Free: %u bytes\n", (unsigned)free_bytes);
    printf("PSRAM Largest Block: %u bytes\n", (unsigned)largest_block);
    printf("PSRAM Min Free Ever: %u bytes\n", (unsigned)min_free);
    printf("Required for 720x480 sprite: 326656 bytes\n");
    printf("Can allocate 720x480? %s\n", largest_block >= 326656 ? "YES" : "NO");

    // 詳細ヒープ情報
    printf("\n=== Detailed PSRAM Heap Info ===\n");
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);

    // ヒープダンプ（ブロック詳細）
    printf("\n=== PSRAM Heap Dump ===\n");
    heap_caps_dump(MALLOC_CAP_SPIRAM);

    printf("=== End Memory Diagnosis ===\n\n");
}

class LGFX : public lgfx::LGFX_Device
{
public:
  lgfx::Panel_CVBS _panel_instance;

  LGFX(void)
  {
    { // 表示パネル制御の設定を行います。
      auto cfg = _panel_instance.config();    // 表示パネル設定用の構造体を取得します。

      // 出力解像度を設定
      // cfg.memory_width  = 720; // 出力解像度 幅
      // cfg.memory_height = 480; // 出力解像度 高さ
      cfg.memory_width  = 480; // 出力解像度 幅
      cfg.memory_height = 320; // 出力解像度 高さ

      // 実際に利用する解像度を設定
      cfg.panel_width  = cfg.memory_width-32;  // 実際に使用する幅   (memory_width と同値か小さい値を設定する)
      cfg.panel_height = cfg.memory_height-32;  // 実際に使用する高さ (memory_heightと同値か小さい値を設定する)

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
      cfg.pin_dac = 25;

      // PSRAMメモリ割当の設定（ESP32-WROVER-Eの場合）
      cfg.use_psram = 1;      // 0=PSRAM不使用 / 1=PSRAMとSRAMを半々使用 / 2=全部PSRAM使用

      // 出力信号の振幅の強さを設定
      cfg.output_level = 128; // 初期値128

      // 彩度信号の振幅の強さを設定
      cfg.chroma_level = 128; // 初期値128

      // バックグラウンドでPSRAMの読出しを行うタスクの優先度を設定
      cfg.task_priority = 25;

      // バックグラウンドでPSRAMの読出しを行うタスクを実行するCPUを選択
      cfg.task_pinned_core = 1; //

      _panel_instance.config_detail(cfg);
    }

    setPanel(&_panel_instance);
  }
};

// PSRAM上のヒープに動的確保するためポインタに変更（DRAMオーバーフロー対策）
static LGFX* gfx = nullptr;

// スプライト用構造体（ダブルバッファ）
static LGFX_Sprite* sprites[2] = {nullptr, nullptr};
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
static MovingObject* objects = nullptr;
static int test_mode = 0; // 0:カラーバー, 1:動く円, 2:物理シミュレーション

extern "C" {
  void lgfx_init(void) {
    printf("=== LGFX Initialization Debug ===\n");

    // メモリ状況の確認
    printf("Before init - Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());
    printf("Before init - Free PSRAM: %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // LGFX本体をPSRAM上に確保（DRAMオーバーフロー対策）
    if (!gfx) {
      printf("Allocating LGFX object in PSRAM (%zu bytes)...\n", sizeof(LGFX));
      void* gfx_mem = heap_caps_malloc(sizeof(LGFX), MALLOC_CAP_SPIRAM);
      if (!gfx_mem) {
        printf("ERROR: Failed to allocate LGFX in PSRAM!\n");
        return;
      }
      gfx = new (gfx_mem) LGFX();  // placement new
      printf("LGFX object created in PSRAM\n");
    }

    // 色数の指定 (省略時は rgb332_1Byte)
    gfx->setColorDepth(lgfx::color_depth_t::rgb332_1Byte);   // RGB332 256色
    bool init_result = gfx->init();
    printf("LGFX init result: %s\n", init_result ? "SUCCESS" : "FAILED");

    if (!init_result) {
      printf("ERROR: LGFX initialization failed!\n");
      return;
    }

    gfx->startWrite();

    // 初期化後の表示情報
    printf("Display size: %" PRId32 " x %" PRId32 "\n", gfx->width(), gfx->height());
    printf("Color depth: %d bits\n", gfx->getColorDepth());
    printf("Buffer size needed: %" PRId32 " bytes per buffer\n", gfx->width() * gfx->height());
    printf("Total buffer size: %" PRId32 " bytes (x2 for double buffer)\n", gfx->width() * gfx->height() * 2);

    // メモリ状況の再確認
    printf("After init - Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());
    printf("After init - Free PSRAM: %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // スプライトの初期化（ダブルバッファ用、PSRAM上に確保）
    printf("Creating sprites with PSRAM...\n");
    for (int i = 0; i < 2; i++) {
      if (!sprites[i]) {
        printf("Allocating LGFX_Sprite[%d] in PSRAM (%zu bytes)...\n", i, sizeof(LGFX_Sprite));
        void* sprite_mem = heap_caps_malloc(sizeof(LGFX_Sprite), MALLOC_CAP_SPIRAM);
        if (!sprite_mem) {
          printf("ERROR: Failed to allocate sprite[%d] in PSRAM!\n", i);
          continue;
        }
        sprites[i] = new (sprite_mem) LGFX_Sprite(gfx);  // placement new
      }

      sprites[i]->setPsram(true);  // PSRAMの明示的使用を設定
      sprites[i]->setColorDepth(gfx->getColorDepth());

      printf("Before Sprite[%d] creation - PSRAM largest block: %u bytes\n", i,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

      bool sprite_result = sprites[i]->createSprite(gfx->width(), gfx->height());

      printf("Sprite[%d] creation: %s (size: %" PRId32 "x%" PRId32 ")\n", i,
             sprite_result ? "SUCCESS" : "FAILED", gfx->width(), gfx->height());

      if (sprite_result) {
        sprites[i]->setFont(&lgfx::fonts::Font4);  // Larger font
        sprites[i]->setTextSize(1);  // Text scaling factor
        printf("Sprite[%d] buffer allocated successfully\n", i);
      } else {
        printf("Sprite[%d] FAILED - After failure PSRAM largest block: %u bytes\n", i,
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
      }
    }

    // 最終メモリ状況
    printf("After sprites - Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());
    printf("After sprites - Free PSRAM: %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // MovingObject配列をPSRAM上に確保
    if (!objects) {
      printf("Allocating MovingObject array in PSRAM (%zu bytes)...\n", sizeof(MovingObject) * MAX_OBJECTS);
      objects = (MovingObject*)heap_caps_malloc(sizeof(MovingObject) * MAX_OBJECTS, MALLOC_CAP_SPIRAM);
      if (!objects) {
        printf("ERROR: Failed to allocate objects array in PSRAM!\n");
        return;
      }
      printf("MovingObject array created in PSRAM\n");
    }

    // オブジェクトの初期化
    for (int i = 0; i < MAX_OBJECTS; i++) {
      objects[i].x = rand() % (gfx->width() - 40) + 20;
      objects[i].y = rand() % (gfx->height() - 40) + 20;
      objects[i].dx = (rand() % 800 - 400) / 100.0f; // -4.0 to 4.0 (faster)
      objects[i].dy = (rand() % 800 - 400) / 100.0f;
      objects[i].radius = 10 + (rand() % 15);  // 10-24 (larger)
      objects[i].color = rand() | 0x808080; // 明るい色になるようビット操作
    }

    last_time = esp_timer_get_time() / 1000;
    printf("=== LGFX Initialization Complete ===\n");
  }

  void lgfx_draw_test_pattern(void) {
    if (!gfx || !sprites[current_sprite]) return;

    // グラデーションテストパターンを描画
    LGFX_Sprite* sprite = sprites[current_sprite];
    sprite->clear();

    for (int x = 0; x < gfx->width(); ++x) {
      int v = x * 256 / gfx->width();
      sprite->fillRect(x, 0 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(v, v, v));  // グレースケール
      sprite->fillRect(x, 1 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(v, 0, 0));  // 赤
      sprite->fillRect(x, 2 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(0, v, 0));  // 緑
      sprite->fillRect(x, 3 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(0, 0, v));  // 青
      sprite->fillRect(x, 4 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(v, v, 0));  // 黄
      sprite->fillRect(x, 5 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(v, 0, v));  // マゼンタ
      sprite->fillRect(x, 6 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(0, v, v));  // シアン
      sprite->fillRect(x, 7 * gfx->height() >> 3, 1, gfx->height() >> 3, gfx->color888(v, v, v));  // 白
    }

    // FPS表示
    sprite->setCursor(2, 2);
    sprite->setTextColor(0x000000); // 黒（影）
    sprite->printf("MODE:%d FPS:%" PRIu32, test_mode, fps);
    sprite->setCursor(1, 1);
    sprite->setTextColor(0xFFFFFF); // 白
    sprite->printf("MODE:%d FPS:%" PRIu32, test_mode, fps);

    sprite->pushSprite(gfx, 0, 0);
    current_sprite = (current_sprite + 1) & 1; // 0と1を交互に
  }

  void lgfx_draw_moving_circles(void) {
    if (!gfx || !sprites[current_sprite] || !objects) return;

    LGFX_Sprite* sprite = sprites[current_sprite];
    sprite->clear(0x001122); // 暗い青背景

    // オブジェクトの移動と描画
    for (int i = 0; i < MAX_OBJECTS; i++) {
      objects[i].move(gfx->width(), gfx->height());
      sprite->fillCircle((int)objects[i].x, (int)objects[i].y, objects[i].radius, objects[i].color);
    }

    // FPS表示
    sprite->setCursor(2, 2);
    sprite->setTextColor(0x000000); // 黒（影）
    sprite->printf("MOVING CIRCLES FPS:%" PRIu32, fps);
    sprite->setCursor(1, 1);
    sprite->setTextColor(0xFFFFFF); // 白
    sprite->printf("MOVING CIRCLES FPS:%" PRIu32, fps);

    sprite->pushSprite(gfx, 0, 0);
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
    if (!gfx || !sprites[current_sprite] || !objects) return;

    LGFX_Sprite* sprite = sprites[current_sprite];
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
      obj1->move(gfx->width(), gfx->height());

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

    sprite->pushSprite(gfx, 0, 0);
    current_sprite = (current_sprite + 1) & 1;
  }

  void lgfx_set_test_mode(int mode) {
    if (!gfx || !objects) return;

    test_mode = mode;

    // モード変更時にオブジェクトをリセット
    if (mode == 2) { // 物理シミュレーションモード
      for (int i = 0; i < MAX_OBJECTS; i++) {
        objects[i].x = rand() % (gfx->width() - 40) + 20;
        objects[i].y = rand() % (gfx->height() / 2) + 20; // 上半分に配置
        objects[i].dx = (rand() % 400 - 200) / 100.0f; // -2.0 to 2.0 (faster)
        objects[i].dy = (rand() % 200) / 100.0f; // 0 to 2.0（下向き、faster）
        objects[i].radius = 8 + (rand() % 12);  // 8-19 (larger)
        objects[i].color = rand() | 0x404040;
      }
    }
  }

  int lgfx_get_test_mode(void) {
    return test_mode;
  }

  void lgfx_draw_random_rect(void) {
    if (!gfx) return;

    // ランダムな位置とサイズで矩形を描画
    int x = rand() % (gfx->width() - 16);
    int y = rand() % (gfx->height() - 16);
    int w = 8 + (rand() % 16);
    int h = 8 + (rand() % 16);
    uint32_t color = rand();

    gfx->fillRect(x, y, w, h, color);
  }

  void lgfx_test_resolution(int width, int height) {
    printf("=== Testing Resolution %dx%d ===\n", width, height);

    // 新しい解像度で設定を変更（実際にはこの方法では動的変更できない）
    printf("Resolution test would require recompilation with new settings\n");
    printf("Suggested memory usage: %d bytes per buffer\n", width * height);
    printf("Total memory needed: %d bytes (x2 for double buffer)\n", width * height * 2);

    // 現在の設定情報を表示
    if (gfx) {
      printf("Current display size: %" PRId32 " x %" PRId32 "\n", gfx->width(), gfx->height());
    }
  }

  void lgfx_print_memory_info(void) {
    printf("\n=== Memory Status ===\n");
    printf("Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());
    printf("Free PSRAM: %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Largest free block (heap): %zu bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    printf("Largest free block (PSRAM): %zu bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    printf("========================\n\n");
  }
  
  void lgfx_draw_test(){
    if (!gfx) return;
    gfx->fillRect(rand() % gfx->width() - 8, rand() % gfx->height() - 8, 100, 100, rand());
  }

  void lgfx_cleanup(void) {
    printf("=== LGFX Cleanup ===\n");

    // スプライトの削除
    for (int i = 0; i < 2; i++) {
      if (sprites[i]) {
        sprites[i]->deleteSprite();  // スプライトバッファを解放
        sprites[i]->~LGFX_Sprite();  // デストラクタ呼び出し
        heap_caps_free(sprites[i]);  // PSRAM解放
        sprites[i] = nullptr;
        printf("Sprite[%d] freed\n", i);
      }
    }

    // LGFXオブジェクトの削除
    if (gfx) {
      gfx->~LGFX();  // デストラクタ呼び出し
      heap_caps_free(gfx);  // PSRAM解放
      gfx = nullptr;
      printf("LGFX object freed\n");
    }

    // MovingObject配列の削除
    if (objects) {
      heap_caps_free(objects);  // PSRAM解放
      objects = nullptr;
      printf("MovingObject array freed\n");
    }

    printf("=== LGFX Cleanup Complete ===\n");
  }
}


void lgfx_test(void)
{
  // 初期化
  lgfx_init();

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

