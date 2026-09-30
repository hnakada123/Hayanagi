#!/usr/bin/env python3
"""過去の開始局面・元棋譜を除外して、新しい先後交換用局面を固定する。"""
import argparse
from collections import Counter
import json
from pathlib import Path
import random
import sqlite3

from build_book import digest_file, key, positive
from match_book import frozen_manifest, read_json


def prepare(args):
    excluded_positions, excluded_games, exclusions = set(), set(), {}
    root = Path(__file__).resolve().parents[1]
    for path in args.exclude_openings:
        filename = str(path.resolve().relative_to(root))
        exclusions[filename] = digest_file(path)
        for opening in read_json(path)['openings']:
            if 'sfen' in opening: excluded_positions.add(key(opening['sfen']))
            if 'source_game' in opening: excluded_games.add(opening['source_game'])
    pools = {n: {} for n in (4, 6, 8)}
    with sqlite3.connect(f'file:{args.work_db.resolve()}?mode=ro', uri=True) as db:
        corpus = json.loads(db.execute("SELECT value FROM metadata WHERE key='corpus_sha256'").fetchone()[0])
        for game, encoded_moves, encoded_sfens in db.execute('SELECT id,moves,sfens FROM games WHERE holdout=1 ORDER BY id'):
            if game in excluded_games:
                continue
            moves, sfens = encoded_moves.split(), json.loads(encoded_sfens)
            if moves[0] not in ('2g2f', '7g7f', '5g5f'):
                continue
            for n, pool in pools.items():
                if n < len(sfens) and key(sfens[n]) not in excluded_positions:
                    pool.setdefault(key(sfens[n]), {'moves': moves[:n], 'sfen': sfens[n], 'source_game': game})
    rng = random.Random(args.seed)
    shuffled = {}
    for n, pool in pools.items():
        shuffled[n] = [pool[k] for k in sorted(pool)]
        rng.shuffle(shuffled[n])
    openings = [{'id': 'startpos', 'group': 'startpos', 'moves': []}]
    positions, games = set(), set()
    while len(openings) <= args.pairs:
        added = False
        for pool in shuffled.values():
            while pool and (key(pool[-1]['sfen']) in positions or pool[-1]['source_game'] in games):
                pool.pop()
            if pool and len(openings) <= args.pairs:
                opening = pool.pop()
                positions.add(key(opening['sfen']))
                games.add(opening['source_game'])
                opening.update(id=f'holdout_{len(openings):03d}', group='holdout')
                openings.append(opening)
                added = True
        if not added:
            raise ValueError('Not enough unused holdout positions/source games')
    suite = {'schema_version': 1, 'seed': args.seed, 'pairs': args.pairs,
             'source_corpus_sha256': corpus, 'exclusions': exclusions,
             'sampling': 'round-robin prefixes 4/6/8; unique SFEN and source game; no book-hit/score/match-outcome selection',
             'selected_by_prefix_length': dict(Counter(str(len(o['moves'])) for o in openings[1:])),
             'limitation': 'Game-level holdout; common opening positions may also occur in training games',
             'openings': openings}
    frozen_manifest(args.output, suite)
    print(f'Frozen {args.pairs} holdout positions plus startpos: {args.output}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-db', type=Path, required=True)
    parser.add_argument('--exclude-openings', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pairs', type=positive, default=40)
    parser.add_argument('--seed', type=int, default=20261002)
    prepare(parser.parse_args())
