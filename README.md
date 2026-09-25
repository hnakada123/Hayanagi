# Hayanagi

Hayanagi は、C++17 で実装した USI プロトコル対応の最小構成将棋エンジンです。
通常対局用の探索に加えて、詰将棋用の詰み探索（`TsumeSearch`）を備え、
将棋 GUI [ShogiBoardQ](https://github.com/hnakada123/ShogiBoardQ) に静的ライブラリとして組み込まれています。

- 現在のバージョン: **1.0.1**（[変更履歴](#バージョンと変更履歴)）
- USI の `id name` は `Hayanagi 1.0.1`。`./build/hayanagi --version` でも表示できます
- CMake の生成実行ファイル名は `hayanagi`、組み込み用の静的ライブラリは `hayanagi_tsume`
- 合法手生成、終局判定、通常探索、詰み探索、`bench` / `perft` をひととおり実装

## 目次

- [動作要件](#動作要件)
- [ビルド](#ビルド)
- [実行例](#実行例)
- [実装範囲](#実装範囲)
- [実装概要](#実装概要)
- [既知の制約](#既知の制約)
- [詰み探索（詰将棋の攻方・玉方）](#詰み探索詰将棋の攻方玉方)
- [ライブラリとしての組み込み](#ライブラリとしての組み込み)
- [テストとベンチマーク](#テストとベンチマーク)
- [ディレクトリ構成](#ディレクトリ構成)
- [バージョンと変更履歴](#バージョンと変更履歴)

## 動作要件

- CMake 3.16 以上
- C++17 対応コンパイラ
  - GCC / Clang を想定（`__builtin_ctzll` などの組み込み関数を使います）
- Python 3（回帰テストとベンチマークスクリプトのみ。標準ライブラリだけで動きます）

## ビルド

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

生成物は次のとおりです。

| 生成物 | 内容 |
|---|---|
| `build/hayanagi` | USI エンジン実行ファイル |
| `build/libhayanagi_tsume.a` | `Position` と `TsumeSearch` の静的ライブラリ（GUI 組み込み用） |
| `build/hayanagi_tests` | C++ 単体テスト（Hayanagi を最上位でビルドしたときだけ生成） |

コンパイル警告は `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion` を有効にしており、GCC と Clang のどちらでも警告なしでビルドできます（Clang の `-Wconversion` は `-Wsign-conversion` を含むため、GCC と同じ意味になるよう明示的に無効にしています）。

## 実行例

```bash
./build/hayanagi --version   # Hayanagi 1.0.1
./build/hayanagi             # USI エンジンとして起動
```

```text
usi
isready
usinewgame
position startpos
perft depth 2 divide
bench nodes 10000
go nodes 5000
quit
```

`usi` に対しては次のように応答します（`Hayanagi 1.0.1` の出力）。

```text
id name Hayanagi 1.0.1
id author OpenAI
option name USI_Ponder type check default false
option name MultiPV type spin default 1 min 1 max 32
option name Threads type spin default 1 min 1 max 128
option name Hash type spin default 16 min 1 max 65536
option name MinimumThinkingTime type spin default 0 min 0 max 600000
option name NetworkDelay type spin default 0 min 0 max 600000
option name NetworkDelay2 type spin default 0 min 0 max 600000
option name SlowMover type spin default 100 min 1 max 1000
option name ResignValue type spin default 99999 min 0 max 99999
option name MaxMovesToDraw type spin default 500 min 0 max 600000
option name EnteringKingRule type combo default CSARule24 var NoEnteringKing var CSARule24 var CSARule24H var CSARule27 var CSARule27H var TryRule
option name GenerateAllLegalMoves type check default true
option name USI_OwnBook type check default true
option name BookDir type string default book
option name BookFile type combo default standard_book.db var no_book var standard_book.db var yaneura_book1.db var yaneura_book2.db var yaneura_book3.db var yaneura_book4.db var user_book1.db var user_book2.db var user_book3.db
option name TsumeMode type check default false
usiok
```

## 実装範囲

### 合法手生成

- 成り
- 打ち駒
- 二歩
- 行き所のない駒の打ち / 移動の禁止
- 打ち歩詰めの禁止
- 王手回避、両王手、ピンされた駒の移動制限を直接生成
- 王手になる手だけの直接生成（`generate_checking_moves`）

### 終局判定

- 千日手
- 連続王手の千日手
- 持将棋の点数判定
- 手数上限による引き分け判定（`MaxMovesToDraw`）
- 入玉ルール切り替え（`EnteringKingRule`）

### USI / 追加コマンド

- `usi`
- `isready`
- `usinewgame`
- `position startpos moves ...`
- `position sfen ... moves ...`
- `go depth N`
- `go movetime N`
- `go nodes N`
- `go ponder ...`
- `go wtime ... btime ... byoyomi ...`
- `go tsume <attack|defense> depth N movetime M`（詰み探索。[詳細](#詰み探索詰将棋の攻方玉方)）
- `setoption name USI_OwnBook value <bool>`
- `setoption name BookDir value <path>`
- `setoption name BookFile value <file>`（`no_book` / `standard_book.db` / `yaneura_book1.db`〜`yaneura_book4.db` / `user_book1.db`〜`user_book3.db`）
- `setoption name USI_Ponder value <bool>`
- `setoption name MultiPV value N`
- `setoption name Threads value N`
- `setoption name Hash value N`（MB 単位）
- `setoption name MinimumThinkingTime value N`
- `setoption name NetworkDelay value N`
- `setoption name NetworkDelay2 value N`
- `setoption name SlowMover value N`
- `setoption name ResignValue value N`
- `setoption name MaxMovesToDraw value N`
- `setoption name EnteringKingRule value <rule>`（`NoEnteringKing` / `CSARule24` / `CSARule24H` / `CSARule27` / `CSARule27H` / `TryRule`）
- `setoption name GenerateAllLegalMoves value <bool>`
- `setoption name TsumeMode value <bool>`（片玉の局面を受け付ける）
- `stop`
- `ponderhit`
- `quit`
- `bench depth N`
- `bench nodes N`
- `bench current depth N`
- `bench tsume [movetime M]`（詰み探索の固定局面ベンチマーク）
- `perft N`
- `perft depth N divide`

実装済みの主要オプションは次のとおりです。

- `USI_OwnBook`
- `BookDir`
- `BookFile`
- `MultiPV`
- `Threads`
- `Hash`
- `USI_Ponder`
- `MinimumThinkingTime`
- `NetworkDelay`
- `NetworkDelay2`
- `SlowMover`
- `ResignValue`
- `MaxMovesToDraw`
- `EnteringKingRule`
- `GenerateAllLegalMoves`
- `TsumeMode`

`USI_OwnBook` を有効にすると（デフォルトで有効）、`go` 時に定跡ファイルを参照し、現局面にヒットすれば探索せずに定跡手を返します。定跡ファイルはやねうら王の DB2016 フォーマット（`#YANEURAOU-DB2016 1.00`）に対応しています。`BookDir` で定跡フォルダ（デフォルトは実行ファイルからの相対パス `book`）、`BookFile` で定跡ファイル名（デフォルトは `standard_book.db`）を指定します。`BookFile` に `no_book` を指定すると定跡を無効にできます。

`MultiPV` を有効にすると、`info ... multipv N pv ...` を順位ごとに出力します。`GenerateAllLegalMoves` は互換性維持のため残していますが、現在は探索強度を落とさないよう no-op です。

通常探索の `info` には `depth` / `score cp` / `nodes` / `time` / `nps` / `pv` を出力します。

`USI_Ponder` を有効にすると、通常探索の `bestmove` は `bestmove <move> ponder <move>` を返します。`go ponder` で始めた探索は `ponderhit` または `stop` を受けるまで `bestmove` を返しません。

## 実装概要

### `Position`

- 9x9 盤面、持ち駒、手番を保持します。
- `startpos` / `sfen` の読み込み、USI 指し手の適用、合法手生成を担当します。
- 盤面配列に加えて色別・駒種別ビットボードを持ち、王手検出、ピン判定、合法手生成をビットボード主導で行います。
- 玉の回避手、単王手時の合駒・駒取り制限、ピンされた駒の移動制限を、局面全体の後段フィルタではなく直接生成で処理します。
- 飛び駒の利きはレイのビットボードと最初の遮蔽駒から求め、王手判定は玉と同一線上にある飛び駒だけを調べます。
- 王手生成（`generate_checking_moves`）は、移動先に置いた駒が相手玉に利く升と開き王手の候補を先に求め、該当する手だけを列挙します。並び順は全合法手の生成順と同じです。
- `make_move` / `unmake_move` は履歴を更新しない軽量な着手・待ったで、詰み探索のように千日手判定が不要な探索で局面のコピーを省きます。
- `has_legal_move` は手を列挙せずに合法手の有無を判定します（王手回避では玉の移動・王手駒の捕獲・合駒の順に調べます）。

### `TsumeSearch`

- 王手の連続による強制詰みを、深さ優先の反復深化で探索します。詳細は[詰み探索](#詰み探索詰将棋の攻方玉方)を参照してください。

### `Search`

- 反復深化付きの `negamax + alpha-beta` 探索を行います。
- `Threads > 1` のときは root move 単位で並列探索します。
- `Hash` オプションでサイズ変更できる lockless 共有置換表、PVS、quiescence search、SEE ベースの着手順序、killer/history heuristic、LMR、null-move pruning を使って探索効率を上げています。
- 短手数の詰みは専用の王手限定探索で先に検出します。
- 評価関数は駒得に加え、駒の前進度、利きの広さ、敵陣進出、手駒価値、玉の安全度を見ます。

### `Book`

- やねうら王の定跡フォーマット（`#YANEURAOU-DB2016 1.00`）を読み込みます。
- SFEN 文字列をキーとして定跡手を検索します。

### `UsiEngine`

- `usi` / `isready` / `position` / `go` / `stop` / `quit` を処理します。
- 探索はワーカースレッドで実行し、`stop` に反応できる構成にしています。
- `isready` 時に定跡ファイルを読み込み、`go` 時に定跡を参照します。
- `bench` と `perft` を内蔵しており、GUI 接続前の自己確認にも使えます。`bench tsume` は詰み探索の固定局面を順に解き、結果とノード数・時間を出力します（[出力例](#bench-tsume)）。

## 既知の制約

- 探索は最小構成で、評価関数も簡易です。
- USI には引き分けを返す専用の `bestmove` がないため、千日手・持将棋など現在局面でゲーム終了と判定した場合は `info string terminal ...` を出した上で `bestmove resign` を返します。
- `perft` は `nodes` に加えて `captures` / `promotions` / `checks` / `mates` を出力します。
- `bench` は各局面と合計について `nodes` / `time` / `nps` / `hashfull` を出力し、`nodes N` 指定で固定ノード数ベンチとして使えます。
- 詰み探索は深さ優先なので、長手数の詰将棋（おおむね 15 手以上）の解図には向きません。GUI 側では短手数の判定と選別に使い、長手数は外部の詰将棋エンジンに任せる想定です。

## 詰み探索（詰将棋の攻方・玉方）

`src/tsume.h` の `TsumeSearch` は、通常の評価値探索と分けて、王手の連続による強制詰みを探索します。
玉方ではすべての合法応手を調べ、詰む場合は最長抵抗、不詰の場合は逃れる手を返します。
攻方の玉を省略した SFEN も扱えます。

### 探索の仕組み

- 深さ優先の反復深化（攻方手番なら深さ 1, 3, 5, …、玉方手番なら 0, 2, 4, …）で、最初に確定した結果を返します。そのため `mate` の `plies` は最短の詰み手数です。
- 局面は `make_move` / `unmake_move` で進めて戻し、局面のコピーと履歴の確保をしません。
- 置換表（局面キーは盤面・持駒・手番を含む）には詰みと不詰の証明を深さをまたいで保持します。深さ打ち切りは「その深さまで詰みなし」としてだけ再利用し、不詰の証明とは区別します。
- 置換表は 4 エントリのバケットを持つ固定サイズの表で、64 KB から始めて必要に応じて 2 倍に拡張し、最大 32 MB に収まります。
- 同じ `TsumeSearch` を続けて使うと、攻方が同じ間は前回の証明を再利用します（PV の再構成や別詰の数え上げが速くなります）。
- `TsumeSearch` と `Position` はグローバルな可変状態を持たないので、スレッドごとに別インスタンスを持てば並行して使えます。
- 計測結果は [BENCHMARK.md](BENCHMARK.md) を参照してください。

### USI 拡張 `go tsume`

単独実行時は次の独自 USI 拡張を利用できます（通常 USI の `go mate` とは別のコマンドです）。

```text
usi
setoption name TsumeMode value true
setoption name USI_OwnBook value false
isready
position sfen 9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1 moves 3c5c+
go tsume defense depth 4 movetime 5000
```

応答例:

```text
tsume mate move 4d4e plies 4 nodes ...
```

- `attack` は現在手番を攻方、`defense` は現在手番を玉方として探索します。
- `depth` は現在局面からの最大手数（既定 31、上限 63）、`movetime` はミリ秒（既定 5000）です。
- 応答の状態は `mate` / `nomate` / `depthlimit` / `timeout` / `cancelled` です。不正な局面・コマンドでは `tsume invalid` を返します。
- `nomate` は不詰の証明です。`depthlimit` は指定手数以内に詰みがないという意味で、それより長い詰みの否定ではありません。`timeout` / `cancelled` は未判定です。
- `move` は攻方の詰め手、または玉方の抵抗・逃れの手です。着手がない場合は `none` です。`plies` は証明された詰みまでの手数で、`mate` の場合に有効です。
- `stop` で中断結果を返し、`position` / `quit` は進行中の探索を破棄します。
- `TsumeMode` は既定 false です。通常の `position` では両玉が必要で、片玉局面での通常の `go` は拒否します。

### `bench tsume`

`bench tsume [movetime M]` は [BENCHMARK.md](BENCHMARK.md) と同じ 8 局面を順に解き、局面ごとの結果と合計を `info string` で出力します（`TsumeMode` の設定は不要です）。

```text
info string bench tsume 1/8 mate5-1 depth 5 status mate move 3c5c+ plies 5 nodes 646 time 0 nps 646000
...
info string bench tsume 8/8 nomate5 depth 5 status depthlimit move R*7i plies 0 nodes 1870972 time 207 nps 9038512
info string bench tsume total positions 8 nodes 2019874 time 225 nps 8977217
```

## ライブラリとしての組み込み

ShogiBoardQ は Hayanagi をサブモジュールとして取り込み、`add_subdirectory` で静的ライブラリ `hayanagi_tsume` をリンクしています。

```cmake
add_subdirectory(Hayanagi)
target_link_libraries(my_app PRIVATE hayanagi_tsume)   # include パス（src/）も伝播する
message(STATUS "Hayanagi ${HAYANAGI_VERSION}")           # 組み込み側からバージョンを参照できる
```

- `hayanagi_tsume` は `src/position.cpp` と `src/tsume.cpp` だけからなり、Qt などへの依存はありません。
- 組み込み時は単体テスト `hayanagi_tests` を既定でビルドしません。必要なら `-DHAYANAGI_BUILD_TESTS=ON` を指定します。
- 主な API（`src/position.h`、`src/tsume.h`）:
  - `Position::set_sfen(sfen, tsume)`: `tsume=true` で攻方の玉がない局面も受け付け、不正な SFEN では `false` を返します。
  - `Position::generate_legal_moves()`: 打ち歩詰めを除いた全合法手。`generate_checking_moves()`: 合法な王手だけ（打ち駒を含み、成・不成は別の手）。どちらも生成順は安定しており、組み込み側が依存できます。
  - `Position::apply_usi_move` / `do_move` / `make_move` / `unmake_move` / `has_legal_move` / `to_sfen` / `move_to_usi`。
  - `TsumeSearch::solve(position, attacker, max_plies, time_limit_ms, stop)`: `TsumeResult{status, move, plies, nodes}` を返します。`max_plies` は 63 で丸められ、`stop` は他スレッドから立てると速やかに `Cancelled` で戻ります。
- 探索結果の意味を変える改良をしたときは、組み込み側で結果をキャッシュしているキー（ShogiBoardQ では Hayanagi の版を含む）を更新する必要があります。

## テストとベンチマーク

ビルド後、Python 3 の標準ライブラリだけで通常将棋と詰将棋の USI 動作を検証できます。

```bash
python3 tests/test_engine.py build/hayanagi
# 変更前の実行ファイルがあれば、通常将棋 4 局面の perft 結果も比較する
python3 tests/test_engine.py build/hayanagi --baseline /path/to/previous/hayanagi
```

平手初期局面の perft、通常探索の合法着手、5 手詰 5 問、別解、不詰、手数超過、
後手攻方、打ち歩詰め、入力不正、時間切れ、中止・局面切替、`id name` の形式を検証します。

C++ の単体テスト `hayanagi_tests` は、Hayanagi 自体をビルドしたときだけ既定で生成されます
（`add_subdirectory` で組み込んだ場合は `-DHAYANAGI_BUILD_TESTS=ON` で有効化）。

```bash
./build/hayanagi_tests
# または
ctest --test-dir build
```

ランダム局面と平手からのランダム対局について、合法手生成・王手生成・`has_legal_move`・
`make_move` / `unmake_move` を単純な参照実装と突き合わせ、詰み探索については
`Limit` と `NoMate` の区別、最短手数、PV の再構成、`stop` と時間切れ、打ち歩詰め、
玉なし局面、持駒の多い局面、同じ探索器の再利用を検証します。

詰み探索のベンチマークは [BENCHMARK.md](BENCHMARK.md) に記録しています。再計測は次のとおりです。

```bash
python3 tests/bench_tsume.py build/hayanagi [--baseline /path/to/previous/hayanagi]
# USI から直接実行する場合
./build/hayanagi <<< $'bench tsume\nquit'
```

## ディレクトリ構成

| パス | 内容 |
|---|---|
| `src/types.h`, `src/bitboard.h` | 基本型、81 升ビットボード |
| `src/position.h/.cpp` | 局面、SFEN、合法手・王手生成、終局判定 |
| `src/tsume.h/.cpp` | 詰み探索 `TsumeSearch` |
| `src/search.h/.cpp` | 通常対局用の探索と評価 |
| `src/book.h/.cpp` | 定跡の読み込み |
| `src/usi_engine.h/.cpp`, `src/main.cpp` | USI プロトコル処理、`bench` / `perft` |
| `src/version.h` | バージョン定義（`HAYANAGI_VERSION`） |
| `tests/test_engine.py` | USI 回帰テスト |
| `tests/test_*.cpp`, `tests/test_support.h` | C++ 単体テスト |
| `tests/bench_tsume.py` | 詰み探索ベンチマーク |
| `BENCHMARK.md` | ベンチマークの計測結果 |

## バージョンと変更履歴

バージョンは `src/version.h` の `HAYANAGI_VERSION` が唯一の定義元で、CMake の `project(... VERSION)`、
USI の `id name`、`--version` はすべてここから読みます。リリース時はこの値と本節を更新し、
同じ番号のタグ（`v1.0.1` など）を付けます。ShogiBoardQ はタグで指定した版をサブモジュールとして参照します。

| バージョン | タグ | 日付 | 概要 |
|---|---|---|---|
| 1.0.1 | [v1.0.1](https://github.com/hnakada123/Hayanagi/releases/tag/v1.0.1) | 2026-09-25 | Clang での `-Wsign-conversion` 警告を解消（ShogiBoardQ が参照中） |
| 1.0.0 | [v1.0.0](https://github.com/hnakada123/Hayanagi/releases/tag/v1.0.0) | 2026-09-25 | 詰み探索と局面処理の高速化、ベンチマーク、単体テスト、バージョン情報 |

### 1.0.1（2026-09-25）

- Clang では `-Wconversion` が `-Wsign-conversion` を含むため、Qt Creator の Clang コードモデルが Hayanagi のソースに約 490 件の符号変換警告を報告していました（GCC でのビルドは元から警告なしで、ビルド失敗ではありません）。`-Wno-sign-conversion` を併記して GCC と同じ警告範囲にし、Clang 22 でも警告ゼロでビルドとテストが通ることを確認しています。探索や結果の変更はありません。

### 1.0.0（2026-09-25）

最初の明示的なバージョンです。ShogiBoardQ が参照していたコミット `30cfce5` からの主な変更は次のとおりです。

- 詰み探索を高速化しました。局面のコピーをやめて `make_move` / `unmake_move` で探索し、置換表を深さごとの `std::unordered_map` から、詰み・不詰の証明を深さをまたいで再利用する固定サイズの表に置き換えました。5 手詰の局面で約 15〜18 倍、攻方の持駒が多い不詰局面（深さ 5）で約 24 倍速くなっています（[BENCHMARK.md](BENCHMARK.md)）。
- 局面処理を高速化しました。飛び利きのビットボード計算、王手判定の同一線上判定、王手の直接列挙、`has_legal_move` を追加し、`perft` も約 2 割速くなっています。`perft` の結果と通常探索のノード数は変わっていません。
- 玉方の最長抵抗の選択が正確になりました（反復深化の途中結果を深さをまたいで使うため）。同じ探索器を続けて使うと、以前 `depthlimit` だった局面で不詰の証明 `nomate` を返すことがあります。
- 玉同士が隣接する不正局面で相手玉を取る手を生成しなくなりました。
- `bench tsume`、`tests/bench_tsume.py`、C++ 単体テスト `hayanagi_tests` を追加しました。
- `-Wshadow -Wconversion` を有効にしました。
- バージョン情報（`src/version.h`、`id name Hayanagi 1.0.0`、`--version`）を追加しました。
