#!/usr/bin/env python3
"""Compare two engine binaries at identical node budgets, with books disabled."""
import argparse
from collections import Counter, defaultdict
from contextlib import ExitStack
from pathlib import Path
import re
import statistics

from build_book import digest_file, positive
from match_book import (Player, Referee, book_score, frozen_manifest, paired_interval,
                        parallel_jobs, position_command, read_json, write_json)


def play(args, opening, nodes, side):
    history, events = list(opening['moves']), []
    with ExitStack() as stack:
        referee = stack.enter_context(Referee([str(args.referee.resolve())]))
        state = referee.state(position_command(history))
        players = {color: stack.enter_context(Player(args.engine if color == side else args.baseline))
                   for color in ('b', 'w')}
        while state['outcome'] == 'none' and len(history) < args.max_plies:
            color = state['side']
            result = players[color].search(history, nodes)
            if result['book_hit'] or result['move'] not in state['legal']:
                raise RuntimeError(f'Illegal move or unintended book use: {result}')
            result.update(ply=len(history) + 1, side=color,
                          engine='candidate' if color == side else 'baseline')
            events.append(result)
            history.append(result['move'])
            state = referee.state('move ' + result['move'])
        outcome = state['outcome'] if state['outcome'] != 'none' else 'draw'
        reason = state['reason'] if state['outcome'] != 'none' else 'runner_ply_cap'
        score = book_score(outcome, state['side'], side)
    return {'id': f'{nodes}-{opening["id"]}-{side}', 'opening_id': opening['id'],
            'group': opening['group'], 'opening_moves': opening['moves'],
            'candidate_side': side, 'candidate_score': score, 'nodes_per_move': nodes,
            'plies': len(history), 'moves': history, 'events': events,
            'terminal_reason': reason, 'final_sfen': state['sfen']}


def summarize(games):
    counts = Counter(g['candidate_score'] for g in games)
    pairs = defaultdict(list)
    for g in games:
        pairs[g['opening_id']].append(g)
    if any(len(p) != 2 or {g['candidate_side'] for g in p} != {'b', 'w'} for p in pairs.values()):
        raise ValueError('Incomplete color-swapped pair')
    values = [statistics.fmean(g['candidate_score'] for g in p) for p in pairs.values()]
    return {'games': len(games), 'pairs': len(pairs), 'wins': counts[1], 'losses': counts[0],
            'draws': counts[0.5], 'score_rate': statistics.fmean(values) if values else None,
            'paired_bootstrap_95_percent_interval': paired_interval(values) if len(values) > 1 else None,
            'terminal_reasons': dict(Counter(g['terminal_reason'] for g in games))}


def run(args):
    suite = read_json(args.openings)
    if len({o['id'] for o in suite['openings']}) != len(suite['openings']):
        raise ValueError('Duplicate opening ID')
    if any(not re.fullmatch(r'[a-z0-9_]+', o['id']) for o in suite['openings']):
        raise ValueError('Invalid opening ID')
    hashes = {name + '_sha256': digest_file(getattr(args, name))
              for name in ('engine', 'baseline', 'referee', 'openings')}
    jobs = [(o, n, s) for n in args.nodes for o in suite['openings'] for s in ('b', 'w')]
    manifest = dict(hashes, schema_version=1, nodes_per_move=args.nodes, threads=1, hash_mb=16,
                    books=False, max_plies=args.max_plies, entering_king_rule='CSARule24',
                    resign_value=99999, expected_games=len(jobs), paired_color_swap=True)
    frozen_manifest(args.output / 'manifest.json', manifest)
    frozen_manifest(args.output / 'openings.json', suite)
    pending = [j for j in jobs if not (args.output / 'games' / f'{j[1]}-{j[0]["id"]}-{j[2]}.json').exists()]
    print(f'engine matches: {len(jobs)-len(pending)} cached, {len(pending)} pending', flush=True)
    for i, (_, result) in enumerate(parallel_jobs(pending, lambda j: play(args, *j), args.workers), 1):
        write_json(args.output / 'games' / (result['id'] + '.json'), result)
        print(f'{i}/{len(pending)} {result["id"]} candidate={result["candidate_score"]}', flush=True)
    for name in ('engine', 'baseline', 'referee', 'openings'):
        if digest_file(getattr(args, name)) != hashes[name + '_sha256']:
            raise RuntimeError(f'{name} changed during matches')
    games = [read_json(args.output / 'games' / f'{n}-{o["id"]}-{s}.json') for o, n, s in jobs]
    result = {'manifest': manifest, 'complete': True, 'conditions': {}, 'limitations': [
        'Fixed-node comparison against this baseline only; no external rating calibration',
        'Opening pairs are bootstrap units; related opening families can still correlate',
        'Startpos is reported separately; ply-cap draws are artificial',
        'Intervals are per condition, without multiplicity correction']}
    for n in args.nodes:
        result['conditions'][str(n)] = {group: summarize([g for g in games if g['nodes_per_move'] == n and g['group'] == group])
                                        for group in ('holdout', 'startpos')}
    write_json(args.output / 'summary.json', result)
    print(result['conditions'], flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    parser.add_argument('--openings', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nodes', type=positive, nargs='+', default=[10000, 50000])
    parser.add_argument('--max-plies', type=positive, default=320)
    parser.add_argument('--workers', type=positive, default=8)
    args = parser.parse_args()
    if len(set(args.nodes)) != len(args.nodes):
        parser.error('Duplicate node budgets')
    run(args)
