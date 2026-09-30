#!/usr/bin/env python3
"""３条件比較を再生し、時計・定跡照合・先後交換・終局・集計を確認する。"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import zipfile

from build_book import digest_file, key, positive
from compare_books import PAIRS, charge_clock, jobs_for, statistics_for
from match_book import Referee, book_score, parallel_jobs, position_command, read_json, write_json


def read_book(path):
    entries, current, max_ply = {}, None, 0
    for line in path.read_text().splitlines():
        if line.startswith('# HAYANAGI_MAX_PLY '):
            max_ply = int(line.split()[-1])
        elif line.startswith('sfen '):
            current = key(line[5:])
            entries.setdefault(current, [])
        elif line and not line.startswith('#'):
            fields = line.split()
            if current is None or len(fields) != 5:
                raise ValueError(f'Invalid book record: {line!r}')
            entries[current].append(fields[0])
    return {'entries': entries, 'max_ply': max_ply}


def verify_holdout(suite, work_db):
    """元棋譜単位の学習除外と、過去の検証集合との重複を確認する。"""
    excluded_positions, excluded_games = set(), set()
    root = Path(__file__).resolve().parents[1]
    for filename, sha in suite['exclusions'].items():
        path = root / filename
        if digest_file(path) != sha:
            raise ValueError('Previous opening suite hash differs')
        for opening in read_json(path)['openings']:
            if 'sfen' in opening: excluded_positions.add(key(opening['sfen']))
            if 'source_game' in opening: excluded_games.add(opening['source_game'])
    seen_positions, seen_games = set(), set()
    with sqlite3.connect(f'file:{work_db.resolve()}?mode=ro', uri=True) as db:
        for opening in suite['openings']:
            if opening['group'] != 'holdout':
                continue
            k, source = key(opening['sfen']), opening['source_game']
            if k in seen_positions | excluded_positions or source in seen_games | excluded_games:
                raise ValueError('Holdout position/source overlaps')
            row = db.execute('SELECT holdout,moves,sfens FROM games WHERE id=?', (source,)).fetchone()
            if row is None or row[0] != 1 or row[1].split()[:len(opening['moves'])] != opening['moves']:
                raise ValueError('Opening not in held-out source games')
            if json.loads(row[2])[len(opening['moves'])] != opening['sfen']:
                raise ValueError('Opening differs from source SFEN')
            seen_positions.add(k)
            seen_games.add(source)
        corpus = json.loads(db.execute("SELECT value FROM metadata WHERE key='corpus_sha256'").fetchone()[0])
    return {'success': True, 'unique_holdout_positions': len(seen_positions),
            'unique_holdout_source_games': len(seen_games), 'training_games_used': 0,
            'overlap_with_previous_suites_by_sfen_or_source_game': 0, 'corpus_sha256': corpus}


def novelty_report(args, suite, games):
    """以前の解析の根局面まで照合し、重複しない局面の補足集計を作る。"""
    def histories(value):
        if isinstance(value, list):
            for row in value:
                yield from histories(row)
        elif isinstance(value, dict):
            if isinstance(value.get('history'), list):
                yield tuple(value['history'])
            if 'request' in value:
                yield from histories(value['request'])
            if isinstance(value.get('positions'), list):
                yield from histories(value['positions'])
    prior, fingerprint = set(), []
    max_prefix = max(len(o['moves']) for o in suite['openings'])
    for source in args.prior_analysis:
        files = sorted(source.glob('*.json')) if source.is_dir() else [source]
        for path in files:
            data = path.read_bytes()
            fingerprint.append(f'{path}:{hashlib.sha256(data).hexdigest()}')
            prior.update(histories(json.loads(data)))
    analyzed = set()
    with Referee([str(args.referee.resolve())]) as referee:
        for history in sorted(prior):
            analyzed.add(key(referee.state(position_command(history))['sfen']))
    overlap = sorted(o['id'] for o in suite['openings'] if o['group'] == 'holdout' and key(o['sfen']) in analyzed)
    eligible = sorted(o['id'] for o in suite['openings'] if o['group'] == 'holdout' and o['id'] not in overlap)
    comparisons = {}
    for mode in ('nodes', 'clock'):
        for a, b in PAIRS:
            comparisons[f'{mode}:{a}:{b}'] = statistics_for([
                g for g in games if (g['mode'], g['a'], g['b']) == (mode, a, b) and g['opening_id'] in eligible])
    return {'analysis_sources': [str(p) for p in args.prior_analysis],
            'source_files_fingerprint_sha256': hashlib.sha256('\n'.join(fingerprint).encode()).hexdigest(),
            'analyzed_root_positions': len(analyzed), 'suite_max_prefix': max_prefix,
            'overlapping_ids': overlap, 'eligible_ids': eligible, 'eligible_pair_count': len(eligible),
            'comparisons': comparisons,
            'limitations': ['Supplementary subset audit; not a replacement for the prespecified full-suite results',
                           'Exclusion depends only on prior analysis roots, never match outcomes',
                           'This excludes saved analysis roots, not internal search nodes or common positions in the training corpus']}


def verify(args, game, job, manifest, books):
    def require(condition, message):
        if not condition:
            raise ValueError(f'{job["id"]}: {message}')

    opening = job['opening']
    for name in ('id', 'mode', 'a', 'b', 'a_side', 'nodes'):
        require(game[name] == job[name], f'wrong {name}')
    require(game['opening_id'] == opening['id'] and game['group'] == opening['group'], 'wrong opening/group')
    require(game['opening_moves'] == opening['moves'], 'wrong opening moves')
    moves, remaining = list(opening['moves']), {'b': manifest['main_ms'], 'w': manifest['main_ms']}
    expired, missing, hit_count = False, Counter(), Counter()
    with Referee([str(args.referee.resolve())]) as referee:
        state = referee.state(position_command(moves))
        if 'sfen' in opening:
            require(state['sfen'] == opening['sfen'], 'wrong opening SFEN')
        for number, event in enumerate(game['events']):
            side = state['side']
            variant = game['a'] if side == game['a_side'] else game['b']
            require(state['outcome'] == 'none' and len(moves) < manifest['max_plies'], 'event after terminal/cap')
            require(event['side'] == side and event['variant'] == variant and
                    event['ply'] == len(moves) + 1, 'wrong event identity')
            require(math.isfinite(event['elapsed_ms']) and event['elapsed_ms'] >= 0, 'invalid elapsed time')
            if game['mode'] == 'clock':
                command = f'go btime {remaining["b"]} wtime {remaining["w"]} byoyomi {manifest["byoyomi_ms"]}'
                require(event['clock_before_ms'] == remaining, 'wrong clock before move')
                remaining[side], charged, expired = charge_clock(remaining[side], event['elapsed_ms'], manifest['byoyomi_ms'])
                require(event['clock_after_ms'] == remaining and event['charged_ms'] == charged, 'wrong clock charge')
            else:
                command = f'go nodes {manifest["nodes"]}'
            require(event['command'] == command, 'wrong search command')
            require(event['accepted'] == (not expired), 'wrong acceptance/timeout')
            if expired:
                require(number == len(game['events']) - 1, 'event after timeout')
                break
            book = books.get(variant)
            choices = (book['entries'].get(key(state['sfen']), []) if book and
                       (not book['max_ply'] or event['ply'] <= book['max_ply']) else [])
            expected_move = next((m for m in choices if m in state['legal']), None)
            require(event['book_hit'] == (expected_move is not None), 'incorrect book hit')
            if expected_move:
                require(event['move'] == expected_move, 'incorrect book move/order')
                hit_count[variant] += 1
            elif event['score'] is None:
                missing[variant] += 1
            require(event['move'] in state['legal'], 'illegal move')
            moves.append(event['move'])
            state = referee.state('move ' + event['move'])
        outcome, reason = state['outcome'], state['reason']
        if expired:
            outcome, reason = 'loss', 'time_forfeit'
        elif outcome == 'none':
            require(len(moves) == manifest['max_plies'], 'unfinished game')
            outcome, reason = 'draw', 'runner_ply_cap'
        require(game['remaining_ms'] == (remaining if game['mode'] == 'clock' else None), 'wrong final clock')
        require(game['moves'] == moves and game['plies'] == len(moves), 'wrong full moves/plies')
        require(game['final_sfen'] == state['sfen'] and game['terminal_reason'] == reason, 'wrong final position/reason')
        require(game['a_score'] == book_score(outcome, state['side'], game['a_side']), 'wrong result')
    return {'plies': len(moves), 'reason': reason, 'missing': missing, 'hits': hit_count}


def run(args):
    manifest = read_json(args.output / 'manifest.json')
    for name in ('referee', 'current', 'candidate'):
        if digest_file(getattr(args, name)) != manifest[name + '_sha256']:
            raise ValueError(f'{name} differs from frozen manifest')
    openings = args.output / 'openings.json'
    if digest_file(openings) != manifest['openings_sha256']:
        raise ValueError('Opening suite differs from frozen manifest')
    jobs = jobs_for(read_json(openings), manifest['nodes'], manifest['clock_pairs'])
    expected = {j['id'] for j in jobs}
    paths = sorted((args.output / 'games').glob('*.json'))
    if paths:
        payloads = {p.name: p.read_bytes() for p in paths}
    else:
        with zipfile.ZipFile(args.output / 'games.zip') as archive:
            names = archive.namelist()
            if len(names) != len(set(names)) or any(Path(name).name != name or not name.endswith('.json') for name in names):
                raise ValueError('Invalid or duplicate names in game archive')
            payloads = {name: archive.read(name) for name in names}
    actual = {Path(name).stem for name in payloads}
    if expected != actual or len(jobs) != manifest['expected_games']:
        raise ValueError('Missing or extra games')
    books = {name: read_book(getattr(args, name)) for name in ('current', 'candidate')}
    loaded = [(json.loads(payloads[j['id'] + '.json']), j) for j in jobs]
    reasons, missing, hits, plies = Counter(), Counter(), Counter(), 0
    for _, result in parallel_jobs(loaded, lambda pair: verify(args, *pair, manifest, books), args.workers):
        plies += result['plies']
        reasons[result['reason']] += 1
        missing.update(result['missing'])
        hits.update(result['hits'])
    summary = read_json(args.output / 'summary.json')
    if summary['manifest'] != manifest or summary['completed_games'] != len(jobs) or not summary['complete']:
        raise ValueError('Incorrect summary manifest/count')
    for mode in ('nodes', 'clock'):
        for a, b in PAIRS:
            for group in ('holdout', 'startpos'):
                games = [g for g, _ in loaded if (g['mode'], g['a'], g['b'], g['group']) == (mode, a, b, group)]
                if summary['comparisons'][f'{mode}:{a}:{b}'][group] != statistics_for(games):
                    raise ValueError('Incorrect summary statistics')
    fingerprint = '\n'.join(f'{name}:{hashlib.sha256(data).hexdigest()}' for name, data in sorted(payloads.items()))
    result = {'success': True, 'games': len(jobs), 'replayed_plies': plies,
              'terminal_reasons': dict(reasons), 'ordinary_moves_without_score': dict(missing),
              'verified_book_hits': dict(hits), 'clocks_and_pairings_verified': True,
              'manifest_sha256': digest_file(args.output / 'manifest.json'),
              'game_files_fingerprint_sha256': hashlib.sha256(fingerprint.encode()).hexdigest(),
              'referee_sha256': digest_file(args.referee),
              'limitation': 'Legal replay uses Hayanagi Position; real elapsed time is checked from saved records, not independently remeasured'}
    if getattr(args, 'archive_games', False):
        temporary = args.output / 'games.zip.tmp'
        with zipfile.ZipFile(temporary, 'w') as archive:
            for name, data in sorted(payloads.items()):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        temporary.replace(args.output / 'games.zip')
    if not paths or getattr(args, 'archive_games', False):
        result['games_archive_sha256'] = digest_file(args.output / 'games.zip')
    write_json(args.output / 'validation.json', result)
    if getattr(args, 'work_db', None):
        write_json(args.output / 'openings.validation.json', verify_holdout(read_json(openings), args.work_db))
    if getattr(args, 'prior_analysis', None):
        write_json(args.output / 'novelty.json', novelty_report(args, read_json(openings), [g for g, _ in loaded]))
    print(result, flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    parser.add_argument('--workers', type=positive, default=4)
    parser.add_argument('--work-db', type=Path, help='任意: 元棋譜のholdout属性と過去集合からの分離も確認する')
    parser.add_argument('--prior-analysis', type=Path, nargs='+', help='任意: 保存済み解析の根局面との重複を確認し補足集計する')
    parser.add_argument('--archive-games', action='store_true', help='検証に通った棋譜を再現可能なZIPにも保存する')
    run(parser.parse_args())
