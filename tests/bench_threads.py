#!/usr/bin/env python3
"""同じ通常探索ノード上限・perft 深さで 1/N スレッドの実時間を比較する。

python3 tests/bench_threads.py build/hayanagi --baseline /path/to/old/hayanagi
起動・Hash の初期化を除くコマンド応答時間の中央値を表示する。
"""
import argparse
from pathlib import Path
from statistics import median
import sys
import time

sys.dont_write_bytecode = True
from test_engine import Engine


def measure(engine, command, prefix):
    engine.send('setoption name Hash value 16\nposition startpos')
    engine.ready()
    start = time.perf_counter()
    engine.send(command)
    lines = engine.until(prefix, timeout=120)
    return (time.perf_counter() - start) * 1000, lines[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--threads', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--repeat', type=int, default=5)
    args = parser.parse_args()
    print('| 計測 | Threads | 旧 ms | 新 ms | 倍率 |', flush=True)
    print('|---|---|---|---|---|', flush=True)
    for threads in args.threads:
        old, new = Engine(args.baseline.resolve()), Engine(args.engine.resolve())
        try:
            for engine in (old, new):
                engine.send(f'setoption name Threads value {threads}\n'
                            'setoption name USI_OwnBook value false')
                engine.ready()
            for command, prefix in [('bench current nodes 400000', 'info string bench total'),
                                    ('perft depth 5', 'info string perft depth')]:
                samples = [[], []]
                for _ in range(args.repeat):
                    for i, engine in enumerate((old, new)):
                        elapsed, result = measure(engine, command, prefix)
                        if command.startswith('perft'):
                            assert 'nodes 19861490 ' in result, result
                        samples[i].append(elapsed)
                before, after = map(median, samples)
                print(f'| {command} | {threads} | {before:.1f} | {after:.1f} | '
                      f'{before / after:.2f}x |', flush=True)
        finally:
            old.close()
            new.close()


if __name__ == '__main__':
    main()
