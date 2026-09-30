#!/usr/bin/env python3
"""現行定跡・候補定跡・定跡なしをノード制／持ち時間制で先後交換比較する。"""
import argparse
from collections import Counter, defaultdict
from contextlib import ExitStack
import itertools
import math
import os
from pathlib import Path
import platform
import queue
import random
import re
import statistics
import time

from build_book import atomic_write, digest_file, positive
from match_book import (Player, Referee, book_score, frozen_manifest, paired_interval,
                        parallel_jobs, position_command, read_json, write_json)

VARIANTS = ('current', 'candidate', 'none')
PAIRS = tuple(itertools.combinations(VARIANTS, 2))


def available_cores():
    """Linuxでは同じ物理コアのSMTを重ねずに割り当てる。"""
    if not hasattr(os, 'sched_getaffinity'):
        return []
    seen, cores = set(), []
    for cpu in sorted(os.sched_getaffinity(0)):
        root = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology')
        try:
            identity = ((root / 'physical_package_id').read_text(), (root / 'core_id').read_text())
        except OSError:
            identity = (cpu,)
        if identity not in seen:
            seen.add(identity)
            cores.append(cpu)
    return cores


def charge_clock(remaining, elapsed_ms, byoyomi):
    spent = math.ceil(elapsed_ms)
    return max(0, remaining - spent), spent, spent > remaining + byoyomi


def jobs_for(suite, nodes, clock_pairs):
    jobs = []
    identifiers = [o['id'] for o in suite['openings']]
    if (len(set(identifiers)) != len(identifiers) or
            any(not re.fullmatch(r'[A-Za-z0-9_]+', name) for name in identifiers)):
        raise ValueError('Opening IDs must be unique, safe filename components')
    if any(o['group'] not in ('startpos', 'holdout') for o in suite['openings']):
        raise ValueError('Unknown opening group')
    clock_ids = {o['id'] for o in suite['openings'] if o['group'] == 'holdout'}
    clock_ids = set(sorted(clock_ids)[:clock_pairs])
    for mode in ('nodes', 'clock'):
        for opening in suite['openings']:
            if mode == 'clock' and opening['group'] != 'startpos' and opening['id'] not in clock_ids:
                continue
            for a, b in PAIRS:
                for side in ('b', 'w'):
                    jobs.append({'id': f'{mode}-{a}-{b}-{opening["id"]}-{side}',
                                 'mode': mode, 'opening': opening, 'a': a, 'b': b, 'a_side': side,
                                 'nodes': nodes if mode == 'nodes' else None})
    return jobs


def play(args, job, cpu):
    opening = job['opening']
    history, events = list(opening['moves']), []
    remaining = {'b': args.main_ms, 'w': args.main_ms}
    paths = {'current': args.current, 'candidate': args.candidate, 'none': None}
    colors = {s: job['a'] if s == job['a_side'] else job['b'] for s in ('b', 'w')}
    timeout_side = None
    with ExitStack() as stack:
        referee = stack.enter_context(Referee([str(args.referee.resolve())]))
        state = referee.state(position_command(history))
        players = {s: stack.enter_context(Player(args.engine, paths[colors[s]])) for s in ('b', 'w')}
        for player in players.values():
            if cpu is not None:
                os.sched_setaffinity(player.process.pid, {cpu})
            # 秒読みから通信・時間確認の余裕を引き、実消費時間自体は全て課金する。
            player.send(f'setoption name NetworkDelay value {args.margin_ms}\n'
                        f'setoption name NetworkDelay2 value {args.margin_ms}\n'
                        'setoption name MinimumThinkingTime value 0')
            player.send('isready')
            player.until('readyok')
        while state['outcome'] == 'none' and len(history) < args.max_plies:
            side = state['side']
            before = dict(remaining)
            command = f'go nodes {args.nodes}' if job['mode'] == 'nodes' else (
                f'go btime {remaining["b"]} wtime {remaining["w"]} byoyomi {args.byoyomi_ms}')
            deadline = (remaining[side] + args.byoyomi_ms) / 1000 + 1
            started = time.monotonic()
            try:
                result = players[side].search_command(history, command,
                                                      timeout=120 if job['mode'] == 'nodes' else deadline)
            except RuntimeError as error:
                if job['mode'] != 'clock' or 'timed out' not in str(error).lower():
                    raise
                result = {'move': None, 'book_hit': False, 'score': None,
                          'elapsed_ms': (time.monotonic() - started) * 1000, 'watchdog': True}
            result.update(ply=len(history) + 1, side=side, variant=colors[side], command=command)
            if job['mode'] == 'clock':
                remaining[side], spent, expired = charge_clock(remaining[side], result['elapsed_ms'], args.byoyomi_ms)
                result.update(clock_before_ms=before, clock_after_ms=dict(remaining), charged_ms=spent)
                if expired:
                    result['accepted'] = False
                    events.append(result)
                    timeout_side = side
                    break
            if result['move'] not in state['legal'] or (colors[side] == 'none' and result['book_hit']):
                raise RuntimeError(f'{job["id"]}: illegal move/unexpected book {result}')
            result['accepted'] = True
            events.append(result)
            history.append(result['move'])
            state = referee.state('move ' + result['move'])
        if timeout_side:
            outcome, reason, final_side = 'loss', 'time_forfeit', timeout_side
        elif state['outcome'] == 'none':
            outcome, reason, final_side = 'draw', 'runner_ply_cap', state['side']
        else:
            outcome, reason, final_side = state['outcome'], state['reason'], state['side']
        points = book_score(outcome, final_side, job['a_side'])
    return {'schema_version': 1, **{k: v for k, v in job.items() if k != 'opening'},
            'opening_id': opening['id'], 'group': opening['group'], 'opening_moves': opening['moves'],
            'a_score': points, 'plies': len(history), 'moves': history, 'events': events,
            'terminal_reason': reason, 'final_sfen': state['sfen'], 'cpu': cpu,
            'remaining_ms': remaining if job['mode'] == 'clock' else None}


def statistics_for(games):
    games = sorted(games, key=lambda game: game['id'])
    pairs = defaultdict(list)
    for game in games:
        pairs[game['opening_id']].append(game)
    complete = [p for p in pairs.values() if len(p) == 2 and {g['a_side'] for g in p} == {'b', 'w'}]
    pair_scores = [statistics.fmean(g['a_score'] for g in p) for p in complete]
    counts = Counter(g['a_score'] for g in games)
    result = {'games': len(games), 'complete_pairs': len(complete), 'a_wins': counts[1],
              'a_losses': counts[0], 'draws': counts[0.5],
              'a_score_rate': statistics.fmean(pair_scores) if pair_scores else None,
              'paired_bootstrap_95_percent_interval': paired_interval(pair_scores) if len(pair_scores) > 1 else None,
              'terminal_reasons': dict(Counter(g['terminal_reason'] for g in games)), 'variants': {}}
    for variant in sorted({e['variant'] for g in games for e in g['events']}):
        events = [e for g in games for e in g['events'] if e['variant'] == variant and e['accepted']]
        hits = [e for e in events if e['book_hit']]
        misses = [e for e in events if not e['book_hit']]
        exits, clocks = [], []
        for g in games:
            own = [e for e in g['events'] if e['variant'] == variant and e['accepted']]
            book = [e for e in own if e['book_hit']]
            after = next((e for e in own if book and not e['book_hit'] and e['ply'] > book[-1]['ply']), None)
            if after:
                exits.append(after['ply'])
            sample = next((e for e in own if e['ply'] >= 30 and 'clock_after_ms' in e), None)
            if sample:
                clocks.append(sample['clock_after_ms'][sample['side']])
        result['variants'][variant] = {
            'moves': len(events), 'book_hits': len(hits), 'last_hit_then_miss_games': len(exits),
            'median_exit_ply': statistics.median(exits) if exits else None,
            'mean_book_move_ms': statistics.fmean(e['elapsed_ms'] for e in hits) if hits else None,
            'mean_search_move_ms': statistics.fmean(e['elapsed_ms'] for e in misses) if misses else None,
            'total_book_ms': sum(e['elapsed_ms'] for e in hits),
            'total_search_ms': sum(e['elapsed_ms'] for e in misses),
            'median_remaining_ms_at_own_move_on_or_after_ply30': statistics.median(clocks) if clocks else None}
    return result


def summarize(output):
    manifest = read_json(output / 'manifest.json')
    games = [read_json(p) for p in sorted((output / 'games').glob('*.json'))]
    result = {'manifest': manifest, 'completed_games': len(games),
              'complete': len(games) == manifest['expected_games'], 'comparisons': {},
              'limitations': ['Same engine and evaluator; no external rating calibration',
                  'Intervals resample color-swapped opening pairs; related opening families may correlate',
                  'Clock games are not deterministic; OS scheduling and other system load remain',
                  'Clock consumption includes IPC latency, rounded up to milliseconds',
                  'Ply-cap draws and time forfeits are listed separately',
                  'Remaining clock comparisons follow different trajectories; not a causal estimate of saved time',
                  'Per-comparison intervals are not multiplicity-adjusted']}
    for mode in ('nodes', 'clock'):
        for a, b in PAIRS:
            result['comparisons'][f'{mode}:{a}:{b}'] = {
                group: statistics_for([g for g in games if (g['mode'], g['a'], g['b'], g['group']) == (mode, a, b, group)])
                for group in ('holdout', 'startpos')}
    write_json(output / 'summary.json', result)
    return result


def run(args):
    suite = read_json(args.openings)
    jobs = jobs_for(suite, args.nodes, args.clock_pairs)
    if len({j['id'] for j in jobs}) != len(jobs):
        raise ValueError('Duplicate opening IDs')
    cores = available_cores()
    if args.clock_workers > len(cores) and cores:
        raise ValueError('Clock workers exceed available physical cores')
    hashes = {name + '_sha256': digest_file(getattr(args, name))
              for name in ('engine', 'referee', 'current', 'candidate', 'openings')}
    manifest = dict(hashes, schema_version=1, nodes=args.nodes, main_ms=args.main_ms,
                    byoyomi_ms=args.byoyomi_ms, margin_ms=args.margin_ms, clock_pairs=args.clock_pairs,
                    max_plies=args.max_plies, threads=1, hash_mb=16, expected_games=len(jobs),
                    entering_king_rule='CSARule24', node_workers=args.node_workers,
                    clock_workers=args.clock_workers, cpu_affinity_cores=cores,
                    platform=platform.system(), machine=platform.machine(), job_order_seed=20261001,
                    runner_sha256=digest_file(Path(__file__)),
                    player_sha256=digest_file(Path(__file__).with_name('match_book.py')))
    frozen_manifest(args.output / 'manifest.json', manifest)
    frozen_openings = args.output / 'openings.json'
    if frozen_openings.exists() and frozen_openings.read_bytes() != args.openings.read_bytes():
        raise ValueError('Frozen opening file differs')
    if not frozen_openings.exists():
        atomic_write(frozen_openings, args.openings.read_bytes())
    for mode in args.modes:
        pending = [j for j in jobs if j['mode'] == mode and not (args.output / 'games' / (j['id'] + '.json')).exists()]
        random.Random(20261001).shuffle(pending)
        workers = args.node_workers if mode == 'nodes' else args.clock_workers
        resources = queue.Queue()
        for i in range(workers): resources.put(cores[i % len(cores)] if cores else None)
        def task(job):
            cpu = resources.get()
            try:
                return play(args, job, cpu)
            finally:
                resources.put(cpu)
        print(f'{mode}: {len(pending)} pending', flush=True)
        for number, (_, result) in enumerate(parallel_jobs(pending, task, workers), 1):
            write_json(args.output / 'games' / (result['id'] + '.json'), result)
            print(f'{mode} {number}/{len(pending)} {result["id"]} a_score={result["a_score"]} {result["terminal_reason"]}', flush=True)
    for name in ('engine', 'referee', 'current', 'candidate', 'openings'):
        if hashes[name + '_sha256'] != digest_file(getattr(args, name)):
            raise RuntimeError(f'{name} changed during comparison')
    result = summarize(args.output)
    print('completed games:', result['completed_games'], 'complete:', result['complete'], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, default=Path('build/hayanagi'))
    parser.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--openings', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nodes', type=positive, default=50000)
    parser.add_argument('--main-ms', type=positive, default=5000)
    parser.add_argument('--byoyomi-ms', type=positive, default=100)
    parser.add_argument('--margin-ms', type=positive, default=10)
    parser.add_argument('--clock-pairs', type=positive, default=40)
    parser.add_argument('--max-plies', type=positive, default=320)
    parser.add_argument('--node-workers', type=positive, default=8)
    parser.add_argument('--clock-workers', type=positive, default=4)
    parser.add_argument('--modes', nargs='+', choices=('nodes', 'clock'), default=['nodes', 'clock'])
    args = parser.parse_args()
    if args.margin_ms >= args.byoyomi_ms:
        parser.error('Clock margin must be smaller than byoyomi')
    run(args)


if __name__ == '__main__':
    main()
