# Ardour MCP / LLM 制御サーフェス — 実装ハンドオフ（macOS arm64 / fresh build）

> **ABSTRACT (English, for any LLM picking this up cold):** This document is a complete, self-contained handoff for continuing work on a **clean-room hardening of Ardour's experimental MCP-over-HTTP control surface** so an LLM can drive Ardour by natural language (audio-app analogue of Codex/Cursor). The branch `feature/mcp-fresh-macos` already builds end-to-end on macOS arm64 against Homebrew deps, the patched MCP surface dylib loads at runtime, and the MCP protocol has been live-verified with curl (initialize, tools/list, Host-header rejection, and a new `track/get_meter` tool). You do **not** need the originating chat. Prose is Japanese, but file paths, identifiers, commands, and code are English — every claim carries a `file:line` or commit citation you can verify. Start at §0, then §1, then §3 (verified run book) or §6 (roadmap for next waves).

> **See also:** project-wide master handoff at https://github.com/masatomoota/llm-daw-handoff (chronological narrative, decision tree, prioritized roadmap to 100%, auto-start protocol).

---

## 0. このドキュメントの使い方・前提

- **目的**：Ardour に LLM 自然言語制御を載せる作業の**継続用ハンドオフ**。本作業は単発で完了するものではなく、Phase 0（堅牢化）まで到達済みで、Phase 1〜4（カバレッジ完成、フィードバックループ拡張、アプリ内 UX、製品化）が残っている。
- **読者**：このスレッドを知らない別の LLM / エンジニア。**この文書だけで 100% 継続できる**ことを意図している。
- **対象リポ**：Ardour fork（`frankp` 由来、`9.7-88-gb25a63c74a` 起点）。ブランチ `feature/mcp-fresh-macos` に Wave 0〜2 を実装済み。
- **環境**：macOS Apple Silicon（arm64）/ Apple clang 17 / Homebrew 6.x / Python 3.9。並行する Audacity 版ハンドオフは別リポ（`audacity` の `mcp-llm-handoff` ブランチ、`masatomoota/audacity` フォーク）に存在。

---

## 1. 中核背景（なぜこの設計か）

### 1.1 出発点
当該フォークには既に実験的な MCP-over-HTTP コントロールサーフェスが存在する：`libs/surfaces/mcp_http/`（`mcp_http_server.cc` 約 8{,}160 行、`tools_json.inc` 約 96 ツール、libwebsockets ベース、ポート 4820、JSON-RPC 2.0 / `protocolVersion 2025-03-26`）。Codex CLI / Claude Desktop / Gemini / Ollama から接続できる。

### 1.2 出発点の3つの欠陥（先行レビューで確定）
1. **スレッド / RT 安全性**：lws サービススレッドから直接 `ARDOUR::Session` を変更し、GUI スレッドと並行する `begin_reversible_command` が **`HistoryOwner::_current_trans`（無ガード）** を破壊する。debug ビルドで `assert(false)` クラッシュ。
2. **知覚ループ欠如**：オブザーバ / SSE / メータ読み出しが皆無。LLM が結果を観測できない。
3. **セキュリティ**：認証なし・平文・**全インタフェース待受**（`_info.iface` 未設定）、`endpoint_url()` 表示の `127.0.0.1` と実待受が乖離。

### 1.3 設計判断：fresh-build vs harden-existing
2 案を検討した結果、**「動く 96 ツールを土台に、壊れている中核だけをクリーンルームで是正」**を採用（fix_plan v2）。理由：
- 95 ツールの再実装は工数膨大、コスト節約のご要望と矛盾。
- 既存実装は正しい部分（ツール意味論、`canonical_tool_name` 別名処理、structured+text フォールバック、`PBD::ID` ガード）も多く、捨てるのは惜しい。
- 壊れている中核（スレッド境界、bind、Host 検証）は明確に局所化できる。
- 著作権上の懸念があれば後続で別ディレクトリの fresh surface に切り替え可能（土台は同じ）。

### 1.4 関連設計ドキュメント（リポ外、参考）
作業ホストの `/Volumes/work-ssd-4TB-USB4/_Git_Repository/llm-daw-report/` に PDF 4 部（Ardour 改造可能性調査、MCP 是正実装プラン v2、Audacity 改造可能性レビュー、ライセンス・コンプライアンス）。**本ハンドオフはこれらに依存せず単体で完結**。読めば設計の経緯がより深く理解できる、というだけ。

---

## 2. リポジトリ状態（事実）

### 2.1 ブランチ・コミット
- ブランチ：`feature/mcp-fresh-macos`（`master` から分岐）
- コミット（古い順 → 新しい順）：
  ```
  b25a63c74a Refine f8f2572f use only with MINGW/Windows                   (HEAD~5, upstream)
  356c5839cf Fix USB surface compile when libusb-dev is not installed       (HEAD~4, upstream)
  0834ec2610 build: enable macOS (arm64) dev build against Homebrew deps    (HEAD~3, Wave 0)
  36b0f04fb0 mcp_http: harden — thread marshaling, localhost bind, Host header check  (HEAD~2, Wave 1a)
  5129c6d773 mcp_http: add track/get_meter — real-time peak readback (dBFS)            (HEAD~1, Wave 1b)
  ```
- `git status` クリーン（本 MD コミット前）。

### 2.2 ワーキングツリーの汚れ
- 追跡対象外の変更：なし（本ハンドオフ MD のみが untracked → 本ハンドオフのコミットで解消）。
- `build/` は `.gitignore` 対象（追跡なし）。

### 2.3 GitHub 同期先
- `origin` = `https://github.com/Ardour/ardour.git`（push 権なし）
- `fork` = `https://github.com/masatomoota/ardour.git`（本作業者のフォーク、本ハンドオフコミット時に push）

---

## 3. 検証済みのラン・ブック（環境構築 → ビルド → 起動 → MCP 疎通）

これは **Wave 2 で実機検証パス済み**の手順。新しいマシンや別 LLM がゼロから再現できる。

### 3.1 ホスト要件
- macOS Apple Silicon（arm64）。Intel Mac は同じ手順で通る可能性が高いが未検証。
- Xcode Command Line Tools（`xcode-select -p` で確認）。
- Homebrew（`/opt/homebrew`）。
- 空き 5GB 程度（ビルド成果物 約 700MB ＋ Homebrew formula）。

### 3.2 Homebrew 依存導入
```bash
# Wave 0 で必要十分が確定した一式（旧ABI GTK2 C++バインディングを含む）
brew install pkg-config glib glibmm@2.66 libsndfile curl libarchive liblo \
  taglib vamp-plugin-sdk rubberband fftw libsamplerate libxml2 lv2 lilv suil \
  boost libwebsockets aubio gettext \
  cairomm@1.14 pangomm@2.46 atkmm@2.28
```
**重要**：`glibmm@2.66` / `cairomm@1.14` / `pangomm@2.46` / `atkmm@2.28` は keg-only。Ardour は旧 GTK2 系の C++ バインディング ABI（`glibmm-2.4`, `cairomm-1.0`, `pangomm-1.4`, `atkmm-1.6`）を要求するため、Homebrew のデフォルト新版（`glibmm`, `cairomm`, `pangomm`, `atkmm`）では**通らない**。

### 3.3 PKG_CONFIG_PATH（再現性の鍵）
全ての configure / build 呼び出しで以下を設定する（`.zshrc` に入れず、ビルド時のみ export 推奨）：
```bash
PCP=""
for f in glibmm@2.66 curl libarchive libxml2 cairomm@1.14 pangomm@2.46 atkmm@2.28 libsigc++@2; do
  d="$(brew --prefix $f 2>/dev/null)/lib/pkgconfig"; [ -d "$d" ] && PCP="$PCP:$d"
done
export PKG_CONFIG_PATH="${PCP#:}:/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig"
```

### 3.4 Configure
```bash
cd /path/to/ardour
GETTEXT="$(brew --prefix gettext)"
python3 ./waf configure --with-backends=coreaudio,dummy \
  --also-include="/opt/homebrew/include,$GETTEXT/include" \
  --also-libdir="/opt/homebrew/lib,$GETTEXT/lib" \
  --boost-include=/opt/homebrew/include
```
所要時間：約 4 秒。最終行 `'configure' finished successfully` を確認。

### 3.5 Build
```bash
python3 ./waf -j10 2>&1 | tee /tmp/ardour_build.log
```
所要時間：初回フルビルド 5〜10 分（10 コア M4 で 5 分実測）。**`echo $?` は信用しない**——シェル末尾の trailing pipe が exit code をマスクすることがある。実成否は：
```bash
grep -E "'build' finished successfully|Build failed" /tmp/ardour_build.log | tail -3
```
で判定。`error:` カウントも `grep -c 'error:' /tmp/ardour_build.log` で確認。

### 3.6 主要成果物
```
build/gtk2_ardour/ardour-9.7.89                                   # ELF 実行バイナリ ~73MB
build/libs/ardour/libardour.dylib                                  # ~53MB
build/libs/surfaces/mcp_http/libardour_mcp_http.dylib              # ~2.9MB（MCP サーフェス本体）
build/libs/surfaces/{osc,websockets,mackie,...}/libardour_*.dylib  # 各種サーフェス
```

### 3.7 起動
```bash
./gtk2_ardour/ardev                                # dev-run ラッパ（DYLD・サーフェスパス自動設定）
# または既存セッションを開く場合:
./gtk2_ardour/ardev /path/to/some/session.ardour
```
`ardev` は `gtk2_ardour/ardev_common_waf.sh`（waf が生成）を source して `ARDOUR_SURFACES_PATH`・`DYLD_FALLBACK_LIBRARY_PATH` 等を設定する。`ARDOUR_SURFACES_PATH` に `libs/surfaces/mcp_http` が含まれる（`gtk2_ardour/ardev_common.sh.in:22` 参照）。

### 3.8 MCP サーフェスの有効化（初回のみ）
GUI で：`Edit > Preferences > Control Surfaces` → `MCP HTTP Server (Experimental)` のチェックを on → ダイアログでポート（既定 4820）確認。状態は Ardour の per-user config（`~/Library/Preferences/Ardour9/` 等）に保存され、以後の起動で自動有効化される。

### 3.9 MCP 疎通（curl による live verification）
Wave 2 で実機通過した手順そのもの：
```bash
# initialize
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' \
  http://127.0.0.1:4820/mcp
# expect: {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-03-26",
#         "capabilities":{"tools":{"listChanged":false}},
#         "serverInfo":{"name":"ardour-mcp-http","version":"0.1.0"}}}

# tools/list — 96 tools incl. track_get_meter
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  http://127.0.0.1:4820/mcp \
  | python3 -c "import json,sys; t=json.load(sys.stdin)['result']['tools']; print(len(t), [x['name'] for x in t if 'meter' in x['name']])"

# Host header security: must return 403
curl -v -H 'Host: evil.example.com:4820' -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"initialize","params":{}}' \
  http://127.0.0.1:4820/mcp

# track_get_meter (replace <route_id> with an actual route's PBD::ID)
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"track_get_meter","arguments":{"id":"<route_id>"}}}' \
  http://127.0.0.1:4820/mcp
```

### 3.10 LLM クライアント接続
```
codex mcp add ardour --url http://127.0.0.1:4820/mcp
gemini mcp add ardour http://127.0.0.1:4820/mcp --type http
# Claude Desktop: mcp-remote 経由（README.md 参照）
```

---

## 4. Wave 0 の変更（macOS ビルド成立化）

**コミット `0834ec2610`** — 「素の Ardour を Homebrew 依存 macOS arm64 でビルド可能にする」最小修正 5 箇所：

| ファイル | 変更 | 理由 |
|---|---|---|
| `libs/tk/ydk/wscript` | darwin ブロックに `obj.defines += ['DISABLE_VISIBILITY']` 追加 | clang on Mach-O は `__attribute__((alias))` 非対応。`gdkaliasdef.c` がコンパイル不能だった。`DISABLE_VISIBILITY` で alias 定義全体を `#ifndef` ガード（`gdkaliasdef.c:3`）が無効化する。 |
| `libs/tk/ytk/wscript` | 同上 | `gtkaliasdef.c` も同根。 |
| `libs/lua/wscript` | vendored Lua 5.3.5 include を `CXXFLAGS` の先頭に prepend | `--boost-include=/opt/homebrew/include` 経由で `-I/opt/homebrew/include` が global CXXFLAGS の先頭近くに入り、`#include "lua/lua.h"` が **Homebrew Lua 5.5 の `/opt/homebrew/include/lua/lua.h`** を引いてしまう（5.3 と 5.5 で API 差異あり、`LUA_GCSETPAUSE` 等が消滅）。ターゲット固有の `includes=['.']` は `INCPATHS` 側で `CXXFLAGS` より後ろにくるため上書き不能。`env.derive()` で当該ターゲットのみ vendored include を勝たせる。 |
| `libs/ardour/wscript` | `obj.uselib` に `'ARCHIVE'` 追加 | `pbd/file_archive.h` 経由で `<archive.h>` を引くが、Homebrew `libarchive` は keg-only で `/opt/homebrew/opt/libarchive/include/` にある。 |
| `libs/ctrl-interface/control_protocol/wscript` | `'ARCHIVE'` を uselib に追加 | 同上（`session.h` 経由で `<archive.h>` を間接 include）。 |

**全て build-system レベルの修正**で、ソースコード（`.cc`/`.h`）は無改変。**Linux/Windows ビルドへの影響なし**（変更は `darwin` ブロック内 or pkg-config uselib 追加のみ）。

---

## 5. Wave 1 の変更（MCP サーフェス Phase 0 ハードニング）

### 5.1 コミット `36b0f04fb0` — Phase 0 堅牢化（4 修正）
1. **スレッド整流**（`mcp_http_server.cc`）：`tools/call` を `_event_loop->call_slot(MISSING_INVALIDATOR, lambda)` + `std::condition_variable` で**GUI/イベントループスレッドへマーシャル**し、condvar で結果を同期取得。lws サービススレッドから直接 Session を変更しない。実装：`run_tools_call()` 自由関数を抽出（7 つのグループディスパッチャを呼ぶ）し、`dispatch_jsonrpc()` の `tools/call` 分岐がこれを `call_slot` 経由で呼ぶ。
2. **再入デッドロック回避**（`mcp_http_server.cc`）：既存の macOS プラグイン追加 `call_slot`（`#ifdef __APPLE__`）に `PBD::EventLoop::get_event_loop_for_thread() != _event_loop` ガードを追加。`tools/call` 外側が既にマーシャル済みなら、内側は直呼び（さもなくば自スレッドへ slot を投げて自分が処理する slot を待つ→デッドロック）。
3. **Localhost bind**（`mcp_http_server.cc`）：`MCPHttpServer::start()` で `_info.iface = "127.0.0.1"` を設定。これまで未設定で libwebsockets 既定の全インタフェース（`0.0.0.0`）待受になっており、`endpoint_url()` 表示と乖離していた。
4. **Host ヘッダ検証**（`mcp_http_server.cc`）：`host_header_is_loopback()` 静的ヘルパを追加し、`handle_http` の POST 分岐で `WSI_TOKEN_HOST` を取得 → loopback 系（`127.0.0.1` / `localhost` / `::1`）以外なら HTTP 403。Host 欠落は許可（local CLI クライアント互換）。**DNS-rebinding 対策**：loopback bind だけでは、悪意あるブラウザページが `127.0.0.1` に解決するホスト名経由で POST し得るため。

**Wave 2 で全て実機検証パス**：Host: evil.example.com → 403、tools/call の dispatch path 動作確認、debug build で assert なし。

### 5.2 コミット `5129c6d773` — 知覚層第一歩（track/get_meter）
新ツール `track/get_meter`：
- `mcp_http_server.cc` に `handle_track_get_meter_tool()` 追加（`route->peak_meter()->meter_level(n, ARDOUR::MeterPeak)` をチャンネル毎に dBFS で返し、`maxPeakDb` も付ける）
- `dispatch_track_tool_call()` に分岐登録
- `tools_json.inc` にスキーマ追加（`outputSchema` 付き）
- `#include "ardour/meter.h"` 追加
- 既存 `us2400/strip.cc:666`、`push2/level_meter.cc:160` と同型 API 使用

Wave 2 で実呼び出し成功：master bus（id=22, 2ch, transport stopped）→ `{"id":"22","name":"master","channels":2,"peakDb":[null,null],"maxPeakDb":null}`（停止中なので null は期待通り）。

### 5.3 仕様逸脱（軽微 1 件）
パッチ仕様（`/tmp/mcp_hardening_patches.md`, 作業ホストのみに残る）では `run_tools_call()` の `root` 引数を `const pt::ptree&` としていたが、`dispatch_midi_region_tool_call` の第 3 引数が非 const `pt::ptree&` を要求するため、**非 const ref へ変更**して適用。他のディスパッチャは const を受けるので両立、ビルド OK。

---

## 6. Wave 2 検証結果と既知事項

### 6.1 検証パス項目
| 項目 | 結果 | 備考 |
|---|---|---|
| dylib (arm64 Mach-O) + `protocol_descriptor` export | ✅ | |
| libwebsockets リンク | ✅ | `/opt/homebrew/opt/libwebsockets/lib/libwebsockets.21.dylib` |
| `tools_json.inc` 妥当 JSON / 96 tools | ✅ | `track_get_meter` 含む |
| 起動 → 4820 LISTEN | ✅ | |
| `initialize` protocolVersion 2025-03-26 | ✅ | |
| `tools/list` 96 tools | ✅ | |
| `hello_world`, `session_get_info` 疎通 | ✅ | |
| `track_get_meter` 実呼び出し | ✅ | |
| Host header rejection (HTTP 403) | ✅ | |
| ブレース整合（HEAD と同じ -1）| ✅ | 文字列リテラル内 `}` 起因の元から -1、編集で増減なし |

### 6.2 既知事項（ハンドオフ・要記録）
1. **GTK on Quartz の表示問題**：`nohup ... &` で起動するとウィンドウが描画されないが、プロセスは生きて MCP は応答する。普通に Finder からか TTY のある terminal から起動すれば OK。**MCP 動作には影響なし**。
2. **空セッションの自動作成は非自明**：Wave 2 検証で手動セッション XML を作るのに `version="3001"`, `<Session sample-rate="48000">`, `<Config>` 内 `native-file-data-format` 必須。`get_info_from_path` が緩いとセッション認識せず。**通常運用では既存セッションを開けば良いので問題なし**。
3. **コンパイル時のリンカ警告**：`ld: warning: building for macOS-11.0, but linking with dylib ... which was built for newer version 26.0`。Homebrew の新しい macOS 用ビルドのため。**機能影響なし**。
4. **VST3 オプション**：`configure` で `VST3 support: True` 確定。VST3 入りバイナリは実効 GPLv3（VST3 SDK の GPLv3 オプション選択による）。ライセンス §9 参照。

---

## 7. アーキテクチャ概要（変更後の MCP サーフェス）

```
[LLM client] --HTTP POST /mcp---> [lws service thread]
                                  dispatch_jsonrpc(payload)
                                  │
                                  ├ initialize / ping / tools/list  ← lws スレッドで直接処理（read-only）
                                  │
                                  └ tools/call
                                    │
                                    ├ (a) loopback Host check        ← handle_http (POST branch) で事前検証
                                    │
                                    └ EventLoop::call_slot(MISSING_INVALIDATOR,
                                          lambda { run_tools_call(...) })
                                      condvar.wait(done)              ← lws スレッドはここでブロック
                                                                       
                                          ▼                            ▲
                                      [GUI/event-loop thread]          │
                                      run_tools_call()                 │
                                          ├ dispatch_track_tool_call   │
                                          ├ dispatch_session_tool_call │
                                          ├ dispatch_transport_...     │
                                          ├ dispatch_markers_...       │
                                          ├ dispatch_tracks_...        │
                                          ├ dispatch_plugin_tool_call ← _event_loop 渡しても 
                                          │                              内部の APPLE call_slot は
                                          │                              get_event_loop_for_thread() ==
                                          │                              _event_loop で再入回避
                                          └ dispatch_midi_region_...   │
                                              ↓                        │
                                          ARDOUR::Session 変更（Undo 安全）
                                              ↓                        │
                                          response 文字列 ─────────────┘
                                                                       
                                  ▼
                                  HTTP response (structuredContent + text fallback)
```

Wave 0 出発点との差分：(1) `tools/call` 全体が GUI スレッドへ整流、(2) listen `0.0.0.0` → `127.0.0.1`、(3) Host loopback 検証、(4) `track/get_meter` ツール追加。

---

## 8. 残作業ロードマップ（Phase 1〜4）

設計済み（先行レビュー由来）の残タスク：

### Phase 1 — カバレッジ完成（大、低リスク）
- オートメーション（レーン / カーブ / モード）
- テンポマップ / 拍子編集（`tempo.h:785` の copy-on-write `TempoMap`）
- フェード / クロスフェード
- プラグインプリセット（`plugin.h`）
- VCA / グループ
- オーディオ import
- `region_by_id` 便宜関数（Session に直接無いため `RegionFactory`/`Playlist` 経由を吸収するヘルパ）
- 新規ツールには `outputSchema` を付与（現状 42/96 のみ）

### Phase 2 — フィードバック / 知覚（高、差別化の本丸）
- WebSockets / OSC の observer パターン移植（`feedback.cc:196` `update_all_clients` / `osc_route_observer.cc:159` `Changed.connect`）
- MCP `notifications/*` または **SSE チャネル**で状態変化を非同期 push
- 同期的な render/bounce/stem-export ツール（パス＋解析サマリ）
- ターン制ロック（fix_plan v2 §5：人間と LLM の編集を排他、自動 snapshot をターン境界で取得）→ knowing observation loop が不要化する代替パス

### Phase 3 — アプリ内エージェント UX（中、低リスク）
- 埋め込みチャットパネル（`editor.cc:5616` の `add_notebook_page` で 1 行）
- dispatch 層を `static` から翻訳単位外公開（gtk2_ardour から直接 `dispatch_jsonrpc` を呼ぶ）
- LLM 通信 worker スレッド + `call_slot` ツール実行 + 選択コンテキスト注入
- Codex 風ステップ可視化（`structuredContent` 表示）

### Phase 4 — 製品化・安全
- トランザクション `begin_batch`/`commit_batch`（失敗時 `abort`）
- バッチ前自動 snapshot
- dry-run / 差分プレビュー
- 名前 + 選択ベースの決定論的ターゲティング（marker 既に `mcp_http_server.cc:1110-1133` で良い手本あり）
- コアの `PBD::ID(string)` を fail-closed 化（`id.cc:54-58` の "danger, will robinson" コメント参照）
- トークン認証 + TLS（lws は対応、`LWS_WITH_SSL=OFF` の Homebrew ビルドだと別問題）
- Win11 ビルド（先行レビューで判定済の 2 ブロッカー：`libs/surfaces/mcp_http/wscript` に `_WIN32_WINNT=0x0601` 追加が必要、ardour-build-tools の Windows スタックに libwebsockets を追加）

---

## 9. ライセンスと公開（masatomoota フォークとして配布する場合）

### 9.1 本体ライセンス
**Ardour は GPLv2-or-later で一様**（`COPYING`、サンプル 290 ファイルが v2-or-later、v3 はわずか 1 ファイル）。あなたの MCP 修正は既存サーフェスのヘッダに整合（GPLv2-or-later）で問題なし。

### 9.2 追加依存
- **libwebsockets = MIT**（GPL 互換）
- 配布 binary に同梱する場合は `LWS_WITH_SSL=OFF` でビルドすると OpenSSL 同梱問題を完全回避できる（本実装は TLS 不使用）

### 9.3 派生関係
`COPYING` の Plugin Clarification は **第三者 API プラグイン（VST/AU/LV2 等）を非派生**と整理。MCP サーフェスは `libardour` の**内部 C++ API にリンク**するので派生物＝GPL 必須——だが**実際に GPLv2 ヘッダ付き**なので矛盾なし。

### 9.4 商標
配布バイナリは **"Ardour" 名 / アイコンを外してリブランド**（Mixbus / Harrison 前例。コミュニティはフォークに寛容）。

### 9.5 CLA / 上流貢献
Ardour に CLA 機構なし（Audacity と異なる）。上流に PR を出す場合は GPLv2-or-later で送るだけ。**Phase 0 ハードニングは上流還元価値が高い**（assert クラッシュ修正なので）。

### 9.6 GPL は「無料」ではない
GPLv3/v2 とも有料配布可（§4）。義務は「配布相手への完全ソース提供」「相手の自由を制限しない」「表示の保持」のみ。詳細は作業ホストの `llm-daw-report/license_report.pdf` 参照。

---

## 10. 主要ファイル・マップ

| 役割 | パス | コメント |
|---|---|---|
| MCP サーフェス本体 | `libs/surfaces/mcp_http/mcp_http_server.cc` | 8{,}160+ 行（hardening 後）。`dispatch_jsonrpc`, `run_tools_call`, `handle_track_get_meter_tool` 等が中核 |
| MCP サーフェス薄ラッパ | `libs/surfaces/mcp_http/mcp_http.cc` / `.h` | `ControlProtocol` 継承、222 行 |
| サーフェス登録 | `libs/surfaces/mcp_http/interface.cc` | `protocol_descriptor` を C リンケージで export |
| ツールカタログ | `libs/surfaces/mcp_http/tools_json.inc` | 96 tools の JSON-Schema（`#include` で `static constexpr std::string_view` として埋め込み）|
| ビルド統合 | `libs/surfaces/mcp_http/wscript` | `WEBSOCKETS OPENSSL` を uselib に持つ。`HAVE_WEBSOCKETS` ゲートは `libs/surfaces/wscript:63-66` |
| dev-run ラッパ | `gtk2_ardour/ardev` | `gtk2_ardour/ardev_common.sh.in` 経由で `ARDOUR_SURFACES_PATH`・DYLD 設定 |
| サーフェス基底 | `libs/ctrl-interface/control_protocol/control_protocol.{h,cc}` | `ControlProtocol`, `BasicUI`, `Stateful`, `ScopedConnectionList` の多重継承 |
| Session API | `libs/ardour/ardour/session.h` | 2{,}464 行のファサード |
| メータ API | `libs/ardour/ardour/meter.h` | `PeakMeter::meter_level(n, MeterType)`、`MeterPeak=0x0004` (`types.h:250`) |
| イベントループ | `libs/pbd/pbd/event_loop.h` | `call_slot(InvalidationRecord*, std::function<void()>)`、`MISSING_INVALIDATOR` は `nullptr` (`:144`) |
| Undo 基盤 | `libs/pbd/pbd/history_owner.h` | **`_current_trans` は無ガード**（旧来の重要欠陥源、`:93`）|

---

## 11. 次の LLM への引き継ぎ（実用ガイド）

### 11.1 着手の最初の 30 分
1. `git log --oneline feature/mcp-fresh-macos` で 3 commits を確認。
2. §3.2〜3.6 を実行（`./waf configure` と build）。インクリメンタルなら数秒〜数十秒。
3. §3.7〜3.9 で MCP 疎通確認。
4. ここまで通れば「環境再現済み」、Phase 1 タスクを 1 つ選んで着手。

### 11.2 推奨：Phase 2 の SSE / notifications から着手
Phase 1 の機能追加は工数が大きく、価値も漸進的。Phase 2 の**フィードバック / SSE** が最も差別化価値が高い：
- 既存資産：`feedback.cc`（websockets サーフェス）と `osc_route_observer.cc` がテンプレート
- 実装目標：peak meter / playhead position の non-polling push
- 成果：エージェントが「観測 → 編集 → 観測」を低コストで回せる

### 11.3 落とし穴
- **`echo $?` を信用しない**（§3.5）
- **`build/c4che/_cache.py` を直接編集しない**（再 configure で消える）。修正はソース（`wscript` / configure フラグ）に。
- **GTK on macOS の表示**（§6.2-1）：起動はバックグラウンドでなく前景で。
- **`PBD::ID` の fail-open**（§8 Phase 4）：`route_by_mcp_id` 等の MCP 層ヘルパは fail-closed 化済みだが、コアは依然脆い。MCP 外経路（Lua スクリプト、他サーフェス）への波及は別問題。

### 11.4 並行する Audacity トラック
別リポ（`/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity` の `mcp-llm-handoff` ブランチ、`masatomoota/audacity` フォーク）に **Audacity 3.x ベースの MCP 実装ハンドオフ**を別途設置済み。Ardour の知見が大きく転用できる（特にスレッド整流の正しさ、ターン制ロックの設計）。両方 LLM-control を備える時代へ。

---

## 12. 来歴・検証メタデータ

- 解析・実装起点：`b25a63c74a` (`9.7-88-gb25a63c74a`)
- ブランチ：`feature/mcp-fresh-macos`（`0834ec2610` → `36b0f04fb0` → `5129c6d773` → 本ハンドオフ commit）
- ビルド・検証マシン：Mac mini M4（arm64, 10 cores, macOS Apple clang 17, Homebrew 6.0.3）
- 検証手法：static（dylib / nm / JSON 妥当性）＋ live（curl による MCP 疎通、Host header rejection、`track_get_meter` 実呼び出し）。詳細は §6.1。
- 補足設計レポート（リポ外、参考）：作業ホスト `/Volumes/work-ssd-4TB-USB4/_Git_Repository/llm-daw-report/` の PDF 4 部。**本ハンドオフはこれらに依存せず単体で完結**。
- 本ハンドオフはエンジニアリング分析であり、ライセンス §9 は**法的助言ではない**（公開前に弁護士確認推奨）。

---

*End of handoff. 次の LLM へ：§3 でビルド・起動・MCP 疎通を再現確認 → §8 から残作業を選ぶ。Phase 0（堅牢化）は完了し、安全な土台が出来ている。次は知覚ループ（Phase 2 が差別化価値最大）または in-app UX（Phase 3）が次の山。*
