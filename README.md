# Hayanagi

Hayanagi は、C++17 で実装した USI プロトコル対応の最小構成将棋エンジンです。

- USI の `id name` は `Hayanagi`
- CMake の生成実行ファイル名は `hayanagi`
- 合法手生成、終局判定、基本的な探索、`bench` / `perft` をひととおり実装

GitHub 上で読みやすいように、ビルド手順と実装範囲を先にまとめています。内部構成の詳細は後半に記載しています。

## 動作要件

- CMake 3.16 以上
- C++17 対応コンパイラ
  - GCC / Clang / MSVC を想定

## ビルド

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

生成物は `build/hayanagi` に出力されます。

## 実行例

```bash
./build/hayanagi
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

`usi` に対しては少なくとも次のように応答します。

```text
id name Hayanagi
id author OpenAI
option name MultiPV type spin default 1 min 1 max 32
option name Threads type spin default 1 min 1 max 128
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
- `setoption name USI_OwnBook value <bool>`
- `setoption name BookDir value <path>`
- `setoption name BookFile value <file>`
- `setoption name USI_Ponder value <bool>`
- `setoption name MultiPV value N`
- `setoption name Threads value N`
- `setoption name MinimumThinkingTime value N`
- `setoption name NetworkDelay value N`
- `setoption name NetworkDelay2 value N`
- `setoption name SlowMover value N`
- `setoption name ResignValue value N`
- `setoption name MaxMovesToDraw value N`
- `setoption name EnteringKingRule value <rule>`
- `setoption name GenerateAllLegalMoves value <bool>`
- `stop`
- `ponderhit`
- `quit`
- `bench depth N`
- `bench nodes N`
- `bench current depth N`
- `perft N`
- `perft depth N divide`
- `bench tsume [movetime M]`（詰み探索の固定局面ベンチマーク）

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
- `bench` と `perft` を内蔵しており、GUI 接続前の自己確認にも使えます。`bench tsume` は詰み探索の固定局面を順に解き、結果とノード数・時間を出力します。

## 既知の制約

- 探索は最小構成で、評価関数も簡易です。
- USI には引き分けを返す専用の `bestmove` がないため、千日手・持将棋など現在局面でゲーム終了と判定した場合は `info string terminal ...` を出した上で `bestmove resign` を返します。
- `perft` は `nodes` に加えて `captures` / `promotions` / `checks` / `mates` を出力します。
- `bench` は各局面と合計について `nodes` / `time` / `nps` / `hashfull` を出力し、`nodes N` 指定で固定ノード数ベンチとして使えます。

## 詰将棋の玉方（ShogiBoardQ連携）

`src/tsume.h` の `TsumeSearch` は、通常の評価値探索と分けて、王手の連続による
強制詰みを探索する。玉方ではすべての合法応手を調べ、詰む場合は最長抵抗、
不詰の場合は逃れる手を返す。攻め方の玉を省略したSFENも扱える。

探索は深さ優先の反復深化（深さ 1, 3, 5, …、玉方手番なら 0, 2, 4, …）で、最初に確定した
結果を返すため、`mate` の `plies` は最短の詰み手数になる。局面は `make_move` /
`unmake_move` で進めて戻し、置換表には詰み・不詰の証明を深さをまたいで保持する
（深さ打ち切りは「その深さまで詰みなし」としてだけ再利用し、不詰の証明とは区別する）。
置換表は 4 エントリのバケットを持つ固定サイズの表で、必要に応じて 2 倍に拡張し、
最大 32 MB に収まる。同じ `TsumeSearch` を続けて使うと、攻方が同じ間は前回の証明を再利用する。
`TsumeSearch` と `Position` はグローバルな可変状態を持たないので、スレッドごとに
別インスタンスを持てば並行して使える。計測結果は `BENCHMARK.md` を参照。

単独実行時は以下の独自USI拡張を利用できる（通常USIの `go mate` とは別のコマンド）。

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

- `attack` は現在手番を攻め方、`defense` は現在手番を玉方として探索する。
- `depth` は現在局面からの最大手数（既定31、上限63）、`movetime` はミリ秒（既定5000）。
- 応答の状態は `mate` / `nomate` / `depthlimit` / `timeout` / `cancelled`。
  不正な局面・コマンドでは `tsume invalid` を返す。
- `nomate` は不詰の証明。`depthlimit` は指定手数以内に詰みがないという意味で、
  それより長い詰みの否定ではない。`timeout` / `cancelled` は未判定。
- `move` は攻め方の詰め手、または玉方の抵抗・逃れの手。着手がない場合は `none`。
  `plies` は証明された詰みまでの手数で、`mate` の場合に有効。
- `stop` で中断結果を返し、`position` / `quit` は進行中の探索を破棄する。
- `TsumeMode` は既定false。通常の `position` では両玉が必要で、片玉局面での
  通常の `go` は拒否する。ShogiBoardQは同じコアを静的リンクして利用する。

## テストとベンチマーク

ビルド後、Python 3 の標準ライブラリだけで通常将棋と詰将棋のUSI動作を検証できます。

```bash
python3 tests/test_engine.py build/hayanagi
# 変更前の実行ファイルがあれば、通常将棋4局面のperft結果も比較する
python3 tests/test_engine.py build/hayanagi --baseline /path/to/previous/hayanagi
```

平手初期局面のperft、通常探索の合法着手、5手詰5問、別解、不詰、手数超過、
後手攻め、打ち歩詰め、入力不正、時間切れ、中止・局面切替を検証します。

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

詰み探索のベンチマークは `BENCHMARK.md` に記録しています。再計測は次のとおりです。

```bash
python3 tests/bench_tsume.py build/hayanagi [--baseline /path/to/previous/hayanagi]
# USI から直接実行する場合
./build/hayanagi <<< $'bench tsume\nquit'
```
