# 詰み探索ベンチマーク

`tests/bench_tsume.py` と USI コマンド `bench tsume` で使う固定局面の計測結果です。
改良前後の比較は次のコマンドで再現できます（`--baseline` に改良前の実行ファイルを渡す）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 tests/bench_tsume.py build/hayanagi --baseline /path/to/previous/hayanagi
```

## 計測環境

- 計測日: 2026-09-25
- OS: Linux 7.2 (x86_64)、GCC 16.2、`-O3`（`CMAKE_BUILD_TYPE=Release`）
- シングルスレッド。時間は 5 回計測した最良値（`go tsume ... movetime 600000` の応答までの実時間）
- 改良前: コミット `30cfce5`（ShogiBoardQ が参照していた版）
- 改良後: 1.0.0（本ブランチ）

## 詰み探索（`go tsume`）

| 局面 | 深さ | 結果 | 旧 nodes | 旧 時間(ms) | 新 nodes | 新 時間(ms) | 倍率 |
|---|---|---|---|---|---|---|---|
| mate5-1 | 5 | mate 3c5c+ plies 5 | 668 | 1.1 | 646 | 0.2 | 5.4x |
| mate5-2 | 5 | mate G*9g plies 5 | 34569 | 77.0 | 31840 | 4.2 | 18.3x |
| mate5-3 | 5 | mate R*1g plies 5 | 38531 | 94.9 | 38319 | 5.8 | 16.4x |
| mate5-4 | 5 | mate S*3b plies 5 | 12088 | 23.8 | 10544 | 1.5 | 15.6x |
| mate5-5 | 5 | mate R*9c plies 5 | 30824 | 65.0 | 30129 | 4.4 | 14.6x |
| defense6 | 6 | mate 4a4b plies 6 (旧 move: 4a5b) | 36882 | 62.4 | 24083 | 3.6 | 17.3x |
| nomate3 | 3 | depthlimit R*7i plies 0 | 13341 | 30.9 | 13341 | 1.3 | 24.5x |
| nomate5 | 5 | depthlimit R*7i plies 0 | 1840939 | 4793.3 | 1870972 | 198.8 | 24.1x |

局面の内容:

| 名前 | 局面 |
|---|---|
| mate5-1 | `9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1`（攻方、深さ 5） |
| mate5-2 | `9/9/9/R8/9/9/1k2+S4/9/3+N5 b RG2b3g3s3n4l18p 1`（攻方、深さ 5） |
| mate5-3 | `9/9/9/9/9/9/7+S1/5G2k/9 b RSr2b3g2s4n4l18p 1`（攻方、深さ 5） |
| mate5-4 | `5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1`（攻方、深さ 5） |
| mate5-5 | `1l7/k8/9/G8/3+R5/9/9/9/9 b R2b3g4s4n3l18p 1`（攻方、深さ 5） |
| defense6 | `5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1 moves S*4b`（玉方、深さ 6） |
| nomate3 / nomate5 | `9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1`（攻方の持駒が多い不詰局面、深さ 3 / 5） |

差分検証として、ランダムに生成した 1,500 局面（持駒あり、攻方玉なしを含む）で
旧実行ファイルと `perft depth 2 divide` の合法手一覧（並び順を含む）と nodes が一致することを確認しました。

`nodes` の数え方は改良前後で少し異なります（改良後は玉方の末端局面も 1 ノードとして数え、
置換表ヒットも数える）。`defense6` の応手が `4a5b` から `4a4b` に変わったのは、
`S*4b 4a5b` の後は 3 手で詰み、`S*4b 4a4b` の後は 5 手で詰むため、`4a4b` が本当の最長抵抗だからです
（改良前は反復深化の途中結果を深さをまたいで使わず、最初に見つかった詰み手数で応手を選んでいた）。

## 通常探索と perft

| 計測 | 旧 | 新 |
|---|---|---|
| perft depth 4（開始局面） | 719731 nodes, 95.0 ms | 719731 nodes, 78.0 ms |
| bench nodes 200000 | 761806 nodes, 2628.0 ms | 761806 nodes, 2072.0 ms |

perft の nodes と `bench` の探索ノード数は改良前後で一致しており、通常探索の挙動は変わっていません。

## 主な変更点

- 局面処理: 飛び利きをレイのビットボードで計算し、王手判定は玉と同一線上の飛び駒だけを調べる。
  `make_move` / `unmake_move` で局面をコピーせずに戻す。王手を利き升の交差で直接列挙する。
  合法手の有無を列挙せずに判定する `has_legal_move` を追加。
- 詰み探索: 深さごとの `std::unordered_map` を、詰み・不詰の証明を深さをまたいで再利用する
  固定サイズの置換表（4 エントリのバケット、必要に応じて 2 倍に拡張、最大 32 MB）に置き換え。
  玉方の末端局面では合法手の有無だけを判定する。時刻の確認は 128 ノードごとに行い、
  `stop` は毎ノード確認する。
