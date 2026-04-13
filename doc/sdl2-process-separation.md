# SDL2プロセス分離設計

## 背景

FreeRTOS Linux portはSIGALRM（1ms周期）でタスクスケジューリングを行うが、SDL2のX11バックエンドはselect()/poll()/connect()等のシステムコールを内部で使用する。SIGALRMがこれらをEINTRで中断し、以下の問題が発生していた：

- `SDL_Init(SDL_INIT_VIDEO)` が "x11 not available" で失敗
- SDLイベントループが断続的にフリーズ（入力応答停止・画面更新停止）
- `SDL_RenderPresent` のVSync待ちがSIGALRMで中断

これらは個別パッチでは根本解決できないため、SDL2を別プロセスに分離する設計とした。

## アーキテクチャ

```
変更前:
[fmruby-core] ──socket──> [fmruby-graphics-audio (FreeRTOS + SDL2)]

変更後:
[fmruby-core] ──socket──> [fmruby-graphics-audio] ──shm/socket──> [sdl2-display]
                           (FreeRTOS のみ)                          (純粋SDL2)
                           (LGFX_Sprite内部合成)                    (表示/入力/音声)
```

### 3コンテナ構成（docker-compose.yml）

| コンテナ | 役割 | SDL2依存 |
|----------|------|----------|
| sdl2-display | SDL2ウィンドウ表示、入力キャプチャ、オーディオ出力 | あり |
| fmruby-graphics-audio | FreeRTOSタスク、グラフィックス合成、APUエミュレーション | なし |
| fmruby-core | mruby VM、カーネル、アプリケーション実行 | なし |

## IPC（プロセス間通信）

### フレームバッファ転送（FreeRTOS → SDL2）

- **方式**: POSIX共有メモリ (`/dev/shm/fmrb_display`) + 名前付きセマフォ
- **データ**: RGB332ダブルバッファ 480x320 = 153,600 bytes/frame
- **頻度**: ~60fps
- **フロー**: graphics_handler が LGFX_Sprite で全キャンバスを合成 → display_shm が共有メモリにコピー → sem_post → sdl2-display が RGB332→RGB888変換して SDL_RenderPresent

### 入力イベント（SDL2 → FreeRTOS）

- **方式**: Unix socket (`/var/run/fmrb/fmrb_sdl_input`)
- **プロトコル**: `[type(1)][len(2)][data(len)]` （HIDイベント形式）
- **イベント型**: HID_EVENT_KEY_DOWN/UP, HID_EVENT_MOUSE_BUTTON, HID_EVENT_MOUSE_MOTION
- **フロー**: sdl2-display が SDL_PollEvent → HIDイベント変換 → socket送信 → input_handler_ipc が受信 → input_socket 経由で fmruby-core に転送

### オーディオ（FreeRTOS → SDL2）

- **方式**: 共有メモリ内リングバッファ（int16_t ステレオ PCM）
- **サンプルレート**: 15,720 Hz
- **フロー**: APUエミュレータ → apuif_ring_read → audio_handler_shm がリングバッファに書込 → sdl2-display の SDL2 audio callback が読取

### 共有メモリレイアウト（shm_display.h）

```c
typedef struct {
    // ハンドシェイク
    volatile uint32_t ready_magic;         // FMRB_SHM_READY_MAGIC (FreeRTOS側が設定)
    volatile uint8_t display_initialized;  // SDL2側が設定
    volatile uint8_t shutdown_requested;

    // ディスプレイパラメータ
    uint16_t display_width, display_height;
    uint8_t color_depth, scaling_x, scaling_y;

    // フレームバッファ（ダブルバッファ）
    uint8_t framebuf[2][480 * 320];  // RGB332
    volatile uint32_t write_index, read_index;

    // オーディオリングバッファ
    int16_t audio_ring[8192 * 2];  // ステレオ
    volatile uint32_t audio_write_pos, audio_read_pos;
} fmrb_shm_t;
```

### 起動シーケンス

```
1. sdl2-display 起動 → SHM待ち (shm_openリトライ)
2. fmruby-graphics-audio 起動 → FreeRTOSタスク生成
3. fmruby-core 接続 → INIT_DISPLAY メッセージ送信
4. display_shm_init: SHM作成 → ready_magic設定
5. sdl2-display: magic検出 → SDL2初期化 → display_initialized=1
6. display_shm_init: initialized確認 → LGFX_Sprite画面バッファ作成
7. graphics_task: レンダリングループ開始
8. fmruby-core: デスクトップアプリ起動
```

## ファイル構成

### 新規作成

| ファイル | 説明 |
|----------|------|
| `sdl2-display/main.c` | SDL2表示プロセス（フレーム表示・入力・オーディオ） |
| `sdl2-display/Makefile` | sdl2-displayビルド設定 |
| `sdl2-display/Dockerfile` | SDL2入りコンテナイメージ |
| `main/common/shm_display.h` | 共有メモリIPC定義（両プロセス共有） |
| `main/graphics/display_shm.cpp` | display_interface_t のSHM実装 |
| `main/audio/audio_handler_shm.c` | オーディオハンドラのSHM実装 |
| `main/input_linux/input_handler_ipc.c` | 入力ハンドラのIPC実装 |

### 変更

| ファイル | 変更内容 |
|----------|----------|
| `main/main_linux.cpp` | SDL2依存完全削除、graphics_taskをFreeRTOSタスクとして起動 |
| `main/CMakeLists.txt` | SDL2リンク削除、SHMファイル指定、rt/pthread追加 |
| `patches/lovyangfx-files/esp-idf.cmake` | LGFX_USE_SDL → LGFX_LINUX_FB |
| `patches/lovyangfx-files/common.hpp` | LGFX_LINUX_FB チェックをESP_PLATFORMの前に配置 |
| `patches/lovyangfx-files/device.hpp` | 同上 |

### 変更不要

| ファイル | 理由 |
|----------|------|
| `main/graphics/graphics_handler.cpp` | LGFX_SpriteはLovyanGFXを継承するためそのまま動作 |
| `main/tasks/graphics_task.cpp` | DISPLAY_INTERFACE抽象化経由で変更不要 |
| ESP32向けビルド全体 | 影響なし |

## LovyanGFX設定

- Linux builds: `-DLGFX_LINUX_FB` を定義（`LGFX_USE_SDL` の代わり）
- LGFX_Sprite はSDL2不要（Panel_Spriteは純粋なメモリバッファ操作）
- `LGFX_Sprite(nullptr)` でディスプレイバックエンドなしで動作
- `platforms/common.hpp` と `platforms/device.hpp` で `LGFX_LINUX_FB` チェックを `ESP_PLATFORM` より前に配置（ESP-IDF Linux buildでは `ESP_PLATFORM` が定義されるため）

## EINTR対策

FreeRTOSタスク内のシステムコールはSIGALRMでEINTRを受ける。以下の対策を実施：

- `shm_open`: do-whileでEINTRリトライ
- `sem_open`: do-whileでEINTRリトライ
- 待機ループ: `usleep` の代わりに `vTaskDelay` を使用（FreeRTOSタスク内）
- sdl2-displayプロセス: SIGALRMなし（FreeRTOS不使用のため対策不要）

## Docker設定のポイント

- `ipc: host`: POSIX共有メモリをコンテナ間で共有するために必要
- sdl2-displayコンテナのみX11/PulseAudio環境変数とボリュームを設定
- fmruby-graphics-audioコンテナにはSDL2/X11関連の設定不要
- SHMはホストの `/dev/shm` に永続化されるため、`ready_magic` によるstaleデータ検出が必要
