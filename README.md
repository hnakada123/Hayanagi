# Hayanagi

Hayanagi は、C++17 で実装した USI プロトコル対応の最小構成将棋エンジンです。
通常対局用の探索に加えて、詰将棋用の詰み探索（`TsumeSearch`）を備え、
将棋 GUI [ShogiBoardQ](https://github.com/hnakada123/ShogiBoardQ) に静的ライブラリとして組み込まれています。

- 現在のバージョン: **1.4.0**（[変更履歴](#バージョンと変更履歴)）
- USI の `id name` は `Hayanagi 1.4.0`。`./build/hayanagi --version` でも表示できます
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
- Python 3（回帰テスト・ベンチマーク・定跡生成ツール。標準ライブラリだけで動きます）

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
| `build/hayanagi_book_positions` | 定跡生成用の棋譜検査・SFEN変換（`HAYANAGI_BUILD_BOOK_TOOLS` で制御） |
| `build/hayanagi_match_referee` | 定跡比較対局用の合法手・終局判定（同オプションで制御） |

コンパイル警告は `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion` を有効にしており、GCC と Clang のどちらでも警告なしでビルドできます（Clang の `-Wconversion` は `-Wsign-conversion` を含むため、GCC と同じ意味になるよう明示的に無効にしています）。

## 実行例

```bash
./build/hayanagi --version   # Hayanagi 1.4.0
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

`usi` に対しては次のように応答します（`Hayanagi 1.4.0` の出力）。

```text
id name Hayanagi 1.4.0
id author OpenAI
option name USI_Ponder type check default false
option name MultiPV type spin default 1 min 1 max 32
option name USI_ShowCurrLine type check default false
option name USI_ShowRefutations type check default false
option name USI_LimitStrength type check default false
option name USI_Strength type spin default 1 min -15 max 6
option name USI_AnalyseMode type check default false
option name Threads type spin default 1 min 1 max 128
option name Hash type spin default 16 min 1 max 65536
option name USI_Hash type spin default 16 min 1 max 65536
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
copyprotection checking
copyprotection ok
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
- `go ... searchmoves <move1> <move2> ...`（探索する初手を指定）
- `go ... movestogo N`（次の時間更新までの自分の残り着手数）
- `go ponder ...`
- `go infinite`（`stop` まで `bestmove` を返さない）
- `go mate <ミリ秒|infinite>`（標準の詰将棋解答。`checkmate` で全手順を返す）
- `go wtime ... btime ... byoyomi ...`
- `go tsume <attack|defense> depth N movetime M`（詰み探索。[詳細](#詰み探索詰将棋の攻方玉方)）
- `setoption name USI_OwnBook value <bool>`
- `setoption name BookDir value <path>`
- `setoption name BookFile value <file>`（`no_book` / `standard_book.db` / `yaneura_book1.db`〜`yaneura_book4.db` / `user_book1.db`〜`user_book3.db`）
- `setoption name USI_Ponder value <bool>`
- `setoption name MultiPV value N`
- `setoption name USI_MultiPV value N`（`MultiPV` の別名）
- `setoption name USI_ShowCurrLine value <bool>`
- `setoption name USI_ShowRefutations value <bool>`
- `setoption name USI_LimitStrength value <bool>`
- `setoption name USI_Strength value N`（-15〜6。負数は級、正数は段の目安）
- `setoption name USI_AnalyseMode value <bool>`
- `setoption name Threads value N`
- `setoption name Hash value N`（MB 単位）
- `setoption name USI_Hash value N`（標準名。`Hash` と同じ置換表を設定）
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
- `debug on|off`（診断情報の切り替え）
- `gameover win|lose|draw`（進行中の探索と先読みを終了）
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
- `USI_ShowCurrLine`
- `USI_ShowRefutations`
- `USI_LimitStrength`
- `USI_Strength`
- `USI_AnalyseMode`
- `Threads`
- `Hash`
- `USI_Hash`（`Hash` の別名）
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

定跡は盤面・手番・持駒で照合し、SFEN の手数が異なる同一局面にもヒットします。
ファイル内で先に現れる合法な候補手を採用します。千日手などの終局判定は定跡より優先し、
予想応手も合法性を確認します。生成ツールが出力する `# HAYANAGI_MAX_PLY 30` は、
実際の対局で30手目まで定跡を使う指定です。このコメントがない外部定跡には手数制限を加えません。

### Hayanagi の定跡生成

`tools/build_book.py` は、Floodgate の平手・通常初期局面の CSA 棋譜から定跡を生成します。
Python 標準ライブラリと、CMake で生成される `hayanagi_book_positions` を使います。
補助プログラムは Hayanagi 本体の `Position` を使用し、序盤の抽出範囲を超えて棋譜全体の合法性を検査します。
KIF、駒落ち、任意の開始局面、CSA の複数レコードをカンマで連結した行は、この取り込み器の対象外です。

初版の入力は [Floodgate 公式アーカイブ](https://wdoor.c.u-tokyo.ac.jp/shogi/) の2025年分です。
取得先と公式掲載の SHA-256 は以下のとおりです。大量の棋譜ページを個別取得せず、アーカイブを一度取得します。

```bash
mkdir -p build/book-work/csa
curl -fL https://wdoor.c.u-tokyo.ac.jp/shogi/archive/wdoor2025.7z -o build/book-work/wdoor2025.7z
sha256sum build/book-work/wdoor2025.7z
# 423504903f211316ec40f9c3d8dcc2e5faf392fc4d1334ede1489242a9d09baa
bsdtar -xf build/book-work/wdoor2025.7z -C build/book-work/csa --no-same-owner --no-same-permissions
```

ハッシュが一致することを確認してから展開してください。展開には7z対応の `bsdtar` または7-Zipが必要です。
配布ページでは生成定跡の再配布条件までは確認できていないため、生成物はローカル検証用として扱います。
サーバプログラムのライセンスを棋譜データのライセンスとみなさず、公開時にはデータの利用条件を別途確認します。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
python3 -B tools/build_book.py import \
  --input build/book-work/csa/2025 \
  --validator build/hayanagi_book_positions \
  --work-db build/book-work/floodgate2025.sqlite \
  --max-ply 30 --min-rating 3000 --min-game-plies 40 \
  --source-url https://wdoor.c.u-tokyo.ac.jp/shogi/archive/wdoor2025.7z \
  --source-sha256 423504903f211316ec40f9c3d8dcc2e5faf392fc4d1334ede1489242a9d09baa
python3 -B tools/build_book.py export \
  --work-db build/book-work/floodgate2025.sqlite \
  --output book/standard_book.db \
  --min-count 3 --min-pairs 2 --pair-cap 16 \
  --max-positions 5000 --max-candidates 3 \
  --engine build/hayanagi --nodes 100000 --workers 4
# 生成後に再構成すると、実行ファイルと同じディレクトリの book/ に定跡をコピーします。
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

取込時は、両対局者の記録レート3000以上、40手以上、通常の終局記録がある棋譜を対象にします。
棋譜全体の指し手列で重複を除き、そのハッシュで約10%を検証用に分離します。
各局面の候補手には3局以上・異なる対局者の組み合わせ2組以上の支持を要求します。
同じ組み合わせによる寄与は選択用の重みでは16局までに抑え、実際の出現回数は別に保持します。
対局者名はエンジンの独立性を保証するものではありません。
初手候補は既定で `7g7f`・`2g2f`・`5g5f`（`--first-moves` で変更可能）です。

`--engine` を指定すると、候補局面ごとに定跡を無効にした新しい Hayanagi プロセスを起動し、
全合法手の探索と、棋譜由来候補に制限した MultiPV 探索を各10万ノード行います。
深さ3以上の完了探索を必要とし、全合法手の探索結果より150以上低い評価や、負けの詰み評価がついた候補を除外します。
これは浅い探索による選別であり、定跡手の正しさや棋力向上の証明ではありません。
採用候補の並び順は、偏りを抑えた出現頻度順を維持します。棋譜に付属する評価値は流用しません。
`--engine` を省略すると頻度だけで生成し、評価値・深さは未解析を表す0になります。

出力する `standard_book.db` は既存の DB2016 形式です。
同名の `.json` に入力・生成条件・ハッシュ・除外件数・検証棋譜での命中率、
`.positions.jsonl` に各局面の代表手順・候補の支持数・解析結果を保存します。
検証は未使用棋譜の再生による収録範囲の測定であり、対局成績の測定ではありません。
SQLite は `build/book-work/` に置き、完了した解析を局面単位で保存します。
中断後は同じ `export` コマンドで解析を再開できます。入力条件を変える場合は新しい作業DBに取り込み直してください。

生成された `book/standard_book.db` がある場合、ビルド時に実行ファイル横へコピーされるため、
既定の `USI_OwnBook=true` のまま追加設定なしで使えます。未収録局面では通常探索へ戻ります。
本ツールでは評価関数の学習や自己対局による更新はまだ行いません。

```bash
python3 -B tests/test_book.py --engine build/hayanagi --validator build/hayanagi_book_positions
```

### 定跡あり／なしの対局検証

`tools/match_book.py` は、同じ Hayanagi の定跡あり／なしを先後入れ替えで比較します。
評価関数・定跡ファイルは更新せず、固定した生成物の効果を測定します。
１手のノード数、Threads=1、Hash=16 MiB、MultiPV=1、先読みなし、棋力制限なしを両者で揃え、
対局ごとに新しいプロセスを起動します。前の対局の置換表は引き継ぎません。
合法手・終局は `hayanagi_match_referee` が着手履歴を保持して判定します。
この判定器は Hayanagi の `Position` を共用し、USI の `resign` を千日手の負けと誤認しません。

次の例では、生成時に未使用とした棋譜から100局面を固定します。
初手が▲２六歩・▲７六歩・▲５六歩の棋譜から、2・4・6・8手進んだ局面を各25個抽出し、
盤面・手番・持駒の同じ局面を重複させません。定跡に当たるかどうかや評価値では選別しません。
この集合は多様な序盤を比較するためのもので、実戦での出現頻度を再現した標本ではありません。
通常の初期局面も別枠で検証し、同じ決定的な対局を繰り返して統計上の局数を増やしません。

```bash
cmake --build build -j 4
python3 -B tools/match_book.py prepare \
  --work-db build/book-work/floodgate2025.sqlite \
  --output book/evaluation/openings-20260930.json --pairs 100 --seed 20260930
python3 -B tools/match_book.py run \
  --engine build/hayanagi --referee build/hayanagi_match_referee \
  --book book/standard_book.db --openings book/evaluation/openings-20260930.json \
  --output book/evaluation/paired-20260930 --nodes 10000 50000 --max-plies 320 --workers 8
python3 -B tools/match_book.py audit \
  --engine build/hayanagi --output book/evaluation/paired-20260930 --nodes 200000 --workers 8
```

上記は100局面×先後2局×探索量2条件の400局と、初期局面からの4局、計404局です。
入玉は両者とも `CSARule24` とし、評価値による投了は無効です。
320手で打ち切った場合は `runner_ply_cap` の引き分けとして、千日手・持将棋と区別します。
定跡による時間節約を後の手へ再配分しない、ノード数固定の比較です。

`games/` に全着手・探索評価・定跡ヒット・終局理由を対局ごとに保存し、
`summary.json` に定跡側の勝敗と得点率（勝ち1、引き分け0.5、負け0）を集計します。
95%区間は先後2局を１組として10,000回再抽出するブートストラップで求めます。
各探索条件の区間であり、多重比較の補正はしません。関連する戦型同士の相関や、
同一エンジン相手への偏りがあるため、一般的な棋力・対人レーティングへ換算しません。

`audit` は最後の定跡ヒット後に定跡側が通常探索へ戻った局面と、その4手後・8手後を、
定跡なし・１局面20万ノードの新しい Hayanagi プロセスで解析します。
先後入れ替え対局の同じ色・同じ手数も参考対照として解析しますが、そこに至る手順や相手の定跡設定は異なります。
200以上の評価低下を集計し、終了済みの局面や詰み評価は通常の評価値平均から分離します。
Hayanagi 自身の評価なので、強い外部エンジンによる独立した正しさの検証ではありません。
詳細は `exit_windows.json` と `audit/` に残します。

エンジン・定跡・判定器・開始局面のハッシュと条件は `manifest.json` に固定します。
同じコマンドを再実行すると完了した対局・解析を再利用します。
条件やファイルを変更して比較する場合は、別の出力ディレクトリを指定してください。

2026-09-30 の生成定跡では、上記404局と定跡離脱前後の813局面の再解析を完了しました。
未使用棋譜の100局面について、定跡側の成績は次のとおりです。

| １手の探索量 | 勝・負・分 | 得点率 | 95%区間 | 定跡が使われた対局 |
| --- | --- | --- | --- | --- |
| 10,000ノード | 112・88・0 | 56.0% | 51.0–61.0% | 73/200局 |
| 50,000ノード | 89・105・6 | 46.0% | 41.25–50.75% | 69/200局 |

１万ノードでは改善が見られましたが、５万ノードでは改善を確認できませんでした。
通常の初期局面からの別枠対局も、それぞれ1勝1敗、0勝2敗です。
この結果だけでは、定跡による一貫した棋力向上を確認したとは判断できません。

定跡が使われた対局では、最後の定跡ヒット後に通常探索へ戻る手数の中央値は7手目・8手目でした。
その局面の20万ノード評価が定跡側から見て−200以下だった例は、両条件ともありませんでした。
８手後までに評価値が200以上低下した例は7/73局（9.6%）・4/69局（5.8%）、
平均変化は+24・+75でした。参考対照の定跡なし側では15/73局・12/69局でしたが、
対局経路と相手の設定も異なるため、この差を定跡単独の因果効果とは解釈しません。
最大の低下は１万ノードの `holdout_017` 先手で、離脱時+95から８手後−1479でした。

全404局・42,448手を再生し、合法性・終局理由・勝敗・開始局面の組合せを照合しました。
320手打切りはなく、終局理由は詰み395局、持将棋判定2局、千日手6局、連続王手の千日手1局です。
集計は [summary.json](book/evaluation/paired-20260930/summary.json)、
再生検証は [validation.json](book/evaluation/paired-20260930/validation.json)、
離脱前後の評価は [exit_windows.json](book/evaluation/paired-20260930/exit_windows.json) に保存しています。
次の改良では、定跡が続く範囲の拡大と、離脱後に大きく評価を落とした局面の追加検証が候補です。

### 対局結果からの原因調査

`tools/diagnose_book.py` は、保存済みの対局と定跡離脱後の解析結果を使って、
定跡手と通常探索のどちらに改善候補があるかを調べます。
定跡を実際に使った５万ノード条件の敗局について、自分の手番ごとに終局まで50万ノードで再解析します。
以前の検証で離脱後８手以内に評価が200以上落ちた対局も、勝敗を問わず対象にします。

全対局で実際に使われた定跡手、最初の通常探索の手、評価急落の直前の手などを抽出します。
各局面の全合法手探索と、実戦の手・再探索の推奨手に制限した MultiPV 探索を、
別々の新しいプロセスで各200万ノード実行します。候補間の評価差は、
全候補の正確な評価がそろった同じ完了深さで計算します。
全定跡手と、通常探索で200以上の差または詰みの差が出た手は、各1,000万ノードで再確認します。

```bash
python3 -B tools/diagnose_book.py \
  --matches book/evaluation/paired-20260930 \
  --output book/evaluation/diagnosis-20260930 --workers 8
```

条件と入力のハッシュは `manifest.json`、対象局面は `plan.json` に固定し、
解析は `analysis/` に保存します。同じコマンドで中断後に再開できます。
`comparisons.json` に実戦手・代替手・SFEN・手順・完了深さ・評価差、
`trajectories.json` に敗局の評価推移、`loss_classification.json` に分類を残します。
終局直前の自分の手は「次の自分の手番との評価差」で拾えないため、
予備解析で−200以上または勝ちの詰み評価なのに直後に負けた未調査の最終着手を、
`terminal_followup.json` で補足します。
補足対象と条件は `terminal_followup/manifest.json` に別途固定します。
通常探索の問題候補は予備解析から選ぶため、すべての着手の網羅的な品質検査ではありません。
Hayanagi 自身の評価関数による診断なので、評価関数の偏りや深さによる評価変動は残ります。
評価差は改善候補を示すもので、着手を変えれば勝敗が逆転するという証明ではありません。
今回の結果を改良に使う場合、改良後の最終評価には別の未使用局面を用意します。

2026-09-30 の原因調査では、2,500局面を予備解析し、376件の着手と終局直前の補足2件を比較しました。
評価差の確認は上限1,000万ノード、同じ完了深さの MultiPV で行いました。

| 調査対象 | 結果 |
| --- | --- |
| 実際に使われた定跡手100件 | 代替手との評価差は最大81。150以上の差は0件 |
| 定跡を使った５万ノード条件の41敗 | 37局に通常探索の問題候補、追加検査で1局に詰み探索のルール判定不具合、残る3局は原因未特定 |
| 初期局面からの５万ノード条件の2敗 | 両局に通常探索の問題候補 |
| 上記43敗の定跡離脱時点 | 200万ノード評価は最低−31、平均+50.1。−200以下は0局 |

通常探索の問題候補は、代替手との差200以上、または詰み評価の差が再確認できたものです。
本調査は通常探索の候補を選別しており、すべての悪手を検出したわけではありません。
37局の分類は [loss_classification.json](book/evaluation/diagnosis-20260930/loss_classification.json)、
追加検査を含む結論は [findings.json](book/evaluation/diagnosis-20260930/findings.json) に保存しています。
主分類の「未特定4局」のうち `50000-holdout_049-b` は、追加検査で次の不具合が判明しました。

- **詰み探索での連続王手の千日手の見落とし**：`50000-holdout_049-b` の67手目は、
  `6a7a` を指すと直ちに連続王手の千日手で負けますが、エンジンは `mate 3` を返してこの手を選びます。
  `Search::mate_search_attack` / `mate_search_defense` は再帰中に `terminal_status()` を確認していません。
  同じ局面・５万ノードでも、全合法手を `searchmoves` に指定して冒頭の詰み探索を省くと、
  `3a4a` を選び、この即時の負けを避けます。これ自体は以後の勝利の保証ではありません。
  再現手順と生出力は [perpetual_check_probe.json](book/evaluation/diagnosis-20260930/perpetual_check_probe.json) にあります。
- **深さ１での１手詰めの見落とし**：`50000-holdout_070-b` の59手目▲９一角成は、
  △７八飛打で詰みます。対局時の完了深さは1でした。代替の▲８八金打なら、同じ飛打に▲同金と取れます。
  この違いは評価値だけでなく、合法手と終局判定でも確認しました。
  記録は [mate_validation.json](book/evaluation/diagnosis-20260930/mate_validation.json) にあります。
- **離脱直後の戦術上の見落とし**：`50000-holdout_099-b` の17手目▲７七桂は−529、
  代替の▲７七銀は−9で、深さ10の比較で差520でした。▲７七銀は△８八角打に備えられます。
  また `10000-holdout_017-b` の９手目▲２五歩打は飛車の退路を塞ぎ、
  ▲２八飛との差は深さ13で1489でした。この対局は最後には勝っており、局所的な失敗と勝敗は区別しています。

別途、`50000-holdout_042-b` の123手目では、50万ノードでも深さ１を完了できず、
静止探索で予算を使い切る挙動を再現しました。詰み探索を省く条件でも発生します。
５万ノードの元対局で評価値が出なかった通常着手は、定跡側・定跡なし側とも53回でした。
この問題だけで定跡側の得点差を説明できるとは判断していません。
再現記録は [search_budget_probe.json](book/evaluation/diagnosis-20260930/search_budget_probe.json) にあります。

今回の優先修正箇所は、詰み探索中の終局ルール判定と、浅い探索で即詰みを見落とす挙動です。
定跡の大幅な評価劣化は今回の範囲では確認されませんでしたが、Hayanagi自身の評価による結果です。
元の得点率46%の95%区間も50%を含むため、定跡が一般に棋力を下げたと断定する結果ではありません。
報告した1,132本の読み筋を合法手として再生し、評価差の計算とエンジン・定跡のハッシュを照合済みです。
詳細は [validation.json](book/evaluation/diagnosis-20260930/validation.json) に保存しています。

### 原因調査で見つかった探索の修正

上記の調査後、通常対局の詰み探索でも着手履歴を更新し、各節点で終局ルールを確認するようにしました。
履歴を更新しない `make_move` では、終局判定を追加するだけでは連続王手の千日手を検出できないため、
この探索では履歴を保存する `do_move` を使います。
深さ１の末端でも、駒を取らない王手・駒打ちによる１手詰めを確認します。

静止探索は通常探索の深さとは別に最大６手までとし、境界で王手されている場合は合法な応手を評価します。
冒頭の５手詰め探索にもノード・時間の部分予算を設けました。
反復深化が完了しない場合にも評価済みの合法手を返すため、先に浅い候補比較を行います。
極端に小さい探索量では、この比較自体が完了する保証はありません。
王手になる駒取りは、単純な駒の交換損だけを理由に静止探索から除外しません。

評価関数には、敵に攻撃され味方に守られていない歩以外の駒の割引と、
飛車の安全な移動先が少ない場合の減点を追加しました。どちらも先後共通の規則で、特定の棋譜や手順は参照しません。
係数は手作業による初期値です。駒の利きに基づく近似であり、学習済み評価関数や完全な戦術判定ではありません。

修正前と同じ棋譜履歴・ノード数で、新しいプロセスから探索した結果です。

| 再現例 | 修正前 | 修正後 |
| --- | --- | --- |
| `049`・67手目、５万ノード | `6a7a` を３手詰めと誤認し、連続王手で即負け | `4d4b+` を選択し、即負けを回避 |
| `070`・59手目、５万ノード | `6d9a+` の直後に１手詰め | `G*6h` を選択。深さ１制限でも即詰みを回避 |
| `042`・123手目、５万ノード | 深さ１を完了せず `5i4i` | 深さ４まで完了して `P*6c` |
| `099`・17手目、５万ノード | 角打ちを許す `8i7g` | `6g6f` |
| `017`・９手目、１万ノード | 飛車の退路を塞ぐ `P*2e` | `2d2f` |

上限1,000万ノードの候補制限 MultiPV でも比較し、最後の２例は修正前の評価関数でも、
新しい着手がそれぞれ500・1701高い評価でした（各比較は同じ完了深さ11・14）。
詰み・千日手・読み筋は合法手として再生して検証します。
元対局の５万ノード条件で評価値が出なかった106着手は、すべて新しい探索で評価値が出ました。
１万ノード条件では168着手中151着手で改善し、17着手では完了した反復の評価値が出ませんでした。
局面の再生は重複を含み、両条件合計の独立した入力履歴は200件です。

テスト入力は `tests/fixtures/search_regressions.json` にあり、大きな棋譜データを必要としません。
修正前バイナリでは８件のサブテストが失敗し、修正後はすべて成功します。
連続王手、深さ１の即詰み回避、静止探索の予算、微小予算での初手制限は１・４スレッドで検証します。

```bash
ctest --test-dir build --output-on-failure
python3 -B tests/test_engine.py build/hayanagi
python3 -B tests/test_search_regressions.py \
  --engine build/hayanagi --referee build/hayanagi_match_referee \
  --report book/evaluation/search-fixes-20260930/regressions-after.json
```

個別局面の比較は [comparisons.json](book/evaluation/search-fixes-20260930/comparisons.json)、
評価値の出力状況は [score-coverage.json](book/evaluation/search-fixes-20260930/score-coverage.json)、
再発防止テストの記録は [regressions-after.json](book/evaluation/search-fixes-20260930/regressions-after.json) に保存しています。
通常の戦術ミスがすべてなくなったという結果ではありません。
追加比較した `083` の55手目では、新しい選択手が深い探索で以前の手より低く評価される例もあります。
局面ごとの改善と、対局全体の改善は分けて検証します。

対局評価には、以前の100局面とSFENが重複しない新しい100局面を固定しました。
短い手順では未使用局面が少ないため、2・4・6・8手の内訳は5・32・32・31局面です。
以前の集合と分布が異なるので、得点率をそのまま前後比較しません。
修正前後の直接対戦は両者とも定跡を切り、同じ新局面・探索量・先後交換で比較します。
定跡あり／なしの比較は別に実施します。

```bash
python3 -B tools/match_book.py prepare \
  --work-db build/book-work/floodgate2025.sqlite \
  --exclude-openings book/evaluation/openings-20260930.json \
  --output book/evaluation/search-fixes-20260930/openings.json --pairs 100 --seed 20260931
python3 -B tools/match_search.py \
  --engine build/book-evaluation-bin/hayanagi-after-search-fixes \
  --baseline build/book-evaluation-bin/hayanagi-before-search-fixes \
  --openings book/evaluation/search-fixes-20260930/openings.json \
  --output book/evaluation/search-fixes-20260930/engine-matches --workers 6
python3 -B tools/match_book.py run \
  --engine build/book-evaluation-bin/hayanagi-after-search-fixes \
  --openings book/evaluation/search-fixes-20260930/openings.json \
  --output book/evaluation/search-fixes-20260930/book-matches --workers 6
python3 -B tools/validate_matches.py --output book/evaluation/search-fixes-20260930/engine-matches
python3 -B tools/validate_matches.py --output book/evaluation/search-fixes-20260930/book-matches
```

`build/book-evaluation-bin/` の実行ファイルは、この修正の前後に保存したローカルの比較用コピーです。
対局条件と実行ファイルのハッシュは各 `manifest.json` に固定しています。
異なるビルドで再評価する際は別の出力ディレクトリを使います。

修正前後の直接対戦404局を完了しました。新しい100局面の先後交換200局について、修正後の成績は次のとおりです。

| １手の探索量 | 修正後の勝・負・分 | 得点率 | 先後ペア単位の95%区間 |
| --- | --- | --- | --- |
| 10,000ノード | 116・83・1 | 58.25% | 51.5–65.0% |
| 50,000ノード | 111・87・2 | 56.0% | 49.5–62.25% |

初期局面の別枠は両条件とも1勝1敗でした。１万ノード条件で改善が見られましたが、
５万ノード条件の区間は50%を含みます。ノード数固定・この相手と開始局面に対する結果であり、
同じ持ち時間での向上や一般的なレーティングを保証しません。
対局記録と集計は [engine-matches/summary.json](book/evaluation/search-fixes-20260930/engine-matches/summary.json) にあります。

修正後エンジン同士の定跡あり／なし404局も完了しました。同じ新規100局面での定跡側の成績です。

| １手の探索量 | 定跡側の勝・負・分 | 得点率 | 先後ペア単位の95%区間 |
| --- | --- | --- | --- |
| 10,000ノード | 108・92・0 | 54.0% | 50.0–58.0% |
| 50,000ノード | 107・92・1 | 53.75% | 49.25–58.25% |

どちらの区間も50%を含むため、定跡単独の優位性が確立したとは判断しません。
修正前後のエンジン比較と混ぜずに、[book-matches/summary.json](book/evaluation/search-fixes-20260930/book-matches/summary.json) に保存しています。
両対局群の棋譜は `tools/validate_matches.py` で再生し、合法手・終局理由・先後の組合せ・勝敗・定跡ヒットを照合しました。
`build/hayanagi` と `build-hayanagi/hayanagi` の両方で、追加設定なしに独自定跡を読み込み、
初手 `2g2f` を返すことも確認しています。

### 通常探索のオプション

`MultiPV` を有効にすると、`info ... multipv N pv ...` を順位ごとに出力します。`GenerateAllLegalMoves` は互換性維持のため残していますが、現在は探索強度を落とさないよう no-op です。

`USI_LimitStrength` は既定で false です。true にすると `USI_Strength`（既定1、範囲-15〜6）に応じて
探索の基本深さとノード数を制限します。負数は級、正数は段の目安、0は1級相当として扱います。
**段級位は対局による校正前の目安であり、実際の人間の段級位に一致する保証はありません。**
探索量の代表値は以下のとおりです。基本深さの先では通常どおり静止探索を行います。

| `USI_Strength` | 基本深さの上限 | ノード数の上限 |
|---|---:|---:|
| -15 | 1 | 256 |
| -10 | 2 | 1,536 |
| -5 | 4 | 8,192 |
| -1 / 0 | 5 | 32,768 |
| 1 | 6 | 49,152 |
| 6 | 7 | 262,144 |

`go depth` / `nodes` がより小さければそちらを優先し、時間制限も守ります。
並列探索の停止処理ではノード数がワーカー数程度超過する場合があります。
棋力制限中は定跡による即答と独立した詰み事前確認を使わず、置換表は今回の探索のワーカー間だけで共有します。
過去の強い探索や同じ弱い探索の繰り返しによって、制限を越えた読みを流用しません。
`USI_LimitStrength` が false の場合、`USI_Strength` は探索に影響しません。

`USI_AnalyseMode` は既定で false です。true にすると棋力制限、定跡による即答、
`ResignValue` による自動投了、`bestmove` の先読み要求を無効にします。
詰みや入玉などの終局判定と、明示した探索深さ・ノード数・時間の指定は維持します。
解析モード自体は時間無制限の指定ではないので、時間無制限の検討には `go infinite` を併用してください。
false に戻すと、保存してある元の棋力・定跡・投了・先読み設定で通常対局に戻ります。
これらの設定変更は次の `go` から適用します。`go infinite` は `stop`、`go ponder` は `stop` または
`ponderhit` まで返答を待ちます。対局用の棋力制限は `go mate` / `go tsume` / `bench` / `perft` には適用しません。

```text
setoption name USI_LimitStrength value true
setoption name USI_Strength value -5
position startpos
go btime 60000 wtime 60000 byoyomi 1000
```

`searchmoves` は初手だけを制限します。例えば `go depth 6 searchmoves 7g7f 2g2f` は、
その2手のいずれかを初手とする手順を探索します。定跡も指定された合法手から選び、
詰み確認や中断時の代替手が制限を迂回することはありません。重複・不正・非合法な指し手は除き、
候補が空なら `bestmove resign` を返します。制限は次の `go` に引き継ぎません。
`MultiPV` の出力数は制限後の合法な候補数が上限です。

`movestogo N` を指定すると、持ち時間を N で割った値を時間配分の基準にします。
未指定または N が0以下なら従来の30手分配を使います。加算・秒読み・通信遅延・
持ち時間による上限も適用し、`movetime` 指定がある場合はそちらを優先します。

`Threads` は通常探索、通常探索前の詰み確認、`go mate` / `go tsume` / `bench tsume`、`perft` に適用されます。
既定値は 1、範囲は 1〜128 です。例えば `setoption name Threads value 4` で計算を最大 4 スレッドに分担します
（USI コマンドを受け付けるスレッドは別）。小さい探索では管理コストを避けるため逐次処理し、
`perft` は深さ 3 以上で並列化します。`divide` の出力順と各統計はスレッド数によらず同じです。

通常探索の `info` には `depth` / `seldepth` / `score cp` / `nodes` / `time` / `nps` / `hashfull` / `cpuload` / `pv` を出力します。
詰みを見つけた場合は `score mate N`、詰まされる場合は `score mate -N` で手数を通知します。
入玉宣言や連続王手の千日手による勝敗は詰みとは区別します。
探索中は約1秒間隔でノード数・ハッシュ使用率・現在の候補手 `currmove` と、探索順で1から数えた `currmovenumber` も通知します。
`cpuload` は探索開始以降のプロセス CPU 時間を、経過時間と実際の探索ワーカー数で割った使用率です。
全ワーカーを使い切った状態を1000とし、0〜1000で通知します。CPU 時間を取得できない環境では省略します。

通常探索の SinglePV では深さ5以降、直前の評価値の周囲を先に調べる aspiration window を使います。
評価値が探索窓を外れた場合は、確認できた下限に `lowerbound`、上限に `upperbound` を付けて通知します。
その後は探索窓を広げて再探索し、値が確定するまで完了深さと最善手を更新しません。
再探索中の `stop` では、最後に完了した反復の最善手を返します。

`USI_ShowCurrLine` を true にすると、各ワーカーの探索中の合法手順を `info currline <番号> <手順>` で通知します。
ワーカー番号は1から始まり、全ワーカー分をまとめて約1秒間隔で出力します。手順は約100ミリ秒間隔で採取し、
待機中のワーカーは手順を空にします。探索内部の null move より先は通知しません。
`USI_ShowRefutations` を true にすると、各候補手について探索から得られた応手・手順を `info refutation <初手> <応手…>` で通知します。
応手が得られていない場合は初手だけを返し、探索中の定期通知と探索終了時に出力します。
両オプションとも既定は false で、次の `go` から反映します。

`debug on` は受信コマンドや探索の時間配分を `info string debug ...` で通知します。
既定は off です。探索中でも `debug on` / `debug off` を切り替えられます。

`USI_Ponder` を有効にすると、通常探索の `bestmove` は `bestmove <move> ponder <move>` を返します。`go ponder` で始めた探索は `ponderhit` または `stop` を受けるまで `bestmove` を返しません。
`go infinite` は、詰みや終局を検出した場合も `stop` を待ちます。無制限探索では定跡による即時返答を行いません。
`gameover` / `position` / `usinewgame` / `quit` は古い探索結果を破棄します。
コマンドは LF・CRLF に対応し、`BookDir` には空白を含むパスも指定できます。

[将棋所のUSI仕様](https://shogidokoro2.stars.ne.jp/usi.html)に掲載された原案の未対応コマンド一覧のうち、
Hayanagi では利用者登録用の `register` / `registration` を除いて実装しています。
`copyprotection` はエンジンからGUIへの通知です。`usiok` の後に `copyprotection checking` と
`copyprotection ok` を返します。公開版にはコピー制限がないため確認結果は常に利用可であり、
ライセンス認証・登録処理・外部への問い合わせは行いません。
`searchmoves` / `movestogo` や追加の探索情報の意味は [USI原案](https://hgm.nubati.net/usi.html) に従います。
これらの追加機能は将棋所自身では未対応のため、対応 GUI や検証ツールから利用してください。

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
- 通常探索前の王手限定探索も候補手で分担し、同じワーカー群を反復深化と次の通常探索で再利用します。
- ノード数はワーカーごとに数えて 128 ノード単位で集計します。`go nodes` 指定時は毎ノード反映して停止判定の精度を保ちます。
- 停止要求は毎回確認し、時刻取得は原則 128 ノード間隔です。駒交換評価（SEE）の再計算と、枝刈りに不要な局面評価を省きます。
- `Hash` オプションでサイズ変更できる lockless 共有置換表、PVS、quiescence search、SEE ベースの着手順序、killer/history heuristic、LMR、null-move pruning を使って探索効率を上げています。
- 短手数の詰みは専用の王手限定探索で先に検出します。
- 評価関数は駒得に加え、駒の前進度、利きの広さ、敵陣進出、手駒価値、玉の安全度を見ます。

### `Book`

- やねうら王の定跡フォーマット（`#YANEURAOU-DB2016 1.00`）を読み込みます。
- SFEN の盤面・手番・持駒をキーとして定跡手を検索します。手数違いの同一局面も照合します。
- Hayanagi 生成定跡の手数上限は `# HAYANAGI_MAX_PLY` コメントから読み込みます。

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
- `threads > 1` を渡すと、十分な探索量がある局面の候補手を並列に調べます。玉方の応手が少ないときは、次の攻方の王手で分担します。各ワーカーは独立した局面・置換表を持ち、反復深化の間は再利用します（置換表の最大 32 MB はワーカーごと）。
- 結果は候補手の生成順で集約し、最短の詰み手数・玉方の最長抵抗を保ちます。結論が出た後の不要な候補は打ち切ります。
- `TsumeSearch` と `Position` はグローバルな可変状態を持たないので、スレッドごとに別インスタンスを持てば並行して使えます。
- 計測結果は [BENCHMARK.md](BENCHMARK.md) を参照してください。

### 標準 USI `go mate`

現在手番を攻方として解答します。制限時間は手数ではなくミリ秒です。
攻方の玉を省略した局面でも、`TsumeMode` を設定せずに利用できます。

```text
position sfen 9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1
go mate 5000
```

応答例:

```text
checkmate 3c5c+ 4d4e 1c4c P*4d 4c4d
```

- 詰みの場合は最長抵抗を含む合法な全手順、不詰を証明できた場合だけ `checkmate nomate` を返します。
- 時間切れや未解決のままの `stop` には `checkmate timeout` を返します。通常探索の `bestmove` は返しません。
- `go mate infinite` は時間制限なしで探索し、詰み・不詰が確定した時点で返答します。
- 内部の探索上限は **63手** です。上限に達しても不詰と判定せず、その旨を `info string` で通知し、期限または `stop` まで待って `checkmate timeout` を返します。
- `position` / `gameover` / `quit` による中断では結果を破棄します。
- C++ API の `TsumeSearch::solve` では `time_limit_ms == 0` が時間無制限です。USI の `go mate 0` は制限時間0ミリ秒として扱います。

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
- `TsumeMode` は既定 false です。片玉局面で独自拡張の `go tsume` を使う場合は true にします。標準 `go mate` は設定不要ですが、片玉局面での通常の `go` は拒否します。

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
  - `TsumeSearch::solve(position, attacker, max_plies, time_limit_ms, stop, threads = 1)`: `TsumeResult{status, move, plies, nodes}` を返します。従来の 5 引数呼び出しも利用できます。`max_plies` は 63、`threads` は 1〜128 に丸められ、`stop` は他スレッドから立てると速やかに `Cancelled` で戻ります。CMake ターゲットは `Threads::Threads` を依存先に公開しています。
- 探索結果の意味を変える改良をしたときは、組み込み側で結果をキャッシュしているキー（ShogiBoardQ では Hayanagi の版を含む）を更新する必要があります。

## テストとベンチマーク

ビルド後、Python 3 の標準ライブラリだけで通常将棋と詰将棋の USI 動作を検証できます。

```bash
python3 tests/test_engine.py build/hayanagi
# 変更前の実行ファイルがあれば、通常将棋 4 局面の perft 結果も比較する
python3 tests/test_engine.py build/hayanagi --baseline /path/to/previous/hayanagi
python3 tests/test_engine.py build/hayanagi --threads 4 --baseline /path/to/previous/hayanagi
# 同じスレッド設定で通常探索と perft の実時間を比較（各 5 回の中央値）
python3 tests/bench_threads.py build/hayanagi --baseline /path/to/previous/hayanagi
# 詰み探索を 4 スレッドで計測
python3 tests/bench_tsume.py build/hayanagi --threads 4 --baseline /path/to/previous/hayanagi
```

平手初期局面の perft、通常探索の合法着手、5 手詰 5 問、別解、不詰、手数超過、
後手攻方、打ち歩詰め、入力不正、時間切れ、中止・局面切替、`id name` の形式を検証します。
標準 `go mate` の全手順、符号付き詰みスコア、`USI_Hash`、`gameover`、
詰み・定跡がある場合の無制限探索、先読み、CRLF、空白付き定跡パス、探索情報の出力も検証します。
初手制限と定跡・MultiPV・中断の組み合わせ、残り着手数による時間配分、診断切り替え、
全ワーカーの合法な探索手順・反駁手順・CPU 使用率、上下限からの再探索と中断も検証します。
棋力制限の上限・切り替え・置換表の分離、初手制限や先読み待機との組み合わせ、
解析モードでの定跡・投了抑止と設定の復帰、コピー保護通知の順序も検証します。

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
| `src/parallel.h` | 再利用可能な並列ワーカー群 |
| `src/book.h/.cpp` | 定跡の読み込み |
| `src/usi_engine.h/.cpp`, `src/main.cpp` | USI プロトコル処理、`bench` / `perft` |
| `src/cpu_time.h` | CPU 使用率通知のためのプロセス CPU 時間計測 |
| `src/version.h` | バージョン定義（`HAYANAGI_VERSION`） |
| `tests/test_engine.py` | USI 回帰テスト |
| `tests/test_*.cpp`, `tests/test_support.h` | C++ 単体テスト |
| `tests/bench_tsume.py` | 詰み探索ベンチマーク |
| `tests/bench_threads.py` | 通常探索・perft のスレッド数別ベンチマーク |
| `BENCHMARK.md` | ベンチマークの計測結果 |

## バージョンと変更履歴

バージョンは `src/version.h` の `HAYANAGI_VERSION` が唯一の定義元で、CMake の `project(... VERSION)`、
USI の `id name`、`--version` はすべてここから読みます。リリース時はこの値と本節を更新し、
同じ番号のタグ（`v1.4.0` など）を付けます。ShogiBoardQ はタグで指定した版をサブモジュールとして参照します。

| バージョン | タグ | 日付 | 概要 |
|---|---|---|---|
| 1.4.0 | [v1.4.0](https://github.com/hnakada123/Hayanagi/releases/tag/v1.4.0) | 2026-09-28 | 棋力制限・解析モード、コピー保護状態の通知 |
| 1.3.0 | [v1.3.0](https://github.com/hnakada123/Hayanagi/releases/tag/v1.3.0) | 2026-09-28 | 初手制限・残り手数指定・デバッグ、追加探索情報と評価値の上下限通知 |
| 1.2.0 | [v1.2.0](https://github.com/hnakada123/Hayanagi/releases/tag/v1.2.0) | 2026-09-28 | 標準詰将棋解答・詰みスコア、USI 互換性と探索終了処理の改善 |
| 1.1.0 | [v1.1.0](https://github.com/hnakada123/Hayanagi/releases/tag/v1.1.0) | 2026-09-28 | 詰み確認・詰将棋・perft の並列化、ワーカー再利用、逐次処理の高速化 |
| 1.0.1 | [v1.0.1](https://github.com/hnakada123/Hayanagi/releases/tag/v1.0.1) | 2026-09-25 | Clang での `-Wsign-conversion` 警告を解消（ShogiBoardQ が参照中） |
| 1.0.0 | [v1.0.0](https://github.com/hnakada123/Hayanagi/releases/tag/v1.0.0) | 2026-09-25 | 詰み探索と局面処理の高速化、ベンチマーク、単体テスト、バージョン情報 |

### 1.4.0（2026-09-28）

- `USI_LimitStrength` / `USI_Strength` による探索量の制限と、`USI_AnalyseMode` による解析用動作を追加しました。
- `copyprotection checking` / `ok` の通知に対応しました。利用者登録やコピー制限は追加していません。
- 棋力制限時の探索上限・置換表再利用と、解析モード・定跡・投了・先読みの組み合わせを検証するテストを追加しました。
- `go depth N infinite` でも指定した探索深さを維持するように修正しました。
- USI 回帰テスト37件と C++ テストを通過し、追加機能のスレッド競合検査も実施しました。

### 1.3.0（2026-09-28）

- `go searchmoves` / `movestogo` と `debug on/off` に対応しました。
- `info currmovenumber` / `cpuload` / `currline` / `refutation` と、表示切り替えオプションを追加しました。
- 探索窓を絞った探索で確認した `lowerbound` / `upperbound` を通知し、全窓で再探索して評価値を確定します。
- `searchmoves` と定跡・並列探索・MultiPV・中断・置換表再利用の整合性を検証するテストを追加しました。
- USI 回帰テストを32件に拡充し、C++ テスト、競合・メモリー・未定義動作・リーク検査を通過しました。

### 1.2.0（2026-09-28）

- 標準 `go mate` / `checkmate` と `score mate` に対応し、入玉などの勝敗との区別を追加しました。
- `USI_Hash` と `gameover` に対応し、`go infinite` の返答待機、先読み終了時の結果出力を修正しました。
- `seldepth` / `hashfull` / `currmove` の通知、並列出力の排他処理、CRLF・空白付きパス対応を追加しました。
- 不正な `position` の後に以前の局面で通常探索してしまう問題を修正しました。
- 持ち時間・秒読みともに0の場合の即時停止と、大きな持ち時間の計算時の桁あふれを修正しました。
- USI 回帰テストを25件に拡充し、既存の C++ テスト、競合・メモリー・未定義動作・リーク検査を通過しました。

### 1.1.0（2026-09-28）

- `Threads` を通常探索前の詰み確認、`go tsume` / `bench tsume`、`perft` に適用しました。小さい探索は逐次処理し、詰み手数・玉方の最長抵抗・perft の統計と出力順を維持します。
- 通常探索のワーカーを再利用し、共有ノード数の更新をまとめました。局面コピー、メモリー確保、不要な局面評価、SEE の重複計算、履歴走査の参照数更新も削減しました。
- `TsumeSearch::solve` に省略可能な `threads` 引数を追加しました。既存の 5 引数呼び出しも利用でき、既定値は 1 です。
- 変更前との実測比較では、1 スレッドの通常探索が 1.12 倍、perft が 2.03 倍、4 スレッドの perft が 7.54 倍、重い詰み探索が約 3 倍になりました（[計測条件と結果](BENCHMARK.md#2026-09-28-並列化と逐次処理の改善)）。
- 並列処理の回帰テストとスレッド数別ベンチマークを追加し、競合・メモリー・未定義動作・リーク検査を実施しました。

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
