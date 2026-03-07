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
- `go wtime ... btime ... byoyomi ...`
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
- `quit`
- `bench depth N`
- `bench nodes N`
- `bench current depth N`
- `perft N`
- `perft depth N divide`

実装済みの主要オプションは次のとおりです。

- `MultiPV`
- `Threads`
- `MinimumThinkingTime`
- `NetworkDelay`
- `NetworkDelay2`
- `SlowMover`
- `ResignValue`
- `MaxMovesToDraw`
- `EnteringKingRule`
- `GenerateAllLegalMoves`

`MultiPV` を有効にすると、`info ... multipv N pv ...` を順位ごとに出力します。`GenerateAllLegalMoves` は探索時の合法手生成にだけ反映し、`position` / `perft` の入力互換性は維持します。

通常探索の `info` には `depth` / `score cp` / `nodes` / `time` / `nps` / `pv` を出力します。

## 実装概要

### `Position`

- 9x9 盤面、持ち駒、手番を保持します。
- `startpos` / `sfen` の読み込み、USI 指し手の適用、合法手生成を担当します。
- 盤面配列に加えて色別・駒種別ビットボードを持ち、王手検出、ピン判定、合法手生成をビットボード主導で行います。
- 玉の回避手、単王手時の合駒・駒取り制限、ピンされた駒の移動制限を、局面全体の後段フィルタではなく直接生成で処理します。
- 1 手ごとの合法性確認で局面コピーを多用しないよう、玉移動の被攻撃判定は専用の軽量経路に分けています。

### `Search`

- 反復深化付きの `negamax + alpha-beta` 探索を行います。
- `Threads > 1` のときは root move 単位で並列探索します。
- 置換表、PVS、quiescence search、SEE ベースの着手順序、killer/history heuristic、LMR、null-move pruning を使って探索効率を上げています。
- 短手数の詰みは専用の王手限定探索で先に検出します。
- 評価関数は駒得に加え、駒の前進度、利きの広さ、敵陣進出、手駒価値、玉の安全度を見ます。

### `UsiEngine`

- `usi` / `isready` / `position` / `go` / `stop` / `quit` を処理します。
- 探索はワーカースレッドで実行し、`stop` に反応できる構成にしています。
- `bench` と `perft` を内蔵しており、GUI 接続前の自己確認にも使えます。

## 既知の制約

- 探索は最小構成で、評価関数も簡易です。
- USI には引き分けを返す専用の `bestmove` がないため、千日手・持将棋など現在局面でゲーム終了と判定した場合は `info string terminal ...` を出した上で `bestmove resign` を返します。
- `perft` は `nodes` に加えて `captures` / `promotions` / `checks` / `mates` を出力します。
- `bench` は各局面と合計について `nodes` / `time` / `nps` / `hashfull` を出力し、`nodes N` 指定で固定ノード数ベンチとして使えます。
