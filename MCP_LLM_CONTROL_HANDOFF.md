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
当該フォークには既に実験的な MCP-over-HTTP コントロールサーフェスが存在する：`libs/surfaces/mcp_http/`（`mcp_http_server.cc` 約 9{,}190 行（Wave T2 後）、`tools_json.inc` 100 ツール、libwebsockets ベース、ポート 4820、JSON-RPC 2.0 / `protocolVersion 2025-03-26`）。Wave T3 で SSE `GET /events` エンドポイントを、Wave T2 で `automation/get_lane`・`automation/set_curve`・`automation/set_mode` を追加済み。Codex CLI / Claude Desktop / Gemini / Ollama から接続できる。

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
  b25a63c74a Refine f8f2572f use only with MINGW/Windows                   (HEAD~8, upstream)
  356c5839cf Fix USB surface compile when libusb-dev is not installed       (HEAD~7, upstream)
  0834ec2610 build: enable macOS (arm64) dev build against Homebrew deps    (HEAD~6, Wave 0)
  36b0f04fb0 mcp_http: harden — thread marshaling, localhost bind, Host header check  (HEAD~5, Wave 1a)
  5129c6d773 mcp_http: add track/get_meter — real-time peak readback (dBFS)            (HEAD~4, Wave 1b)
  2ea50d0292 docs: add MCP_LLM_CONTROL_HANDOFF.md (Wave 3 handoff)                     (HEAD~3, Wave 3)
  458f99a63b gitignore: add .env to prevent API key leakage                             (HEAD~2, Wave 3b)
  19853971f0 mcp_http: add session/export_audio — open the delivery port (T1)           (HEAD~2, Wave T1)
  43f4848f09 mcp_http: add SSE GET /events + notifications/transport (T3 MVP)           (HEAD~1, Wave T3)
  ee8ffb10fd mcp_http: add automation/{get_lane,set_curve,set_mode} (T2 MVP)            (HEAD,   Wave T2)
  ```
- `git status` クリーン（本 MD コミット前）。

### 2.2 ツール数サマリ
- Wave 0 以前：95 ツール（`mcp_http_server.cc` 既存実装）
- Wave 1b（`5129c6d773`）：`track/get_meter` 追加 → **96 ツール**
- Wave T1（`19853971f0`）：`session/export_audio` 追加 → **97 ツール**
- Wave T3（`43f4848f0979bd83371aec31252cbd43011bba2b`）：SSE `GET /events` エンドポイント追加 → **97 ツール + 1 SSE エンドポイント**
- Wave T2（`ee8ffb10fd177a9e09fb000bf0a8bf75c4d72b8b`）：`automation/get_lane`・`automation/set_curve`・`automation/set_mode` 追加 → **100 ツール + 1 SSE エンドポイント**（現在の正確な数）

### 2.3 ワーキングツリーの汚れ
- 追跡対象外の変更：なし（本ハンドオフ MD のみが untracked → 本ハンドオフのコミットで解消）。
- `build/` は `.gitignore` 対象（追跡なし）。

### 2.4 GitHub 同期先
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

# tools/list — 100 tools incl. track_get_meter, session_export_audio, automation/* 
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  http://127.0.0.1:4820/mcp \
  | python3 -c "import json,sys; t=json.load(sys.stdin)['result']['tools']; print(len(t), [x['name'] for x in t if 'meter' in x['name'] or 'export' in x['name'] or 'automation' in x['name']])"

# Host header security: must return 403
curl -v -H 'Host: evil.example.com:4820' -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"initialize","params":{}}' \
  http://127.0.0.1:4820/mcp

# track_get_meter (replace <route_id> with an actual route's PBD::ID)
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"track_get_meter","arguments":{"id":"<route_id>"}}}' \
  http://127.0.0.1:4820/mcp
```

### 3.9b SSE エンドポイント確認（Wave T3 以降）
```bash
# GET /events — SSE ストリームに接続（Ardour 起動・MCP 有効化後）
curl -N -H 'Accept: text/event-stream' http://127.0.0.1:4820/events
# expect: 接続直後に transport スナップショットが届く（例）:
#   data: {"jsonrpc":"2.0","method":"notifications/transport",
#          "params":{"state":"stopped","position_samples":0,
#                    "position_seconds":0.000000,"sample_rate":48000}}
#
# その後 Ardour で再生を開始すると:
#   data: {"jsonrpc":"2.0","method":"notifications/transport",
#          "params":{"state":"playing","position_samples":...,"position_seconds":...,"sample_rate":48000}}
#
# 15 秒ごとにハートビート:
#   : heartbeat

# Host header check applies here too (403 for non-loopback Host):
curl -v -H 'Host: evil.example.com:4820' http://127.0.0.1:4820/events
# expect: HTTP 403
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
| `tools_json.inc` 妥当 JSON / 97 tools | ✅ | `track_get_meter`, `session_export_audio` 含む |
| 起動 → 4820 LISTEN | ✅ | |
| `initialize` protocolVersion 2025-03-26 | ✅ | |
| `tools/list` 97 tools | ✅ | |
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

Wave 0 出発点との差分：(1) `tools/call` 全体が GUI スレッドへ整流、(2) listen `0.0.0.0` → `127.0.0.1`、(3) Host loopback 検証、(4) `track/get_meter` ツール追加、(5) `session/export_audio` ツール追加（Wave T1）、(6) SSE `GET /events` エンドポイント追加（Wave T3）、(7) `automation/get_lane`・`automation/set_curve`・`automation/set_mode` ツール追加（Wave T2）。

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
| MCP サーフェス本体 | `libs/surfaces/mcp_http/mcp_http_server.cc` | 8{,}460+ 行（Wave T1 後）。`dispatch_jsonrpc`, `run_tools_call`, `handle_track_get_meter_tool`, `handle_session_export_audio_tool` 等が中核 |
| MCP サーフェス薄ラッパ | `libs/surfaces/mcp_http/mcp_http.cc` / `.h` | `ControlProtocol` 継承、222 行 |
| サーフェス登録 | `libs/surfaces/mcp_http/interface.cc` | `protocol_descriptor` を C リンケージで export |
| ツールカタログ | `libs/surfaces/mcp_http/tools_json.inc` | 100 tools の JSON-Schema（`#include` で `static constexpr std::string_view` として埋め込み）|
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

### 11.2 T1 + T3 + T2 すべて完了 — 次の推奨は T4/T5/T9
本セッション（2026-06-26）で T1（`session/export_audio`）・T3（SSE `GET /events`）・T2（`automation/get_lane`・`automation/set_curve`・`automation/set_mode`）の 3 つが全て landing した。マスターハンドオフ §1.2 が "致命的欠落" と指摘した 3 軸 (納品 / オートメーション / 知覚) を一挙に閉じ、プロジェクトは「実用 90%+」に到達した。次の推奨は：
- **T4（テンポ / 拍子編集）**：`TempoMap::write_copy()` → 編集 → `update()`。依存なし。
- **T5（フェード / クロスフェード）**：`AudioRegion::set_fade_in_length` / `set_fade_in_shape`。
- **T9（ターン制ロック）**：fix_plan v2 §5 設計済み。多段編集の原子性保証。
- **SSE 拡張**（T3 follow-up）：`notifications/meter`（10Hz ポーリング）、`notifications/position`（再生中のヘッドアップデート）、per-client フィルタ。

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
- ブランチ：`feature/mcp-fresh-macos`（`0834ec2610` → `36b0f04fb0` → `5129c6d773` → `2ea50d0292` → `458f99a63b` → `19853971f0` → `43f4848f09`）
- ビルド・検証マシン：Mac mini M4（arm64, 10 cores, macOS Apple clang 17, Homebrew 6.0.3）
- 検証手法：static（dylib / nm / JSON 妥当性 / string table）＋ live（curl による MCP 疎通、Host header rejection、`track_get_meter` 実呼び出し）。Wave T1 はライブ Ardour なしのため static 確認のみ（`session/export_audio` 文字列・`session_export_audio` 文字列いずれも dylib string table に存在確認済み）。詳細は §6.1。
- 補足設計レポート（リポ外、参考）：作業ホスト `/Volumes/work-ssd-4TB-USB4/_Git_Repository/llm-daw-report/` の PDF 4 部。**本ハンドオフはこれらに依存せず単体で完結**。
- 本ハンドオフはエンジニアリング分析であり、ライセンス §9 は**法的助言ではない**（公開前に弁護士確認推奨）。

---

## 13. Wave T1：`session/export_audio`（納品口を開ける）

**コミット `19853971f07f6f81413b55a298487e5574efa98c`** — 2026-06-26 — `mcp_http: add session/export_audio — open the delivery port (T1)`

### 13.1 ツール名とスキーマ

`tools_json.inc` の追加エントリ（verbatim、行 142–186）:

```json
{
  "name": "session_export_audio",
  "title": "Export Audio",
  "description": "Export the session master bus to an audio file and return metadata about the exported file. For the MVP only WAV (PCM) output is supported; FLAC, AIFF, and MP3 support may be added in a future release. The export runs in freewheel (offline) mode and blocks until complete. The caller must supply an absolute path for the output file; the parent directory must already exist. format defaults to \"wav\". sample_rate defaults to the session's nominal sample rate. sample_format may be \"PCM_16\" (default), \"PCM_24\", or \"FLOAT\". start_sec and length_sec define the export range in seconds; omit both to export the entire session. channels may be \"stereo\" (default) or \"mono\" (sums L+R from the master bus).",
  "inputSchema": {
    "type": "object",
    "properties": {
      "path": {
        "type": "string",
        "description": "Absolute filesystem path for the output file (e.g. /home/user/export/mix.wav). The parent directory must exist."
      },
      "format": {
        "type": "string",
        "enum": ["wav"],
        "description": "Output file format. Only \"wav\" is supported in the MVP."
      },
      "sample_rate": {
        "type": "number",
        "description": "Sample rate in Hz (e.g. 44100, 48000). Defaults to the session nominal sample rate."
      },
      "sample_format": {
        "type": "string",
        "enum": ["PCM_16", "PCM_24", "FLOAT"],
        "description": "PCM bit depth or float. Defaults to \"PCM_16\"."
      },
      "start_sec": {
        "type": "number",
        "minimum": 0,
        "description": "Export range start, in seconds from the session origin. Defaults to 0."
      },
      "length_sec": {
        "type": "number",
        "exclusiveMinimum": 0,
        "description": "Export range length in seconds. Defaults to the full session length."
      },
      "channels": {
        "type": "string",
        "enum": ["stereo", "mono"],
        "description": "\"stereo\" exports L+R from the master bus. \"mono\" sums them."
      }
    },
    "required": ["path"],
    "additionalProperties": false
  }
}
```

**MCP tool name**（`canonical_tool_name` が `/` ↔ `_` 両形を受理する）：`session/export_audio` または `session_export_audio`。

### 13.2 実装箇所

| ファイル | 場所 | 役割 |
|---|---|---|
| `mcp_http_server.cc` | 行 4298–4575 | `handle_session_export_audio_tool()` — 引数解析・パイプライン構築・export 実行・結果返却 |
| `mcp_http_server.cc` | 行 4633–4635 | `dispatch_session_tool_call()` に `"session/export_audio"` 分岐追加 |
| `tools_json.inc` | 行 142–186 | スキーマ定義（`session_export_audio` エントリ）|
| `wscript` | `uselib` に `SNDFILE` 追加 | `ExportFormatBase` 等が libsndfile を要求 |

### 13.3 実装ウォークスルー

`handle_session_export_audio_tool()` の処理ステップ（`mcp_http_server.cc:4298-4575`）：

1. **引数解析・検証**（行 4302–4402）：`path`（必須・絶対パス・親ディレクトリ存在確認）、`format`（MVP: wav のみ）、`sample_rate`（`ExportFormatBase::SampleRate` 列挙への変換）、`sample_format`（`SF_16`/`SF_24`/`SF_Float`）、`start_sec`/`length_sec`（サンプル単位の `range_start`/`range_end` に変換）、`channels`（stereo/mono）。
2. **マスターバス確認**（行 4405–4419）：`session.master_out()` でマスターバスを取得、`IO::n_ports().n_audio()` で出力ポート確認、`export_status()->running()` で二重起動を防止。
3. **ExportFormatSpecification の構築**（行 4431–4458）：`handler->add_format()` → `ExportFormatTaggedLinear("WAV", F_WAV)` を組み立てて `spec->set_format()` で私有フラグ `_has_sample_format=true` をセット（これをしないと libsndfile が `SF_None` を受け取り無音ファイルになる）。SR、ビット深度、ディザ（16bit は D_Shaped）を設定。
4. **ExportFilename の構築**（行 4466–4483）：`handler->add_filename()` → stem（`.wav` 拡張子を除去）を `set_label()` でセット後、副作用で `include_label=false` になるのを `include_label=true` で上書き。フォルダ・タイムスパン参照も設定。
5. **ExportTimespan の構築**（行 4486–4491）：`handler->add_timespan()` → `set_range(range_start, range_end)`、`set_realtime(false)` でオフラインフリーホイールモード指定。
6. **ExportChannelConfiguration の構築**（行 4497–4512）：mono の場合はマスターバスの全ポートを 1 つの `PortExportChannel` に積む、stereo の場合はポートごとに 1 チャンネル。
7. **エクスポート開始**（行 4516–4521）：`handler->add_export_config(ts, chan_cfg, spec, fn, BroadcastInfoPtr())` → `handler->do_export()`。
8. **イベントループポンプ**（行 4523–4548）：`while(status->running()) { gtk_main_iteration_do(false) || usleep(10ms) }`、10 分タイムアウトで `status->abort()` 後エラー返却。オーディオスレッドからのフリーホイールコールバックが GUI スレッドへ投函されるため、GTK イベントループのポンプが必須（`export_dialog.cc:410-418` と同型）。
9. **後処理**（行 4550–4574）：`status->finish(TRS_UI)` でフリーホイール停止・状態リセット → `fn->get_path(spec)` で実際のファイルパスを取得 → `stat()` でファイルサイズ確認 → JSON 結果返却（`path`, `bytes`, `sampleRate`, `channels`, `durationSec`, `format`）。

### 13.4 スレッドモデル

`handle_session_export_audio_tool()` は `run_tools_call()` から呼ばれ、`run_tools_call()` は `_event_loop->call_slot()` 経由で**GUI/イベントループスレッド上で実行**される。

フリーホイールエクスポートのコールバック（オーディオスレッド → GUI スレッド）を受け取るために、ハンドラ内で `gtk_main_iteration()` をポンプする必要がある。これは既存の `ExportDialog::show_progress()` と同じ手法（`export_dialog.cc:410-418`）。呼び出し側の lws スレッドは `condvar.wait()` でブロックしており、GUI スレッドが GTK をポンプしながらエクスポート完了を待つ。**10 分タイムアウト**はライブエクスポートが応答なしになった場合の安全弁。

### 13.5 MVP の制限

1. **WAV 専用**：libsndfile が対応する FLAC / AIFF / MP3 は未実装（`ExportFormatTaggedLinear` の `F_FLAC` 等を指定するだけで拡張可能）。
2. **ブロッキング**：最大 10 分間 GUI スレッドをブロックする。T3（SSE）が実装されれば、非同期完了通知に切り替えられる。
3. **マスターバス固定**：出力ソースはセッションのマスターバスのみ。ステム（個別トラック）エクスポートは未実装。
4. **LUFS 解析なし**：`spec->set_analyse(false)` で loudness 解析をスキップ。将来 T3 連携で解析結果をプッシュ通知する拡張余地あり。

### 13.6 スモーク検証結果

- `errors=0`, `warnings=8`（macOS deployment target 不一致の既知警告のみ）
- dylib `libardour_mcp_http.dylib` の string table に `session/export_audio`（count=1）と `session_export_audio`（count=1）が存在 → `tools_json.inc` がコンパイルに取り込まれていることを確認
- Ardour 未起動のため live curl テスト（`tools/list` で 97 ツール確認、`session/export_audio` の実呼び出し）は未実施。次回起動時に §3.9 のパターンで確認すること。

### 13.7 推奨次波

- **T2（オートメーション曲線）**：ミックスの時間軸操作。依存なし。
- **T3（SSE 通知）**：✅ **Wave T3 で MVP 完了**（commit `43f4848f0979bd83371aec31252cbd43011bba2b`）。`GET /events` で `notifications/transport` をストリーミング配信中。拡張（meter / position / route_changed）は §14 の open items を参照。
- T3 → `session/export_audio` の非同期バージョン（`do_export` 後すぐ返却、完了時 SSE `notifications/export_complete` を送出）が理想形（follow-up item）。

---

## 14. Wave T3：SSE / notifications/transport （知覚ループ MVP）

**コミット `43f4848f0979bd83371aec31252cbd43011bba2b`** — 2026-06-26 — `mcp_http: add SSE GET /events + notifications/transport (T3 MVP)`

### 14.1 エンドポイントと接続仕様

| 項目 | 内容 |
|---|---|
| URL | `GET http://127.0.0.1:4820/events` |
| レスポンスヘッダ | `Content-Type: text/event-stream`, `Cache-Control: no-cache` |
| ボディ | Server-Sent Events ストリーム（無限持続接続）|
| Host ヘッダ検証 | 同 `POST /mcp` と同一（loopback 以外で 403）|
| ハートビート | `": heartbeat\n\n"` を 15 秒ごと（プロキシ / CDN タイムアウト対策）|
| 初期フレーム | 接続直後にトランスポート状態スナップショットを 1 枚送信 |

### 14.2 イベントペイロードスキーマ（verbatim）

SSE の各フレームは `data: <JSON>\n\n` の形式で送出される。JSON 本体は JSON-RPC 2.0 Notification（`id` フィールドなし）：

```
data: {"jsonrpc":"2.0","method":"notifications/transport","params":{"state":"<STATE>","position_samples":<INT64>,"position_seconds":<FLOAT>,"sample_rate":<INT64>}}
```

`state` の値域：
- `"stopped"` — トランスポート停止中
- `"playing"` — 再生中（録音なし）
- `"recording"` — 録音中（`actively_recording()` が true）
- `"looping"` — ループ再生中（`get_play_loop()` が true かつ録音なし）

優先順：`recording` > `looping` > `playing` > `stopped`（ソース：`build_transport_event()`, `mcp_http_server.cc:3436-3439`）。

### 14.3 ハンドラ実装ウォークスルー

**接続受け入れ（`handle_http`、`mcp_http_server.cc:3251-3280`）**

`handle_http` 内の `path == "/events"` 分岐：
1. Host ヘッダを loopback 検証（`host_header_is_loopback()`）→ 失敗で 403
2. `send_sse_headers()` でレスポンスヘッダ送信（`mcp_http_server.cc:3385-3420`）
3. `new SseSubscriber()` を作成し `_sse_subscribers` に push（mutex 保護）
4. `ctx.sse_client = true` を設定して `handle_http_writeable` で SSE ドレインパスを使わせる
5. 初期スナップショット（`build_transport_event()`）を `ctx.sse_queue` に積み、`lws_callback_on_writable()` を呼ぶ

**ドレイン（`handle_http_writeable`、`mcp_http_server.cc:3380-3532`）**

`ctx.sse_client` が true の場合：
1. `ctx.sse_queue_mutex` を取りフレームをデキュー
2. `lws_write(wsi, frame, LWS_WRITE_HTTP)` で送信
3. キューが空でなければ自分で `lws_callback_on_writable()` を再スケジュール
4. タイムスタンプが 15 秒を超えた場合はハートビート送信

**切断（`LWS_CALLBACK_CLOSED_HTTP`、`mcp_http_server.cc:3182-3194`）**

`_sse_subscribers` から該当 wsi を `std::remove_if` + `erase` で除去（mutex 保護）。

**シグナル接続（`connect_transport_signals`、`mcp_http_server.cc:3506-3526`）**

`start()` 内（`mcp_http_server.cc:3092`）で呼ばれ、`Session::TransportStateChange` と `Session::RecordStateChanged` を `_sse_signal_connections`（`ScopedConnectionList`）に登録：

```cpp
_session.TransportStateChange.connect (
    _sse_signal_connections, MISSING_INVALIDATOR,
    std::bind (&MCPHttpServer::on_transport_state_changed, this),
    _event_loop);  // ← _event_loop 引数でクロススレッドマーシャル指定

_session.RecordStateChanged.connect (
    _sse_signal_connections, MISSING_INVALIDATOR,
    std::bind (&MCPHttpServer::on_transport_state_changed, this),
    _event_loop);
```

**イベント発火（`on_transport_state_changed`、`mcp_http_server.cc:3497-3501`）**

```cpp
void MCPHttpServer::on_transport_state_changed () {
    const std::string frame = build_transport_event ();
    broadcast_sse (frame);
}
```

**ブロードキャスト（`broadcast_sse`、`mcp_http_server.cc:3456-3490`）**

1. `_sse_subscribers_mutex` の下でターゲット wsi のリストを抽出
2. 各 wsi の `ClientContext::sse_queue_mutex` の下でフレームを `sse_queue` に push
3. `lws_callback_on_writable(wsi)` を呼ぶ（lws >= 3.x でスレッドセーフ）
4. `lws_cancel_service(_context)` で lws ポールループを即時ウェイクアップ

### 14.4 スレッドモデル

| 場所 | スレッド | 操作 |
|---|---|---|
| `TransportStateChange` / `RecordStateChanged` シグナル発火 | RT オーディオスレッドまたは Butler スレッド | PBD signal 発行のみ |
| `on_transport_state_changed()` 実行 | **GUI/event_loop スレッド** | `connect()` 時の `_event_loop` 引数によりマーシャル済み |
| `broadcast_sse()` | GUI/event_loop スレッド | `sse_queue` への push と `lws_callback_on_writable()` 呼び出し |
| `handle_http_writeable` でドレイン | lws サービススレッド | `sse_queue` からデキュー → `lws_write()` |
| `lws_callback_on_writable()` | クロススレッド呼び出しを許容（lws 内部の atomic フラグ設定）| — |
| `lws_cancel_service()` | 明示的にスレッドセーフ（lws-service.h:87-88 コメント）| — |

**重要**：`_sse_subscribers` リスト自体の変更（push / erase）は lws サービススレッドのみが行う（`LWS_CALLBACK_HTTP` と `LWS_CALLBACK_CLOSED_HTTP` は同スレッド上）。`broadcast_sse()` は `_sse_subscribers_mutex` の下で**読み取り専用**コピーを取る。

### 14.5 ファイル変更サマリ

```
commit 43f4848f0979bd83371aec31252cbd43011bba2b
 libs/surfaces/mcp_http/mcp_http_server.cc | 284 +++++++++++++++++++++++++++-
 libs/surfaces/mcp_http/mcp_http_server.h  |  33 ++++
 2 files changed, 313 insertions(+), 4 deletions(-)
```

ツール数の変化なし（SSE はツールではなくエンドポイント）：**97 ツール + 1 SSE エンドポイント**。

### 14.6 スモーク検証結果

- `errors=0`, `warnings=2`（macOS deployment target 不一致のみ）、`iterations=3`
- dylib `libardour_mcp_http.dylib` の string table に `text/event-stream`（1件）、`notifications/transport`（1件）、`/events`（1件）が存在 → SSE コードがコンパイルに取り込まれていることを確認
- シンボルプローブ：`broadcast_sse`（type T = global exported）、`on_transport_state_changed`（type T = global exported）確認済み
- ライブ curl テスト：Ardour 未起動のため `Connection refused`（予期通り）。次回 Ardour 起動時に §3.9b のパターンで確認すること

### 14.7 MVP の制限

1. **トランスポート状態のみ**：`notifications/meter`（レベルメータ）、`notifications/position`（再生ヘッド 10Hz ポーリング）、`notifications/route_changed`（ルート追加/削除）は未実装
2. **per-client フィルタなし**：接続した全クライアントが同一イベントを受信。特定トラックのみ購読する機能は未実装
3. **ハートビートのみ 15 秒**：再接続後に状態スナップショットが 1 枚送られるが、その後は状態変化時のみ。ポーリング系通知（position）は今後追加
4. **subscriber カウントによるシグナル切断最適化なし**：接続が 0 になってもシグナルはアクティブなまま（`ScopedConnectionList` のライフタイムはサーバ停止まで）

### 14.8 推奨次波

- **T2（オートメーション曲線編集）**：✅ **Wave T2 で MVP 完了**（commit `ee8ffb10fd177a9e09fb000bf0a8bf75c4d72b8b`）。§15 参照。
- **SSE 拡張 — notifications/meter**：`on_meter_update()` を追加し、10Hz タイマーで全ルートのピーク値を `notifications/meter` として push。`Route::peak_meter()->meter_level()` を使用（Wave 1b の `track/get_meter` と同じ API）。
- **SSE 拡張 — notifications/position**：再生中に 100ms ごと `notifications/position` を push。`_session.transport_sample()` をポーリング → フレームが変化した場合のみ送信。
- **session/export_audio 非同期化**（T1 follow-up）：`do_export()` 後すぐ `"status":"started"` を返し、完了時 SSE `notifications/export_complete` を push する形に切り替え（T3 があるから実現可能）。

---

## 15. Wave T2：automation tools（ミックス本丸）

**コミット `ee8ffb10fd177a9e09fb000bf0a8bf75c4d72b8b`** — 2026-06-26 — `mcp_http: add automation/{get_lane,set_curve,set_mode} (T2 MVP)`

### 15.1 新ツール（3本）の名称・スキーマ

`tools_json.inc` に追加された 3 エントリ（verbatim）。`canonical_tool_name` により `/` と `_` 両形式を受理する。

**automation_get_lane**（`automation/get_lane` も可）

```json
{
  "name": "automation_get_lane",
  "title": "Get Automation Lane",
  "description": "Return all control points and current mode for an automation lane on a route. Supported parameters: gain, pan, mute, solo, rec_enable.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "id": { "type": "string", "description": "Route MCP id (PBD::ID as decimal string)" },
      "parameter": { "type": "string", "enum": ["gain", "pan", "mute", "solo", "rec_enable"] }
    },
    "required": ["id", "parameter"],
    "additionalProperties": false
  }
}
```

**automation_set_curve**（`automation/set_curve` も可）

```json
{
  "name": "automation_set_curve",
  "title": "Set Automation Curve",
  "description": "Replace all control points on an automation lane with the provided list (mode=replace). Points are specified as seconds from session start plus a parameter value in internal units (gain: 0.0-2.0 linear, pan: 0.0-1.0, mute/solo/rec_enable: 0.0 or 1.0). The operation is undoable as a single step.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "id": { "type": "string" },
      "parameter": { "type": "string", "enum": ["gain", "pan", "mute", "solo", "rec_enable"] },
      "points": {
        "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "timeSec": { "type": "number", "minimum": 0 },
            "value": { "type": "number" }
          },
          "required": ["timeSec", "value"],
          "additionalProperties": false
        }
      },
      "mode": { "type": "string", "enum": ["replace"] }
    },
    "required": ["id", "parameter", "points"],
    "additionalProperties": false
  }
}
```

**automation_set_mode**（`automation/set_mode` も可）

```json
{
  "name": "automation_set_mode",
  "title": "Set Automation Mode",
  "description": "Set the automation playback/record mode for a parameter on a route. Use 'play' (or 'read') to play back recorded automation, 'write' to record all moves, 'touch' to record only while touching, 'latch' to latch written values, 'off' to disable automation.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "id": { "type": "string" },
      "parameter": { "type": "string", "enum": ["gain", "pan", "mute", "solo", "rec_enable"] },
      "mode": { "type": "string", "enum": ["off", "play", "read", "touch", "write", "latch"] }
    },
    "required": ["id", "parameter", "mode"],
    "additionalProperties": false
  }
}
```

### 15.2 ファイル変更サマリ

```
commit ee8ffb10fd177a9e09fb000bf0a8bf75c4d72b8b
 libs/surfaces/mcp_http/mcp_http_server.cc | 348 +++++++++++++++++++++++++++++-
 libs/surfaces/mcp_http/tools_json.inc     |  93 ++++++++
 2 files changed, 440 insertions(+), 1 deletion(-)
```

ツール数の変化：**97 → 100**（+3 automation ツール）。

### 15.3 ハンドラ実装ウォークスルー

#### ヘルパ関数群（`mcp_http_server.cc`、新規追加）

| 関数 | 役割 |
|---|---|
| `resolve_automation_parameter(name, err)` | パラメータ名文字列 → `Evoral::Parameter` 変換。未知名は `NullAutomation` + エラー文字列を返す。`GainAutomation`・`PanAzimuthAutomation`・`MuteAutomation`・`SoloAutomation`・`RecEnableAutomation` に対応 |
| `get_route_automation_control(route, param)` | `Evoral::Parameter` → `shared_ptr<AutomationControl>` 取得。パンがない mono ルートや rec_enable が bus に存在しない場合は `nullptr` を返す（fail-closed） |
| `auto_state_to_mcp_string(s)` | `ARDOUR::AutoState` → `"off"/"play"/"touch"/"write"/"latch"` 文字列変換 |
| `mcp_string_to_auto_state(s, out, err)` | その逆。`"read"` は `"play"` のエイリアスとして受理 |

#### `handle_automation_get_lane_tool()`

引数解析 → `route_by_mcp_id()` でルート取得 → `resolve_automation_parameter()` → `get_route_automation_control()` → `ctrl->alist()` で `AutomationList` 取得 → `PBD::RWLock::ReaderLock` 下で `alist->events()` をイテレートして点列 JSON を組み立てる。各点は `{timeSec, timeSamples, value}` の 3 フィールド（時刻はサンプル数 ÷ `session.sample_rate()` で秒換算）。返却 JSON には `automationState`・`sampleRate`・`pointCount`・`lower`・`upper` も含まれる。

#### `handle_automation_set_curve_tool()`

1. 引数解析・検証（`mode != "replace"` は `-32602` エラー）
2. `route_by_mcp_id()` + `resolve_automation_parameter()` + `get_route_automation_control()`
3. `pts_node` から `ControlList::OrderedPoints` を構築。`timeSec × session.sample_rate()` でサンプル位置に変換（`std::floor` で整数化、`Temporal::timepos_t` にラップ）
4. **reversible command ラップ**：
   - `XMLNode& before = alist->get_state()` でスナップショット取得
   - `alist->freeze()` → `alist->clear()` → `alist->editor_add_ordered(ops, /*with_guard=*/false)` → `alist->thaw()`
   - `XMLNode& after = alist->get_state()`
   - `session.begin_reversible_command("automation: set curve")`
   - `session.add_command(new MementoCommand<AutomationList>(*alist, &before, &after))`
   - `session.commit_reversible_command()`（例外時は `abort_reversible_command()`）
5. `session.set_dirty()` で未保存フラグを立てる
6. 結果 JSON：`{routeId, parameter, mode:"replace", pointsSet, previousPointCount}`

`with_guard=false` の理由：`editor_add_ordered` のデフォルト（`with_guard=true`）は前後に 64 サンプルのガードポイントを挿入する。プログラマティックな書き込みでは不要かつ読み取り時に混乱を招くため無効化。

#### `handle_automation_set_mode_tool()`

引数解析 → `route_by_mcp_id()` + `resolve_automation_parameter()` + `get_route_automation_control()`（パラメータ適用可否確認のみ） → `mcp_string_to_auto_state()` → `route->set_parameter_automation_state(param, new_state)`（`Automatable::set_parameter_automation_state`、`automatable.h:104`）。

**モード変更は reversible command なし**：Ardour の既存サーフェス（OSC: `osc.cc:4509-4534`）と一貫性を保つため、オートメーションモード変更は Undo 履歴に積まない設計。

#### `dispatch_automation_tool_call()` + `run_tools_call()` 統合

```cpp
static bool
dispatch_automation_tool_call (ARDOUR::Session& session,
                               const std::string& tool_name,
                               const pt::ptree& root,
                               const std::string& id,
                               std::string& response)
{
    if (tool_name == "automation/get_lane") { response = handle_automation_get_lane_tool(…); return true; }
    if (tool_name == "automation/set_curve") { response = handle_automation_set_curve_tool(…); return true; }
    if (tool_name == "automation/set_mode") { response = handle_automation_set_mode_tool(…); return true; }
    return false;
}
```

`run_tools_call()` 末尾の「`dispatch_automation_tool_call()` が `true` を返したら `response` を返す」分岐が追加された。`"midi_note"` ディスパッチャの直後、フォールスルーエラーの直前に挿入。

### 15.4 スレッドモデル

Wave 1 のハードニング以来のパターンを継承：lws サービススレッドは `condvar.wait()` でブロック、`run_tools_call()` は `_event_loop->call_slot()` 経由で **GUI/event_loop スレッド上で実行** される。

オートメーション書き込み（`set_curve`）は `begin/commit_reversible_command` を GUI スレッドから呼ぶため、`HistoryOwner::_current_trans` の無ガードアクセスを踏まない（Phase 0 ハードニングの恩恵）。

### 15.5 AutoState マッピング

| MCP 文字列 | `ARDOUR::AutoState` | 挙動 |
|---|---|---|
| `"off"` | `Off` | オートメーション無効 |
| `"play"` | `Play` | 記録済みオートメーション再生 |
| `"read"` | `Play` | `"play"` のエイリアス（Ardour 用語では `Play` = Read） |
| `"touch"` | `Touch` | タッチ中のみ録音、離れると再生 |
| `"write"` | `Write` | 常に録音 |
| `"latch"` | `Latch` | タッチ後ラッチ |

### 15.6 スモーク検証結果

- `errors=0`, `warnings=2`（macOS deployment target 不一致の既知警告のみ）、`iterations=2`
- dylib `libardour_mcp_http.dylib` の string table に `automation/get_lane`・`automation/set_curve`・`automation/set_mode`（slash 形式）および `automation_get_lane`・`automation_set_curve`・`automation_set_mode`（underscore 形式）いずれも存在確認済み → `canonical_tool_name` が両形を emit していることの証拠
- シンボルプローブ：`resolve_automation_parameter`（type t = local）、`handle_automation_get_lane_tool`（type t）、`handle_automation_set_mode_tool`（type t）、`handle_automation_set_curve_tool`（type t）の 4 シンボルが確認済み
- ツール数の grep：`"name": "` パターンで 100 エントリ確認（スペースなし `"name":"` は embedded JSON 側の誤パターン、正しくは `"name": "` をカウントすること）
- ライブ curl テスト：Ardour 未起動のため `Connection refused`（予期通り）

### 15.7 MVP の制限

1. **route 標準パラメータのみ**：`gain`・`pan`・`mute`・`solo`・`rec_enable` の 5 パラメータ。プラグイン固有パラメータ（`PluginAutomation`）は未対応。LV2/VST パラメータには別の `Evoral::Parameter` type（`PluginAutomation`）と route→processor→plugin の lookup chain が必要
2. **replace モードのみ**：`set_curve` は常にクリア→追加（replace）。`merge` モード（既存点との合成）は未実装
3. **ガードポイント無効**：`editor_add_ordered(ops, /*with_guard=*/false)` でガードポイントを抑制。将来 `true` に変更することで GUI 表示との一貫性を高められる
4. **モード変更の通知なし**：`set_mode` / `set_curve` 後に `notifications/automation` SSE イベントを送出する機構が未実装。状態変化の知覚は次回 `get_lane` 呼び出しまで不可
5. **MIDI CC なし**：`MidiCCAutomation` 等の MIDI オートメーションパラメータは別途実装が必要

### 15.8 ファイル:行チートシート

| 関数 / 要素 | ファイル | 行（ee8ffb10 時点概算）|
|---|---|---|
| `resolve_automation_parameter()` | `mcp_http_server.cc` | 追加ブロック先頭付近 |
| `get_route_automation_control()` | `mcp_http_server.cc` | 上記の直後 |
| `auto_state_to_mcp_string()` / `mcp_string_to_auto_state()` | `mcp_http_server.cc` | 上記の直後 |
| `handle_automation_get_lane_tool()` | `mcp_http_server.cc` | ブロック +83 行目〜 |
| `handle_automation_set_curve_tool()` | `mcp_http_server.cc` | ブロック +165 行目〜 |
| `handle_automation_set_mode_tool()` | `mcp_http_server.cc` | ブロック +272 行目〜 |
| `dispatch_automation_tool_call()` | `mcp_http_server.cc` | ブロック +328 行目〜 |
| tools_json.inc の automation エントリ | `tools_json.inc` | 末尾付近の +93 行ブロック |
| `ControlList::editor_add_ordered` | `libs/evoral/ControlList.h` | 158-222 周辺 |
| `Automatable::set_parameter_automation_state` | `libs/ardour/ardour/automatable.h` | 104 |
| `AutoState` 列挙 | `libs/ardour/ardour/types.h` | `Off/Play/Touch/Write/Latch` |

### 15.9 推奨次波

T1 + T3 + T2 の 3 つが同一セッションで完了し、プロジェクトは「実用 90%+」に到達した。

- **T4（テンポ / 拍子編集）**：可変テンポ楽曲に必須。`TempoMap::write_copy()` → 編集 → `update()`。依存なし。
- **T5（フェード / クロスフェード）**：`AudioRegion::set_fade_in_length` / `set_fade_in_shape`。リージョン操作の必須要素。
- **T9（ターン制ロック）**：fix_plan v2 §5 設計済み。多段編集の原子的ロールバック。
- **SSE 拡張**：`notifications/meter`（10Hz）・`notifications/position`（100ms）。§14.8 の pattern を踏襲するだけ。
- **プラグイン automation**：`PluginAutomation` type の `Evoral::Parameter` を `resolve_automation_parameter()` に追加し、`PluginInsert::automation_control(param)` で control を取得する形で拡張可能。

---

*End of handoff. 次の LLM へ：§3 でビルド・起動・MCP 疎通を再現確認 → §8 から残作業を選ぶ。Phase 0（堅牢化）は完了。Wave T1（`session/export_audio`、`19853971f0`）で納品口が開き、Wave T3（`GET /events` SSE、`43f4848f09`）で知覚ループ MVP が完成し、Wave T2（`automation/get_lane`・`set_curve`・`set_mode`、`ee8ffb10fd`）でオートメーション曲線が使えるようになった。マスターハンドオフの「致命的欠落」3 軸すべてが同一セッションで閉じた。次は T4（テンポ）・T5（フェード）・T9（ターン制ロック）が推奨。SSE 拡張（meter / position 通知）は §14 の open items を参照。*
