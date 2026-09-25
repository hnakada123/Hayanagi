#!/usr/bin/env python3
"""詰み探索のベンチマーク。BENCHMARK.md の表と同じ局面を go tsume で計測する。

Usage: python3 tests/bench_tsume.py build/hayanagi [--baseline OLD_ENGINE] [--repeat N]

各局面について status / move / plies / nodes / 時間を Markdown の表で出力する。
--baseline を指定すると旧エンジンも同じ局面で計測し、時間の比（旧 / 新）を併記する。
"""
import argparse
import subprocess
import time
from pathlib import Path

# (名前, SFEN, 手順, attack/defense, 深さ)
CASES = [
    ('mate5-1', '9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1', '', 'attack', 5),
    ('mate5-2', '9/9/9/R8/9/9/1k2+S4/9/3+N5 b RG2b3g3s3n4l18p 1', '', 'attack', 5),
    ('mate5-3', '9/9/9/9/9/9/7+S1/5G2k/9 b RSr2b3g2s4n4l18p 1', '', 'attack', 5),
    ('mate5-4', '5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1', '', 'attack', 5),
    ('mate5-5', '1l7/k8/9/G8/3+R5/9/9/9/9 b R2b3g4s4n3l18p 1', '', 'attack', 5),
    ('defense6', '5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1', 'S*4b', 'defense', 6),
    ('nomate3', '9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1', '', 'attack', 3),
    ('nomate5', '9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1', '', 'attack', 5),
]


class Engine:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding='utf-8', bufsize=1)
        self.send('usi')
        self.until('usiok')
        self.send('setoption name USI_OwnBook value false')
        self.send('setoption name TsumeMode value true')
        self.send('isready')
        self.until('readyok')

    def send(self, command):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def until(self, prefix):
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError(f'engine exited waiting for {prefix!r}')
            if line.startswith(prefix):
                return line.strip()

    def tsume(self, sfen, moves, side, depth, repeat):
        self.send('position sfen ' + sfen + (' moves ' + moves if moves else ''))
        best = None
        for _ in range(repeat):
            start = time.perf_counter()
            self.send(f'go tsume {side} depth {depth} movetime 600000')
            fields = self.until('tsume ').split()
            elapsed = time.perf_counter() - start
            result = dict(zip(fields[2::2], fields[3::2]))
            result['status'] = fields[1]
            result['seconds'] = elapsed
            if best is None or elapsed < best['seconds']:
                best = result
        return best

    def perft(self, depth):
        self.send('position startpos')
        self.send(f'perft depth {depth}')
        fields = self.until('info string perft').split()
        stats = dict(zip(fields[3::2], fields[4::2]))
        return int(stats['nodes']), int(stats['time']) / 1000.0

    def bench(self, nodes):
        self.send(f'bench nodes {nodes}')
        fields = self.until('info string bench total').split()
        stats = dict(zip(fields[4::2], fields[5::2]))
        return int(stats['nodes']), int(stats['time']) / 1000.0

    def close(self):
        self.send('quit')
        self.process.wait(timeout=5)


def fmt_ms(seconds):
    return f'{seconds * 1000:.1f}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--repeat', type=int, default=3, help='各局面の計測回数（最良値を採用）')
    args = parser.parse_args()

    engine = Engine(args.engine.resolve())
    baseline = Engine(args.baseline.resolve()) if args.baseline else None

    if baseline:
        print('| 局面 | 深さ | 結果 | 旧 nodes | 旧 時間(ms) | 新 nodes | 新 時間(ms) | 倍率 |')
        print('|---|---|---|---|---|---|---|---|')
    else:
        print('| 局面 | 深さ | 結果 | nodes | 時間(ms) |')
        print('|---|---|---|---|---|')
    for name, sfen, moves, side, depth in CASES:
        new = engine.tsume(sfen, moves, side, depth, args.repeat)
        summary = f"{new['status']} {new['move']} plies {new['plies']}"
        if baseline:
            old = baseline.tsume(sfen, moves, side, depth, args.repeat)
            if old['status'] != new['status'] or old['plies'] != new['plies']:
                summary += f" (旧: {old['status']} {old['move']} plies {old['plies']})"
            elif old['move'] != new['move']:
                summary += f" (旧 move: {old['move']})"
            ratio = old['seconds'] / new['seconds'] if new['seconds'] > 0 else float('inf')
            print(f"| {name} | {depth} | {summary} | {old['nodes']} | {fmt_ms(old['seconds'])} "
                  f"| {new['nodes']} | {fmt_ms(new['seconds'])} | {ratio:.1f}x |")
        else:
            print(f"| {name} | {depth} | {summary} | {new['nodes']} | {fmt_ms(new['seconds'])} |")

    print()
    print('| 計測 | 旧 | 新 |' if baseline else '| 計測 | 結果 |')
    print('|---|---|---|' if baseline else '|---|---|')
    perft_nodes, perft_seconds = engine.perft(4)
    bench_nodes, bench_seconds = engine.bench(200000)
    if baseline:
        old_perft_nodes, old_perft_seconds = baseline.perft(4)
        old_bench_nodes, old_bench_seconds = baseline.bench(200000)
        print(f'| perft depth 4 | {old_perft_nodes} nodes, {fmt_ms(old_perft_seconds)} ms '
              f'| {perft_nodes} nodes, {fmt_ms(perft_seconds)} ms |')
        print(f'| bench nodes 200000 | {old_bench_nodes} nodes, {fmt_ms(old_bench_seconds)} ms '
              f'| {bench_nodes} nodes, {fmt_ms(bench_seconds)} ms |')
        baseline.close()
    else:
        print(f'| perft depth 4 | {perft_nodes} nodes, {fmt_ms(perft_seconds)} ms |')
        print(f'| bench nodes 200000 | {bench_nodes} nodes, {fmt_ms(bench_seconds)} ms |')
    engine.close()


if __name__ == '__main__':
    main()
