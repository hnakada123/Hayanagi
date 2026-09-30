#!/usr/bin/env python3
"""旧版から変えた定跡手を同じ深さ・別プロセスで追加解析する。"""
import argparse
from pathlib import Path

from build_book import analyze_position, digest_file, positive
from match_book import Referee, parallel_jobs, position_command, read_json, write_json


def run(args):
    source = read_json(args.positions)
    engine_hash = digest_file(args.engine)
    def task(position):
        moves = sorted({position['old_move'], position['new_move']})
        analysis = analyze_position({'history': position['history'], 'entries': [{'move': m} for m in moves]},
                                    args.engine, args.nodes)
        if 'error' in analysis:
            raise RuntimeError(analysis['error'])
        with Referee([str(args.referee.resolve())]) as referee:
            for result in list(analysis['candidates'].values()) + [analysis['discovery'], analysis['baseline']]:
                state = referee.state(position_command(position['history']))
                for move in result['pv']:
                    if state['outcome'] != 'none' or move not in state['legal']:
                        raise ValueError('Illegal analysis PV')
                    state = referee.state('move ' + move)
        old, new = (analysis['candidates'][position[name + '_move']] for name in ('old', 'new'))
        return {**position, 'analysis': analysis, 'new_minus_old_cp': new['score'] - old['score'],
                'new_loss_to_comparison_best_cp': analysis['baseline']['score'] - new['score']}
    results = [result for _, result in parallel_jobs(source['positions'], task, args.workers)]
    results.sort(key=lambda p: p['sfen'])
    if digest_file(args.engine) != engine_hash:
        raise ValueError('Engine changed during analysis')
    output = {'schema_version': 1, 'engine_sha256': engine_hash, 'nodes_per_search': args.nodes,
              'positions_sha256': digest_file(args.positions), 'selection': source['selection'],
              'positions': results, 'all_pvs_legal': True,
              'new_better_by_over_150': sum(r['new_minus_old_cp'] > 150 for r in results),
              'new_worse_by_over_150': sum(r['new_minus_old_cp'] < -150 for r in results),
              'new_more_than_150_below_comparison_best': sum(r['new_loss_to_comparison_best_cp'] > 150 for r in results),
              'limitation': 'Selected training-derived positions only, same evaluator; this does not establish global move correctness'}
    write_json(args.output, output)
    print({k: v for k, v in output.items() if k != 'positions'}, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--positions', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    parser.add_argument('--nodes', type=positive, default=2000000)
    parser.add_argument('--workers', type=positive, default=4)
    run(parser.parse_args())
