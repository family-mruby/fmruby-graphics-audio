# Composite Region 機能で Canvas 転送の透明比較オーバーヘッドを削減

## Context (なぜこの変更をするか)

現在の Family mruby の画面合成 ([graphics_handler.cpp:338-356](../main/graphics/graphics_handler.cpp#L338-L356)) は、各 Canvas を **active 領域全体に対して1回の `pushSprite`** で合成している。

**前提**:
- ターゲットスクリーン: 320×240 (= 76,800 pixels / フレーム)
- **目標フレームレート: 余裕を持った 30fps** (= 内部処理は 30fps を安定して維持できる速度を確保したい)
- 現状: ~20fps 程度で頭打ち (CPU 処理が限界)

→ **目標達成には現状比 ~1.5倍 + 余裕分** の速度向上が必要。60fps 目標の重い最適化は不要だが、合成パスの無駄を削れば十分到達可能な範囲。

`use_transparent=true` の Canvas (= windowed app と system_desktop メイン Canvas) では、毎フレーム全 active 領域を読み込み・色比較する。代表的な無駄:

- **system_desktop メイン Canvas (z=254)**: 画面全体サイズだが実 content は上部 13px のメニューバーのみ。残り 95% 以上が透明色 (0x01) で、毎フレーム ~73,600 ピクセルを「読んで・比較して・捨てる」状態
- **全画面に近い windowed app**: rounded corner (四隅 数ピクセル) のためだけに全ピクセルで色比較が走る (~76,800 比較/フレーム)

Canvas サイズのリサイズは NG (system_desktop は dropdown / file selector / 各種ダイアログのため画面全体サイズが必要)。代わりに **「合成時に Canvas のどの部分・どんなモード (透明/不透明) で転送するか」をアプリ側から宣言的に制御できる仕組み** を導入する。

## 設計概要

2つの独立した機能を追加する:

### 機能A: CREATE_CANVAS に `rounded_corners` オプションを追加

`.app.toml` で `rounded_corners = false` を指定したアプリは、Canvas を `use_transparent=false` で作成。透明色比較自体が走らなくなる。デフォルトは true (後方互換)。fullscreen アプリは従来通り常に false。

### 機能B: `SET_COMPOSITE_REGIONS` RPC を追加

Canvas ごとに「合成領域の配列 (各要素: src_x, src_y, dst_x, dst_y, w, h, use_transparent)」を持つ。状態変化時 (アプリが UI を開閉した時など) のみ Core から送信。設定済みなら render loop はその領域だけを `pushImage + pixelcopy_t` で sub-rect 転送し、未設定なら従来通り active 領域全体を1回 `pushSprite`。

system_desktop は (1) 通常時は menu bar のみ、(2) dropdown/dialog 表示時はそれを加えた領域、と切り替える。

**性能根拠**: `pushImage` の opaque 経路は LovyanGFX 内部で `memcpy_P` (行単位コピー) に最適化されているため、矩形領域なら最速。透明色指定時は per-pixel の `fp_copy / fp_skip` ループになる (1ピクセルあたりのコストは従来 `pushSprite(transp)` と同等)。

ただし region 単位で `use_transparent` を独立に選べるので、**Canvas を「opaque な大領域 + transparent な狭い領域」に分割すれば、比較回数は全体として劇的に減る**。

| 用途 | region 構成 | 比較ピクセル数 | 備考 |
|---|---|---|---|
| 現状 (機能B 未適用) | 全画面 1 region, transparent | W × H (例: 130k) | 全領域走査 + 全比較 |
| system_desktop メニュー | (0,0,W,13) opaque 1枚 | **0** | メニュー以外は walk しない |
| Windowed app rounded corner (半径 2px) | 内部 (2,2,W-4,H-4) opaque + 4隅 各 2×2 transparent | **16** (=4×4) | 内部は memcpy、隅だけ比較 |
| Fullscreen app | (機能 A で `use_transparent=false`) | 0 | region 不要 |

つまり機能Bの旨味は2方向:
1. **走査領域そのものを減らす** (system_desktop メニューバー型 — region 外は walk すらしない)
2. **透明比較を狭い領域に閉じ込める** (rounded corner 型 — 大部分は opaque memcpy)

## 変更ファイル

### fmruby-graphics-audio (GA 側)

- **main/common/fmrb_link_protocol.h**:
  - 新規 RPC ID `FMRB_LINK_GFX_SET_COMPOSITE_REGIONS = 0x56`
  - 新規構造体 `fmrb_link_graphics_set_composite_regions_t { canvas_id, count, regions[] }` と `fmrb_link_graphics_composite_region_t { src_x, src_y, dst_x, dst_y, w, h, use_transparent, _pad }`
  - 1フレーム = 256B 制約 (`fmrb_link_protocol.h:16`) のため region 最大数 ~8 を確認 (1 region で int16×6 + uint8×2 = 14B、ヘッダ込みで ~250B に収まる目安)。超過時は CHUNKED 対応も検討

- **main/graphics/graphics_handler.cpp**:
  - `canvas_state_t` に `composite_region_t regions[MAX_COMPOSITE_REGIONS]; uint8_t region_count;` を追加 (`MAX_COMPOSITE_REGIONS = 8` 程度)
  - `graphics_handler.cpp:338-356` の合成ループを `region_count > 0` で分岐:
    - 設定あり: region 配列を順次、**`pushImage` + `pixelcopy_t` で sub-rect 転送** (詳細は下記「Sub-rect 転送の実装方針」参照)
    - 設定なし: 従来の `pushSprite` による active 領域全体 push
  - 新規 RPC ハンドラ `FMRB_LINK_GFX_SET_COMPOSITE_REGIONS` で region 配列を canvas_state_t にコピー (count=0 で fallback 復帰)
  - bg_canvas 経路 (`graphics_handler.cpp:332-334`) は変更しない (常に全コピー)

#### Sub-rect 転送の実装方針

`pushSprite` には source rect 指定がない。代わりに **destination 側の `pushImage(x, y, w, h, pixelcopy_t*)`** を `pixelcopy_t::src_x / src_y / src_bitwidth` を設定して呼ぶことで実現する。

LovyanGFX 内部挙動 (検証済み):
- `Panel_Sprite::writeImage` の fast path ([`LGFX_Sprite.cpp:429-465`](../components/LovyanGFX/src/lgfx/v1/LGFX_Sprite.cpp#L429-L465)) は、`transp == NON_TRANSP && no_convert && r == 0 && _img.use_memcpy()` が成立すると **`memcpy_P` で行単位コピー** (最速)
- 透明色指定時は per-pixel の `fp_copy / fp_skip` ループ (1ピクセルあたりのコストは現状の `pushSprite(..., transp)` と同等)。ただし region を分割して `use_transparent` を opaque / transparent に振り分ければ、**全体の比較ピクセル数を桁違いに削減できる** (上の「機能B」セクションの表参照)
- 第6引数 `use_dma` は Panel_Sprite では無視される (DMA は外部デバイス転送時のみ; sprite-to-sprite は常に CPU)

つまり本機能の効果は2方向:
1. **走査領域そのものを減らす** (system_desktop メニューバー型) — region 外は walk すらしない。menu bar が opaque region 1枚なら走査・比較・書き込みすべてゼロ化
2. **透明比較を狭い領域に閉じ込める** (rounded corner 型) — 内部を opaque region (memcpy)、隅だけ transparent region (per-pixel比較) に分割。比較ピクセル数が W×H から「隅のピクセル数」に激減

windowed app の rounded corners 用途では、機能 A (use_transparent=false) で 4 隅を直角にして比較ゼロにする道と、機能 B (region 分割) で 4 隅だけ透明比較する道の2択。アプリ毎に選択可能。

実装スケッチ:
```cpp
for (uint8_t i = 0; i < canvas->region_count; i++) {
    composite_region_t& r = canvas->regions[i];
    pixelcopy_t p(canvas->render_buffer->getBuffer(),
                  screen_buffer->getColorDepth(),
                  canvas->render_buffer->getColorDepth(),
                  false, nullptr,
                  r.use_transparent ? canvas->transparent_color : pixelcopy_t::NON_TRANSP);
    p.src_bitwidth = canvas->render_buffer_bitwidth;  // _bitwidth (実stride)
    p.src_x = r.src_x;
    p.src_y = r.src_y;
    screen_buffer->pushImage(canvas->push_x + r.dst_x, canvas->push_y + r.dst_y,
                             r.w, r.h, &p);
}
```

`pixelcopy_t` のコンストラクタは LovyanGFX の internal 寄りのため、必要に応じて graphics_handler.cpp 内に薄いヘルパー関数を切り出すか、`LGFX_Sprite` をサブクラス化して `pushSubSprite(dst, dst_x, dst_y, src_x, src_y, w, h, transp)` メソッドを生やすことも検討する。

`render_buffer_bitwidth` は `canvas_state_alloc` 時に `_bitwidth = (active_width + x_mask) & ~x_mask` で算出して保持する (8bpp 固定なら `active_width` と等価だが将来の color depth 拡張に備える)。

### fmruby-core (Core 側 — プロトコル/RPC クライアント)

- **main/common/fmrb_link_protocol.h**: GA 側と同じ enum/struct を追加 (mirror)

- **components/fmrb_gfx/fmrb_gfx.h** + **components/fmrb_gfx/fmrb_gfx.c**:
  - 新規 C API: `fmrb_gfx_err_t fmrb_gfx_set_composite_regions(gfx_ctx, canvas_id, regions, count)`
  - `count=0` で clear (= fallback 復帰)

### fmruby-core (Core 側 — アプリ管理 / TOML / Ruby API)

- **main/app/fmrb_app_spawner.c:245** 周辺: `default_window_mode` 同様に `rounded_corners` フィールドを TOML から読み取り、`attr->rounded_corners` (新規) に格納。未指定時のデフォルトは true

- **main/app/fmrb_app.c:1269** 周辺: `ctx->rounded_corners = attr->rounded_corners;` を追加 (`fmrb_app_task_context_t` に新規フィールド)

- **lib/add/picoruby-fmrb-app/ports/esp32/app.c:184-192**: CREATE_CANVAS 呼び出しの `use_transparent` 引数を現状の `!ctx->fullscreen` から `!ctx->fullscreen && ctx->rounded_corners` に変更

- **lib/add/picoruby-fmrb-app/mrblib/fmrb-app.rb**:
  - 新規 Ruby API: `gfx.set_composite_regions(regions)` — regions は `[{ src_x:, src_y:, dst_x:, dst_y:, w:, h:, transparent: }, ...]` または `nil`/`[]` で fallback
  - 簡易呼び出し用に `src_x/src_y` 省略時は `dst_x/dst_y` と同値、`dst_x/dst_y` 省略時は 0,0 とするデフォルト処理を Ruby 側で吸収
  - 内部で C API `fmrb_gfx_set_composite_regions` を呼ぶラッパーメソッドを追加 (`ports/esp32/app.c` 側に Ruby メソッドバインド追加が必要かを確認)

### fmruby-core (system_desktop 統合)

- **main/prebuild_scripts/kernel/system_desktop.app.rb**:
  - 起動完了後 (boot animation 終了時 = 既存の `finish_boot_animation`) に menu bar 領域のみを宣言:
    ```ruby
    @gfx.set_composite_regions([
      { src_x: 0, src_y: 0, dst_x: 0, dst_y: 0,
        w: @window_width, h: MENU_BAR_HEIGHT, transparent: false }
    ])
    ```
  - dropdown 開閉時 / launcher 開閉時 / file_selector 開閉時 / 各種 dialog 開閉時に regions を再計算して送信
  - 専用ヘルパー `update_composite_regions` を新設し、現在開いている UI 要素から region 配列を組み立てる
  - boot animation 中は region 設定を解除 (`set_composite_regions(nil)`) して従来通りの全画面合成

- 各サブモジュール (`system_desktop/launcher.rb`, file_selector 系, `confirm_dialog.rb`, `config_dialog.rb`, `clock_setting.rb`, `about_dialog.rb`, `error_dialog.rb`, `tbd_dialog.rb`): 開閉処理から `update_composite_regions` を呼ぶ

## 再利用する既存の仕組み

- 領域指定の RPC パターンは既存 `fmrb_link_graphics_update_window_t` (`fmrb_link_protocol.h:355-359`) と同じ packed struct スタイルを踏襲
- TOML パースは `fmrb_toml_get_string` / 既存の `default_window_mode` 読込みパターン (`fmrb_app_spawner.c:245`) を流用
- Ruby 側 API は既存 `@gfx` (FmrbGfx) クラスの他メソッドと同じ呼び出し規約に揃える

## Verification (動作確認)

### Linux ターゲット
```bash
cd <repo_root>/fmruby-core
rake clean       # lib/ 編集ありなので clean 必須
rake build:linux
# (ユーザが実行) 別ターミナルで GA を起動し、core を起動
```

確認項目:
1. **表示の互換性**:
   - 起動後、menu bar が正しく表示される
   - ファイル → Launcher を開く → 正しく表示・閉じる
   - ファイル → File Manager を開く → 正しく表示・閉じる
   - About / Config / Clock setting 各ダイアログ → 正しく表示・閉じる
   - ドロップダウンメニュー → 正しく表示・閉じる
2. **windowed app**:
   - shell / editor 起動 (rounded_corners 未指定 = default true) → 従来通り四隅丸角で表示
   - 試験的に `rounded_corners = false` を指定したアプリを作成 → 四隅が直角で表示 (退化なし)
3. **fullscreen app**:
   - raycaster 起動 → 従来通り全画面表示、終了後 desktop に戻る
4. **パフォーマンス計測**: GA 側のフレーム処理時間ログ (`graphics_handler.cpp:342` の `ESP_LOGD` を一時的に INFO 化、または FPS 表示機能) で **30fps + 余裕分** が安定して出ることを確認 (desktop 表示時 / windowed app 起動時の両方)

### ESP32 ターゲット
```bash
rake clean_all   # ターゲット切替なので必須
rake build:esp32
# (ユーザが実行) 書き込み・動作確認
```

確認項目:
- NTSC 出力が安定 (フレーム落ち無し)
- desktop / windowed app / fullscreen app の表示が Linux と一致
- SPI 通信エラーがログに出ない
- メモリプール (`FMRB_MEM_POOL_SIZE_SYSTEM_OVERLAY` 等) のサイズ超過がない

### リグレッション対策
- 既存全 `.app.toml` は `rounded_corners` 未指定 → デフォルト true → `use_transparent=true` で従来挙動を維持 (機能 A は opt-in)
- system_desktop 以外のアプリは `set_composite_regions` を呼ばない → 機能 B も opt-in
- 段階的にデプロイ可能 (機能 A→B の順 or 逆も可)

## 段階的実装の提案

実装を3段階に分けると安全:

1. **Phase 1**: 機能 A (`rounded_corners` オプション) のみ実装。fullscreen 以外の test app で動作確認 → コミット
2. **Phase 2**: 機能 B (`SET_COMPOSITE_REGIONS` RPC) を実装、graphics_handler 側の合成ループ拡張。system_desktop は未統合 → 単体テスト → コミット
3. **Phase 3**: system_desktop に region 設定を統合 (dropdown/dialog 各々) → 最終動作確認 → コミット

## Future Enhancement: 静的レイヤーキャッシュ (本計画スコープ外)

### 背景: DMA は使えない

ESP32 メモリ間 DMA (Canvas → Sprite, Canvas → screen_buffer) を活用したいところだが、ターゲットの ESP32-WROVER-E/IE (無印 ESP32) は以下の制約で実現困難:

- **GDMA controller 非搭載** (S3 専用)。LovyanGFX も `#if defined(SOC_GDMA_SUPPORTED)` で条件分岐 ([`components/LovyanGFX/src/lgfx/v1/platforms/esp32/common.cpp`](../components/LovyanGFX/src/lgfx/v1/platforms/esp32/common.cpp))
- LovyanGFX `Panel_Sprite::writeImage` の `use_dma` 引数は **Sprite 間コピーで無視**される ([`LGFX_Sprite.cpp:429`](../components/LovyanGFX/src/lgfx/v1/LGFX_Sprite.cpp#L429))。DMA は SPI/I2S 出力経路でのみ機能
- SPI/I2S loopback による mem-to-mem 流用テクニックは理論上可能だが、PSRAM のキャッシュ整合性・descriptor setup の煩雑さ・ESP-IDF 公式未サポートでリスク大

### DMA の代わりに: 「コピーしない」最適化

CPU memcpy 速度自体は ~10–20 MB/s 出ているため、目標 30fps の必要帯域 (320×240 × 30Hz × 1B = ~2.3 MB/s) は数値上余裕がある。にもかかわらず現状 ~20fps 止まりということは、ボトルネックは合成パス全体 (memcpy + 透明比較 + 描画) の総和、特に **system_desktop メイン Canvas の毎フレーム ~73,600 pixel 透明色比較** が支配的と推定される。

→ DMA で速くする方向ではなく、**「不要な走査・比較を消す」方向** が正解。本計画の機能 A/B はこの方針。30fps + 余裕分の達成には機能 A/B だけで十分届くと見込む。

### bg_canvas キャッシュ案 (Phase 4 候補)

現状 [`graphics_handler.cpp:332-334`](../main/graphics/graphics_handler.cpp#L332-L334) は毎フレーム `bg_canvas->draw_buffer->pushSprite(screen_buffer, 0, 0)` で 76,800 pixels の memcpy を実行。bg_canvas (system_desktop の壁紙) は壁紙変更時を除きほぼ静的なので、ここを以下の構造に変えられる:

- 追加バッファ `g_bg_cache` (320×240 × 1B = 76,800B) を確保 (PSRAM)
- bg_canvas の draw_buffer が更新された時 (`bg_canvas->dirty == true`) のみ `g_bg_cache` を再構築
- 各フレームは `g_bg_cache → screen_buffer` の memcpy で起動 (= 同じコスト)

→ 単純にコピー先を変えるだけでは利得ゼロ。**利得を出すには「静的な複数レイヤを cache に焼き込み、変化のあるレイヤだけ毎フレーム再合成」する必要がある**:

- 例: bg_canvas (壁紙) + system_desktop メニューバー静的部分 (時計以外) を `g_bg_cache` に焼き込む
- 各フレーム: `g_bg_cache → screen_buffer` + 動的レイヤ (時計・dropdown・dialog・user app) のみ合成
- 効果: 「2レイヤ memcpy + 透明比較」が「1 memcpy + 動的レイヤのみ」に圧縮

### 実装コスト・スコープ判断

- 追加メモリ: ~76KB (PSRAM 余裕次第)
- dirty tracking: bg_canvas / 静的 UI 要素の変更検出ロジック
- レイヤ分類 (静的 vs 動的) の設計

→ **機能 A/B で 30fps + 余裕分が達成できれば本案は不要**。本計画では Phase 4 候補として記録のみとし、実装は機能 A/B の効果計測後に判断する。