# Linux/SDL2 オーディオ実装計画

## 目的

Linux/SDL2シミュレーション環境でNES APUエミュレータの音声出力を有効にし、
ESP32実機なしでオーディオ開発ができる環境を構築する。

## 現状

### 動作するもの (ESP32のみ)
- `audio_check.c` がLittleFSから `.reglog` ファイルを読み込み再生
- APUエミュレータ (nofrendo) が 15720Hz/60fps で音声サンプルを生成 (約262サンプル/フレーム)
- I2S (またはPWMフォールバック) でハードウェアスピーカーに出力

### 動作しないもの (Linux)
- `components/apu_emu/CMakeLists.txt` がLinuxでは空スタブ (ヘッダのみ)
- `main/CMakeLists.txt` のLinuxビルドが `apu_emu` をREQUIRESしていない
- `audio_handler_sdl2.c` のコールバックが無音出力 (`memset(stream, 0, len)`)
- `audio_check.c` はLinux向けにコンパイルされない (ESP32専用のinclude)
- サンプルレート不一致: `audio_commands.h` では44100Hz定義、APUは15720Hz出力

## アーキテクチャ

```
  [audio_task]         [SDL2 オーディオスレッド]    [メインスレッド]
       |                     |                         |
  apuif_init()               |                   Panel_sdl::main()
       |                     |                    SDL_Init(VIDEO)
  60Hzループ:                |                    SDL_PollEventループ
   apuif_process()           |                         |
       |                     |                         |
   リングバッファ -------> audio_callback()            |
   (int16_t)              リングバッファから読み出し   LovyanGFX
                          SDLストリームへ書き込み      描画処理
```

### SDL2サブシステムの分離

LovyanGFXは `SDL_INIT_VIDEO` のみ初期化:
- ファイル: `components/LovyanGFX/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp:261`
- `SDL_Init(SDL_INIT_VIDEO)` -- ビデオのみ

オーディオハンドラは `SDL_INIT_AUDIO` を別途初期化:
- ファイル: `main/audio/audio_handler_sdl2.c:33`
- `SDL_InitSubSystem(SDL_INIT_AUDIO)` -- オーディオのみ

これらは独立したサブシステムであり、SDL2レベルでの競合はない。

### スレッド安全性: 初期化順序の注意点

`main_linux.cpp` では、タスク生成が `Panel_sdl::main()` より前:
```
app_main() {
    xTaskCreatePinnedToCore(audio_task, ...);   // SDL_InitSubSystemを呼ぶ可能性
    ...
    lgfx::Panel_sdl::main(user_func);          // SDL_Init(SDL_INIT_VIDEO)を呼ぶ
}
```

リスク: `SDL_InitSubSystem(SDL_INIT_AUDIO)` が `SDL_Init(SDL_INIT_VIDEO)` より先に
呼ばれる可能性がある。SDL2は `SDL_Init()` が最初に呼ばれることを前提としている。

解決策の候補:
1. `Panel_sdl::setup()` で `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)` を呼ぶ
   -- ただしLovyanGFXはgitsubmoduleなので編集禁止
2. audio_taskがSDL初期化完了をフラグ/セマフォで待つ
3. `app_main()` でタスク生成前に `SDL_Init(0)` を呼ぶ

推奨: **案3** -- `app_main()` の先頭で `SDL_Init(0)` を呼ぶ。
SDLコアをサブシステムなしで初期化し、ビデオとオーディオは後から各自追加可能にする。

## 実装手順

### 手順1: apu_if.h のLinux互換対応

ファイル: `components/apu_emu/include/apu_if.h`

ESP32専用のincludeとピン定義を `#ifndef CONFIG_IDF_TARGET_LINUX` で囲む:
```c
#ifndef CONFIG_IDF_TARGET_LINUX
#include "fmrb_pin_assign.h"

#define USE_I2S
#ifdef USE_I2S
#define PIN_BCK   FMRB_PIN_I2S_BCK
#define PIN_WS    FMRB_PIN_I2S_WS
#define PIN_DOUT  FMRB_PIN_I2S_DOUT
#else
#define AUDIO_PIN   FMRB_PIN_AUDIO_PWM
#endif
#endif /* CONFIG_IDF_TARGET_LINUX */
```

公開APIとstruct定義は変更なし。

### 手順2: apu_emuのLinuxビルドを有効化

ファイル: `components/apu_emu/CMakeLists.txt`

Linux用パスを空コンポーネントからソースコンパイルに変更:
```cmake
if (IDF_TARGET STREQUAL "linux")
    idf_component_register(
        SRCS "src/apu_if_linux.c"
             "src/nofrendo/nes_apu.c"
        INCLUDE_DIRS "include"
                     "src/nofrendo"
    )
else()
    # 既存のESP32ビルドは変更なし
endif()
```

補足: `nes_apu.c` は `esp_timer.h` をincludeしているが、
`APU_WRITE_DEBUG != 0` の場合のみ使用 (現状0)。
ESP-IDFのLinuxターゲットはスタブの `esp_timer.h` を提供するため問題なし。

### 手順3: apu_if_linux.c の新規作成

ファイル: `components/apu_emu/src/apu_if_linux.c` (新規)

`apu_if.cpp` のLinux版。同じ `apu_if.h` APIを提供し、ハードウェア依存なし。

主要な実装内容:
- `apuif_init()`: `apu_create(0, 15720, 60, 8)` を呼ぶのみ
- `apuif_frame_sample_count()`: ESP32と同じ固定小数点計算
- `apuif_process()`: ESP32と同じ `apu_process()` 呼び出し + サンプル変換
- `apuif_write_reg()` / `apuif_read_reg()`: `apu_write()`/`apu_read()` へのパススルー
- `apuif_audio_write()`: 共有リングバッファに書き込み (SDL2コールバックが読む)
- `apuif_read_entries()`: `heap_caps_malloc(MALLOC_CAP_SPIRAM)` の代わりに `malloc` を使用
- `apuif_set_external_process()` / `apuif_use_external_process()`: ESP32と同じフラグ管理

リングバッファ設計:
```c
#define AUDIO_RING_SIZE 8192
static int16_t ring_buffer[AUDIO_RING_SIZE];
static volatile uint32_t ring_read = 0;
static volatile uint32_t ring_write = 0;
static SDL_AudioDeviceID audio_dev_id = 0;  // audio_handlerがSDL初期化後に設定

// audio_handlerから呼ばれる設定関数
void apuif_set_sdl_device(SDL_AudioDeviceID dev);

void apuif_audio_write(const int16_t* s, int len, int channels) {
    SDL_LockAudioDevice(audio_dev_id);
    for (int i = 0; i < len; i++) {
        ring_buffer[ring_write & (AUDIO_RING_SIZE - 1)] = s[i];
        ring_write++;
    }
    SDL_UnlockAudioDevice(audio_dev_id);
}
```

### 手順4: main/CMakeLists.txt の更新

ファイル: `main/CMakeLists.txt`

Linux用REQUIRESに `apu_emu` を追加:
```cmake
if (IDF_TARGET STREQUAL "linux")
    set(REQUIRES LovyanGFX fmrb_pin_assign apu_emu)
```

Linux用SRCSに必要なオーディオソースを追加 (audio_check.cの統合、
またはaudio_task.cにリプレイロジックを組み込み)。

### 手順5: audio_handler_sdl2.c の書き換え

ファイル: `main/audio/audio_handler_sdl2.c`

変更内容:
1. SDL2オーディオを15720Hzで開く (SDL2が内部リサンプリング)
2. 無音コールバックをリングバッファ読み出しに置き換え
3. SDL_AudioDeviceIDを `apuif_set_sdl_device()` 経由でapu_if_linuxに渡す

```c
void audio_callback(void *userdata, Uint8 *stream, int len) {
    int16_t *out = (int16_t *)stream;
    int samples = len / sizeof(int16_t) / 2; // ステレオ
    for (int i = 0; i < samples; i++) {
        int16_t sample = 0;
        if (ring_read != ring_write) {
            sample = ring_buffer[ring_read & (AUDIO_RING_SIZE - 1)];
            ring_read++;
        }
        out[i * 2] = sample;     // L
        out[i * 2 + 1] = sample; // R (モノラル -> ステレオ)
    }
}

int audio_handler_init(void) {
    // ...
    want.freq = 15720;  // APU出力レートに合わせる
    want.format = AUDIO_S16LSB;
    want.channels = 2;  // ステレオ出力
    want.samples = 512;
    want.callback = audio_callback;

    // SDL2がリサンプリングできるよう周波数変更を許可
    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                       SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    apuif_set_sdl_device(audio_device);
    SDL_PauseAudioDevice(audio_device, 0); // 即座に再生開始
    // ...
}
```

### 手順6: audio_taskに60Hzリプレイループを追加 (Linux用)

ファイル: `main/tasks/audio_task.c`

Linux用パスは現在アイドル状態。APU処理ループを追加:

```c
void audio_task(void *pvParameters) {
#ifndef CONFIG_IDF_TARGET_LINUX
    audio_check_impl();  // ESP32: 既存パス
    return;
#endif

    // Linuxパス
    if (audio_handler_init() < 0) {
        ESP_LOGE(TAG, "Audio handler initialization failed");
        return;
    }

    // APUエミュレータ初期化
    apuif_init();

    // テスト用reglogファイルの読み込み (パスは要検討)
    apu_log_header_t header;
    apu_log_entry_t *entries = apuif_read_entries("test_data/sample.reglog", &header);

    if (entries) {
        // audio_check.cから抽出したリプレイ初期化処理
        replay_init(entries, &header);
    }

    // 60Hz処理ループ
    const uint32_t frame_interval_ms = 16; // 約60Hz
    while (task_running) {
        if (entries) {
            replay_frame(entries, &header);  // フレームごとのレジスタ書き込み
        }

        int16_t buffer[528]; // (262+1)*2
        memset(buffer, 0, sizeof(buffer));
        int count = apuif_process(buffer, sizeof(buffer));
        if (count > 0) {
            apuif_audio_write(buffer, count, 1);
        }

        vTaskDelay(pdMS_TO_TICKS(frame_interval_ms));
    }

    if (entries) free(entries);
    audio_handler_cleanup();
}
```

### 手順7: SDL_Init の順序修正

ファイル: `main/main_linux.cpp`

タスク生成前に `SDL_Init(0)` を追加:

```c
int app_main(void) {
    SDL_Init(0);  // SDLコアを初期化 (サブシステムなし)
    SDL_ShowCursor(SDL_DISABLE);
    // ... 以降変更なし
}
```

これにより、audio_taskの `SDL_InitSubSystem(SDL_INIT_AUDIO)` が
`Panel_sdl::setup()` の `SDL_Init(SDL_INIT_VIDEO)` より先に呼ばれても安全になる。

## テストデータ

テスト用の `.reglog` ファイルが必要。

現在ESP32での配置場所: `/flash/data/sample.reglog` (LittleFSパーティション)

Linux用の配置方法の候補:
- docker-compose.ymlでテストデータディレクトリをマウント
- リポジトリにテスト用reglogを含める (例: `test_data/sample.reglog`)

## 検証方法

1. `rake build:linux` -- ビルド成功を確認
2. `docker compose up` -- 両コンテナが起動
3. PulseAudio経由 (WSL2g) でオーディオが出力されることを確認
4. APUエミュレータが正しいNESスタイルのチップチューン音声を生成

## リスク

| リスク | 対策 |
|--------|------|
| SDL2が15720Hzを受け付けない | `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE` でSDL2の内部リサンプリングを利用 |
| DockerでPulseAudioが利用不可 | docker-compose.ymlで既に `/mnt/wslg/PulseServer` をマウント済み |
| リングバッファアンダーラン (音飛び) | 約4フレーム分 (1024サンプル) のバッファを確保、デバッグ時に監視 |
| nes_apu.c のLinuxコンパイルエラー | コア部分は移植性の高いC。`esp_timer.h` は条件付き (未使用パス) |

## 変更ファイル一覧

| ファイル | 変更内容 |
|----------|----------|
| `components/apu_emu/include/apu_if.h` | ESP32専用includeをifdefガード |
| `components/apu_emu/CMakeLists.txt` | Linuxビルドでapu_if_linux.cとnes_apu.cをコンパイル |
| `components/apu_emu/src/apu_if_linux.c` | **新規**: Linux用APUインタフェース (リングバッファ出力) |
| `main/CMakeLists.txt` | Linux用REQUIRESに `apu_emu` を追加 |
| `main/audio/audio_handler_sdl2.c` | リングバッファ読み出しコールバック、15720Hz出力 |
| `main/tasks/audio_task.c` | Linux用60Hz APU処理ループ追加 |
| `main/main_linux.cpp` | タスク生成前に `SDL_Init(0)` を追加 |
