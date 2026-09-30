#!/usr/bin/env python3
"""Paired book/no-book matches and separate post-book position analysis.

All searches use a fixed node budget and one thread. Games use fresh engine
processes, preserve full move history, and are adjudicated by match_referee.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import random
import re
import sqlite3
import statistics
import time

from build_book import LineProcess, atomic_write, digest_file, key, parse_search, positive

VERSION = 1


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(path, (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode())


def read_json(path):
    return json.loads(path.read_text())


def position_command(moves):
    return 'position startpos' + (' moves ' + ' '.join(moves) if moves else '')


def prepare(args):
    pools = {n: {} for n in (2, 4, 6, 8)}
    exclude_path = getattr(args, 'exclude_openings', None)
    excluded = {key(o['sfen']) for o in read_json(exclude_path)['openings'] if 'sfen' in o} if exclude_path else set()
    with sqlite3.connect(f'file:{args.work_db.resolve()}?mode=ro', uri=True) as db:
        metadata = {k: json.loads(v) for k, v in db.execute('SELECT key,value FROM metadata')}
        for game_id, moves_text, sfens_text in db.execute(
                'SELECT id,moves,sfens FROM games WHERE holdout=1 ORDER BY id'):
            moves, sfens = moves_text.split(), json.loads(sfens_text)
            if moves[0] not in ('2g2f', '7g7f', '5g5f'):
                continue
            for n, pool in pools.items():
                if n < len(sfens) and key(sfens[n]) not in excluded:
                    pool.setdefault(key(sfens[n]), {'moves': moves[:n], 'sfen': sfens[n],
                                                   'source_game': game_id})
    rng = random.Random(args.seed)
    shuffled = {}
    for n, pool in pools.items():
        shuffled[n] = [pool[k] for k in sorted(pool)]
        rng.shuffle(shuffled[n])
    openings = [{'id': 'startpos', 'group': 'startpos', 'moves': []}]
    used = set()
    while len(openings) <= args.pairs:
        added = False
        for n, pool in shuffled.items():
            while pool and key(pool[-1]['sfen']) in used:
                pool.pop()
            if pool and len(openings) <= args.pairs:
                opening = pool.pop()
                used.add(key(opening['sfen']))
                opening.update(id=f'holdout_{len(openings):03d}', group='holdout')
                openings.append(opening)
                added = True
        if not added:
            raise ValueError('Not enough distinct held-out opening positions')
    output = {'schema_version': VERSION, 'seed': args.seed, 'pairs': args.pairs,
              'source_corpus_sha256': metadata['corpus_sha256'],
              'source': 'held-out games only, first move 2g2f/7g7f/5g5f, SFEN-deduplicated',
              'sampling': 'round-robin prefix lengths 2/4/6/8, shuffled unique positions per length; no book-hit or score filter',
              'available_by_prefix_length': {str(n): len(p) for n, p in pools.items()},
              'selected_by_prefix_length': dict(Counter(str(len(p['moves'])) for p in openings[1:])),
              'openings': openings}
    if exclude_path:
        output['excluded_openings_sha256'] = digest_file(exclude_path)
        output['excluded_position_count'] = len(excluded)
    if args.output.exists() and read_json(args.output) != output:
        raise ValueError('Refusing to replace a different frozen opening suite')
    write_json(args.output, output)
    print(f'Frozen {args.pairs} opening pairs plus startpos: {args.output}', flush=True)


class Referee(LineProcess):
    def state(self, command):
        self.send(command)
        fields = self.receive().split('\t')
        if len(fields) != 6 or fields[0] != 'state':
            raise RuntimeError(f'Referee rejected {command!r}: {fields}')
        return {'side': fields[1], 'outcome': fields[2], 'reason': fields[3],
                'sfen': fields[4], 'legal': set(fields[5].split())}


class Player(LineProcess):
    def __init__(self, executable, book_path=None, analysis=False):
        super().__init__([str(executable.resolve())])
        self.send('usi')
        self.identity = [line for line in self.until('usiok') if line.startswith('id name ')]
        self.send('setoption name Threads value 1\nsetoption name Hash value 16\n'
                  'setoption name MultiPV value 1\nsetoption name USI_Ponder value false\n'
                  'setoption name USI_LimitStrength value false\n'
                  'setoption name ResignValue value 99999\n'
                  'setoption name MaxMovesToDraw value 0\n'
                  'setoption name EnteringKingRule value CSARule24\n'
                  f'setoption name USI_AnalyseMode value {str(analysis).lower()}\n'
                  f'setoption name USI_OwnBook value {str(book_path is not None).lower()}')
        if book_path:
            self.send(f'setoption name BookDir value {book_path.resolve().parent}\n'
                      f'setoption name BookFile value {book_path.name}')
        self.send('isready')
        lines = self.until('readyok')
        if book_path and not any('book loaded ' in line for line in lines):
            self.close()
            raise RuntimeError(f'Book not loaded: {lines}')
        self.send('usinewgame')

    def search(self, history, nodes):
        return self.search_command(history, f'go nodes {nodes}')

    def search_command(self, history, command, timeout=120):
        self.send(position_command(history))
        started = time.monotonic()
        self.send(command)
        lines = self.until('bestmove ', timeout=timeout)
        elapsed_ms = (time.monotonic() - started) * 1000
        scores = parse_search(lines)
        score = max(scores[max(scores)].values(), key=lambda x: x['score']) if scores else None
        return {'move': lines[-1].split()[1],
                'book_hit': any(line.startswith('info string book hit ') for line in lines),
                'score': score, 'elapsed_ms': elapsed_ms}


def book_score(outcome, side, book_side):
    if outcome == 'draw':
        return 0.5
    winner = side if outcome == 'win' else ('w' if side == 'b' else 'b')
    return float(winner == book_side)


def play_game(args, opening, nodes, book_side):
    game_id = f'{nodes}-{opening["id"]}-{book_side}'
    history = list(opening['moves'])
    events = []
    started = time.monotonic()
    with ExitStack() as stack:
        referee = stack.enter_context(Referee([str(args.referee.resolve())]))
        state = referee.state(position_command(history))
        players = {side: stack.enter_context(Player(args.engine, args.book if side == book_side else None))
                   for side in ('b', 'w')}
        while state['outcome'] == 'none' and len(history) < args.max_plies:
            side = state['side']
            result = players[side].search(history, nodes)
            if result['move'] not in state['legal']:
                raise RuntimeError(f'{game_id}: illegal move/unsupported resignation {result}, state={state}')
            if result['book_hit'] and side != book_side:
                raise RuntimeError('Control player unexpectedly used a book')
            result.update(ply=len(history) + 1, side=side,
                          player='book' if side == book_side else 'no_book')
            events.append(result)
            history.append(result['move'])
            state = referee.state('move ' + result['move'])
        if state['outcome'] == 'none':
            outcome, reason = 'draw', 'runner_ply_cap'
        else:
            outcome, reason = state['outcome'], state['reason']
        points = book_score(outcome, state['side'], book_side)
    hits = [e['ply'] for e in events if e['book_hit']]
    misses = [e['ply'] for e in events if e['player'] == 'book' and not e['book_hit']]
    first_miss = next((ply for ply in misses if hits and ply > hits[0]), None)
    exit_ply = next((ply for ply in misses if hits and ply > hits[-1]), None)
    return {'schema_version': VERSION, 'id': game_id, 'opening_id': opening['id'],
            'group': opening['group'], 'opening_moves': opening['moves'], 'nodes_per_move': nodes,
            'book_side': book_side, 'book_score': points, 'terminal_reason': reason,
            'final_sfen': state['sfen'], 'plies': len(history), 'moves': history,
            'book_hit_plies': hits, 'first_book_miss_ply': first_miss,
            'permanent_exit_ply': exit_ply,
            'elapsed_seconds': round(time.monotonic() - started, 3), 'events': events}


def parallel_jobs(jobs, function, workers):
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(function, job): job for job in jobs}
        try:
            for future in as_completed(futures):
                yield futures[future], future.result()
        except BaseException:
            for future in futures:
                future.cancel()
            raise


def frozen_manifest(path, config):
    if path.exists() and read_json(path) != config:
        raise ValueError(f'Configuration differs from frozen manifest: {path}')
    write_json(path, config)


def run_matches(args):
    suite = read_json(args.openings)
    for opening in suite['openings']:
        if not re.fullmatch(r'[a-z0-9_]+', opening['id']):
            raise ValueError('Invalid opening id')
    if len({p['id'] for p in suite['openings']}) != len(suite['openings']):
        raise ValueError('Duplicate opening ids')
    config = {'schema_version': VERSION, 'engine_sha256': digest_file(args.engine),
              'book_sha256': digest_file(args.book), 'referee_sha256': digest_file(args.referee),
              'openings_sha256': digest_file(args.openings), 'nodes_per_move': args.nodes,
              'max_plies': args.max_plies, 'threads': 1, 'hash_mb': 16,
              'entering_king_rule': 'CSARule24', 'resign_value': 99999,
              'fresh_processes_per_game': True, 'paired_color_swap': True,
              'primary_measure': 'mean book score; win=1, draw=0.5, loss=0',
              'exit_offsets_plies': [0, 4, 8], 'substantial_drop_cp': 200,
              'expected_games': len(suite['openings']) * 2 * len(args.nodes)}
    frozen_manifest(args.output / 'manifest.json', config)
    frozen_manifest(args.output / 'openings.json', suite)
    jobs = [(o, n, side) for n in args.nodes for o in suite['openings'] for side in ('b', 'w')]
    pending = [j for j in jobs if not (args.output / 'games' / f'{j[1]}-{j[0]["id"]}-{j[2]}.json').exists()]
    print(f'matches: {len(jobs)-len(pending)} cached, {len(pending)} pending', flush=True)
    for number, (_, result) in enumerate(parallel_jobs(
            pending, lambda j: play_game(args, *j), args.workers), 1):
        write_json(args.output / 'games' / (result['id'] + '.json'), result)
        print(f'match {number}/{len(pending)} {result["id"]} '
              f'book={result["book_score"]} plies={result["plies"]} '
              f'{result["terminal_reason"]}', flush=True)
    if digest_file(args.engine) != config['engine_sha256'] or digest_file(args.book) != config['book_sha256']:
        raise RuntimeError('Engine or book changed during matches')
    summarize(args.output)


def percentile(values, proportion):
    ordered = sorted(values)
    index = (len(ordered) - 1) * proportion
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def paired_interval(pair_means, seed=20260930, samples=10000):
    rng = random.Random(seed)
    estimates = [statistics.fmean(rng.choices(pair_means, k=len(pair_means))) for _ in range(samples)]
    return [percentile(estimates, 0.025), percentile(estimates, 0.975)]


def match_statistics(games):
    pair_groups = defaultdict(list)
    for g in games:
        pair_groups[g['opening_id']].append(g)
    complete = [pair for pair in pair_groups.values() if {g['book_side'] for g in pair} == {'b', 'w'} and len(pair) == 2]
    scores = [statistics.fmean(g['book_score'] for g in pair) for pair in complete]
    counts = Counter(g['book_score'] for g in games)
    exits = [g['permanent_exit_ply'] for g in games if g['permanent_exit_ply'] is not None]
    return {'games': len(games), 'complete_pairs': len(complete),
            'book_wins': counts[1.0], 'book_losses': counts[0.0], 'draws': counts[0.5],
            'score_rate': statistics.fmean(g['book_score'] for g in games) if games else None,
            'paired_bootstrap_95_percent_interval': paired_interval(scores) if len(scores) >= 2 else None,
            'by_book_color': {side: dict(Counter(str(g['book_score']) for g in games if g['book_side'] == side))
                              for side in ('b', 'w')},
            'terminal_reasons': dict(Counter(g['terminal_reason'] for g in games)),
            'games_with_book_hit': sum(bool(g['book_hit_plies']) for g in games),
            'median_book_hits': statistics.median(len(g['book_hit_plies']) for g in games) if games else None,
            'median_exit_ply': statistics.median(exits) if exits else None,
            'unique_full_game_sequences': len({tuple(g['moves']) for g in games})}


def audit_position(args, job):
    with Player(args.engine, analysis=True) as engine:
        result = engine.search(job['history'], args.nodes)
    if result['book_hit']:
        raise RuntimeError('Auditor used a book')
    return {'history': job['history'], 'analysis': result['score'],
            'bestmove': result['move'], 'elapsed_ms': result['elapsed_ms']}


def audit_key(history, engine_hash, nodes):
    return hashlib.sha256(json.dumps([VERSION, engine_hash, nodes, history]).encode()).hexdigest()


def audit(args):
    manifest = read_json(args.output / 'manifest.json')
    if digest_file(args.engine) != manifest['engine_sha256']:
        raise ValueError('Auditor must use the frozen engine binary')
    config = {'engine_sha256': manifest['engine_sha256'], 'nodes': args.nodes,
              'offsets': [0, 4, 8], 'score_perspective': 'player who has the book in the anchor game',
              'auditor': 'fresh Hayanagi process, own book disabled; not an independent stronger engine'}
    frozen_manifest(args.output / 'audit_manifest.json', config)
    games = [read_json(p) for p in sorted((args.output / 'games').glob('*.json'))]
    if len(games) != manifest['expected_games']:
        raise ValueError('Complete the matches before auditing')
    by_id = {g['id']: g for g in games}
    jobs, windows = {}, []
    for g in games:
        exit_ply = g['permanent_exit_ply']
        if exit_ply is None:
            continue
        other_side = 'w' if g['book_side'] == 'b' else 'b'
        control = by_id[f'{g["nodes_per_move"]}-{g["opening_id"]}-{other_side}']
        window = {'game_id': g['id'], 'group': g['group'], 'nodes_per_move': g['nodes_per_move'],
                  'exit_ply': exit_ply, 'book_side': g['book_side'], 'points': {}}
        for role, game in [('book', g), ('control_same_color', control)]:
            window['points'][role] = {}
            for offset in config['offsets']:
                length = exit_ply - 1 + offset
                if length >= len(game['moves']):
                    window['points'][role][str(offset)] = None  # Ended before this sample.
                    continue
                history = game['moves'][:length]
                aid = audit_key(history, manifest['engine_sha256'], args.nodes)
                jobs[aid] = {'id': aid, 'history': history}
                window['points'][role][str(offset)] = aid
        windows.append(window)
    pending = [j for aid, j in jobs.items() if not (args.output / 'audit' / (aid + '.json')).exists()]
    print(f'audit: {len(jobs)-len(pending)} cached, {len(pending)} pending', flush=True)
    for n, (job, result) in enumerate(parallel_jobs(pending, lambda j: audit_position(args, j), args.workers), 1):
        write_json(args.output / 'audit' / (job['id'] + '.json'), result)
        if n % 100 == 0 or n == len(pending):
            print(f'audit {n}/{len(pending)}', flush=True)
    for window in windows:
        for role, points in window['points'].items():
            for offset, aid in points.items():
                if aid is None:
                    continue
                record = read_json(args.output / 'audit' / (aid + '.json'))
                score = record['analysis']
                if score is None or score['score_type'] != 'cp':
                    points[offset] = {'cp': None, 'analysis': score}
                else:
                    side = 'b' if len(record['history']) % 2 == 0 else 'w'
                    value = score['score'] * (1 if side == window['book_side'] else -1)
                    points[offset] = {'cp': value, 'depth': score['depth']}
    write_json(args.output / 'exit_windows.json', windows)
    summarize(args.output)


def exit_statistics(windows):
    result = {'eligible_games': len(windows), 'drop_threshold_cp': 200, 'offsets_are_total_plies': True}
    for role in ('book', 'control_same_color'):
        points = [w['points'][role] for w in windows]
        entry = [p['0']['cp'] for p in points if p.get('0') is not None and p['0'].get('cp') is not None]
        result[role] = {'entry_cp_count': len(entry), 'entry_cp_mean': statistics.fmean(entry) if entry else None,
                        'entry_cp_median': statistics.median(entry) if entry else None,
                        'entry_below_minus_200_cp': sum(v <= -200 for v in entry)}
        for offset in ('4', '8'):
            deltas = [p[offset]['cp'] - p['0']['cp'] for p in points
                      if p.get('0') is not None and p.get(offset) is not None
                      and p['0'].get('cp') is not None and p[offset].get('cp') is not None]
            result[role][f'delta_after_{offset}_plies'] = {
                'count': len(deltas), 'mean': statistics.fmean(deltas) if deltas else None,
                'median': statistics.median(deltas) if deltas else None,
                'ended_before_sample': sum(p.get(offset) is None for p in points),
                'non_cp_or_missing_score': sum(p.get(offset) is not None and p[offset].get('cp') is None for p in points),
                'drops_at_least_200_cp': sum(d <= -200 for d in deltas),
                'drop_rate': sum(d <= -200 for d in deltas) / len(deltas) if deltas else None}
    # Matched changes are descriptive: the swapped leg has a different opponent
    # book setting and may follow a different trajectory by the sampling ply.
    paired_changes = defaultdict(list)
    for w in windows:
        b, c = w['points']['book'], w['points']['control_same_color']
        if all(p.get(o) is not None and p[o].get('cp') is not None
               for p in (b, c) for o in ('0', '8')):
            delta = (b['8']['cp'] - b['0']['cp']) - (c['8']['cp'] - c['0']['cp'])
            paired_changes[w['game_id'].rsplit('-', 1)[0]].append(delta)
    opening_means = [statistics.fmean(values) for values in paired_changes.values()]
    result['matched_delta_difference_after_8_plies'] = {
        'openings': len(opening_means), 'positive_means_book_deteriorates_less': True,
        'mean_cp_per_opening': statistics.fmean(opening_means) if opening_means else None,
        'paired_bootstrap_95_percent_interval': paired_interval(opening_means) if len(opening_means) > 1 else None}
    worst = []
    for w in windows:
        points = w['points']['book']
        if all(points.get(o) is not None and points[o].get('cp') is not None for o in ('0', '8')):
            worst.append({'game_id': w['game_id'], 'exit_ply': w['exit_ply'],
                          'entry_cp': points['0']['cp'], 'after_8_plies_cp': points['8']['cp'],
                          'delta_cp': points['8']['cp'] - points['0']['cp']})
    result['worst_book_drops'] = sorted(worst, key=lambda x: (x['delta_cp'], x['game_id']))[:10]
    return result


def summarize(output):
    manifest = read_json(output / 'manifest.json')
    games = [read_json(p) for p in sorted((output / 'games').glob('*.json'))]
    windows = read_json(output / 'exit_windows.json') if (output / 'exit_windows.json').exists() else None
    result = {'schema_version': VERSION, 'manifest': manifest, 'completed_games': len(games),
              'complete': len(games) == manifest['expected_games'], 'conditions': {},
              'interval_method': {'method': 'percentile bootstrap over color-swapped opening pairs',
                                  'confidence': 0.95, 'resamples': 10000, 'seed': 20260930},
              'limitations': [
                  'Conditional on this fixed held-out opening suite and same-engine opposition',
                  'Prefix lengths are balanced and distinct positions sampled uniformly; this is not natural opening-frequency weighting',
                  'Two color-swapped games are resampled together; related opening families may still correlate',
                  'Startpos is deterministic and reported separately, not counted as repeated independent samples',
                  'Fixed nodes measure move choice; saved book time is not transferred to later moves',
                  'Ply-cap draws are artificial and listed separately',
                  'Post-exit evaluation uses Hayanagi itself and excludes mate/missing scores from CP averages',
                  'Bootstrap intervals are per condition, without multiplicity correction']}
    for nodes in manifest['nodes_per_move']:
        group = [g for g in games if g['nodes_per_move'] == nodes]
        condition = {name: match_statistics([g for g in group if g['group'] == name])
                     for name in ('holdout', 'startpos')}
        if windows is not None:
            condition['post_exit'] = exit_statistics([w for w in windows if w['nodes_per_move'] == nodes and w['group'] == 'holdout'])
        result['conditions'][str(nodes)] = condition
    write_json(output / 'summary.json', result)
    print(json.dumps({n: r['holdout'] for n, r in result['conditions'].items()}, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prep = commands.add_parser('prepare')
    prep.add_argument('--work-db', type=Path, required=True)
    prep.add_argument('--output', type=Path, required=True)
    prep.add_argument('--pairs', type=positive, default=100)
    prep.add_argument('--seed', type=int, default=20260930)
    prep.add_argument('--exclude-openings', type=Path,
                      help='Exclude SFENs from a previous suite before sampling')
    run = commands.add_parser('run')
    run.add_argument('--engine', type=Path, default=Path('build/hayanagi'))
    run.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    run.add_argument('--book', type=Path, default=Path('book/hayanagi_book.db'))
    run.add_argument('--openings', type=Path, required=True)
    run.add_argument('--output', type=Path, required=True)
    run.add_argument('--nodes', type=positive, nargs='+', default=[10000, 50000])
    run.add_argument('--max-plies', type=positive, default=320)
    run.add_argument('--workers', type=positive, default=8)
    check = commands.add_parser('audit')
    check.add_argument('--engine', type=Path, default=Path('build/hayanagi'))
    check.add_argument('--output', type=Path, required=True)
    check.add_argument('--nodes', type=positive, default=200000)
    check.add_argument('--workers', type=positive, default=8)
    summary = commands.add_parser('summarize')
    summary.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare(args)
    elif args.command == 'run':
        if len(set(args.nodes)) != len(args.nodes):
            parser.error('Duplicate node budgets')
        run_matches(args)
    elif args.command == 'audit':
        audit(args)
    else:
        summarize(args.output)


if __name__ == '__main__':
    main()
