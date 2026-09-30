#!/usr/bin/env python3
"""Replay saved book/engine matches and verify pairing, adjudication and scores."""
import argparse
from collections import Counter
from pathlib import Path

from build_book import digest_file, positive
from match_book import (Referee, book_score, parallel_jobs, position_command,
                        read_json, write_json)


def verify(args, game, opening, manifest):
    moves = game['moves']
    prefix = opening['moves']
    if game['opening_moves'] != prefix or moves[:len(prefix)] != prefix:
        raise ValueError(f'{game["id"]}: wrong opening')
    if game['group'] != opening['group'] or game['plies'] != len(moves):
        raise ValueError(f'{game["id"]}: wrong group/ply count')
    if len(game['events']) != len(moves) - len(prefix):
        raise ValueError(f'{game["id"]}: event count mismatch')
    side = game.get('candidate_side', game.get('book_side'))
    missing = Counter()
    with Referee([str(args.referee.resolve())]) as ref:
        state = ref.state(position_command(prefix))
        for ply, event in enumerate(game['events'], len(prefix) + 1):
            if state['outcome'] != 'none' or event['move'] not in state['legal']:
                raise ValueError(f'{game["id"]}: move after terminal state or illegal move at {ply}')
            if event['ply'] != ply or event['side'] != state['side'] or event['move'] != moves[ply - 1]:
                raise ValueError(f'{game["id"]}: event mismatch at {ply}')
            if 'candidate_side' in game:
                expected = 'candidate' if event['side'] == side else 'baseline'
                if event['engine'] != expected or event['book_hit']:
                    raise ValueError(f'{game["id"]}: player/book mismatch')
            else:
                expected = 'book' if event['side'] == side else 'no_book'
                if event['player'] != expected or (event['book_hit'] and expected != 'book'):
                    raise ValueError(f'{game["id"]}: player/book mismatch')
            if not event['book_hit'] and event['score'] is None:
                missing[f'{game["nodes_per_move"]}:{expected}'] += 1
            state = ref.state('move ' + event['move'])
        outcome, reason = state['outcome'], state['reason']
        if outcome == 'none':
            if len(moves) != manifest['max_plies']:
                raise ValueError(f'{game["id"]}: unfinished game')
            outcome, reason = 'draw', 'runner_ply_cap'
        if len(moves) > manifest['max_plies']:
            raise ValueError(f'{game["id"]}: exceeded ply cap')
        score = game.get('candidate_score', game.get('book_score'))
        if (game['final_sfen'] != state['sfen'] or game['terminal_reason'] != reason or
                score != book_score(outcome, state['side'], side)):
            raise ValueError(f'{game["id"]}: wrong final state, terminal reason or score')
        if 'book_side' in game:
            hits = [e['ply'] for e in game['events'] if e['book_hit']]
            misses = [e['ply'] for e in game['events'] if e['player'] == 'book' and not e['book_hit']]
            first = next((p for p in misses if hits and p > hits[0]), None)
            last = next((p for p in misses if hits and p > hits[-1]), None)
            if (hits != game['book_hit_plies'] or first != game['first_book_miss_ply'] or
                    last != game['permanent_exit_ply']):
                raise ValueError(f'{game["id"]}: wrong book hit/exit accounting')
    return {'plies': len(moves), 'reason': reason, 'missing': missing}


def run(args):
    manifest = read_json(args.output / 'manifest.json')
    if digest_file(args.referee) != manifest['referee_sha256']:
        raise ValueError('Referee differs from frozen match manifest')
    suite = read_json(args.output / 'openings.json')
    jobs = [(f'{n}-{o["id"]}-{s}', o, n, s)
            for n in manifest['nodes_per_move'] for o in suite['openings'] for s in ('b', 'w')]
    expected = {j[0] for j in jobs}
    actual = {p.stem for p in (args.output / 'games').glob('*.json')}
    if len(expected) != manifest['expected_games'] or actual != expected:
        raise ValueError('Missing, extra or duplicate game IDs')
    loaded = []
    for gid, opening, nodes, side in jobs:
        game = read_json(args.output / 'games' / (gid + '.json'))
        if (game['id'] != gid or game['opening_id'] != opening['id'] or
                game['nodes_per_move'] != nodes or game.get('candidate_side', game.get('book_side')) != side):
            raise ValueError(f'{gid}: wrong pairing/budget')
        loaded.append((game, opening))
    reasons, missing = Counter(), Counter()
    plies = 0
    for _, r in parallel_jobs(loaded, lambda j: verify(args, *j, manifest), args.workers):
        plies += r['plies']
        reasons[r['reason']] += 1
        missing.update(r['missing'])
    result = {'success': True, 'games': len(jobs), 'replayed_plies': plies,
              'terminal_reasons': dict(reasons), 'ordinary_moves_without_score': dict(missing),
              'manifest_sha256': digest_file(args.output / 'manifest.json'),
              'referee_sha256': digest_file(args.referee),
              'limitation': 'Legal replay uses Hayanagi Position, not an independent rules implementation'}
    write_json(args.output / 'validation.json', result)
    print(result, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    parser.add_argument('--workers', type=positive, default=4)
    run(parser.parse_args())
