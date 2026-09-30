#!/usr/bin/env python3
"""Build a reproducible DB2016 opening book from Floodgate's standard-start CSA games.

Python standard library only. Full-game legality is checked by hayanagi_book_positions.
Raw records and the resumable analysis cache stay in the working SQLite database.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
from pathlib import Path
import queue
import re
import sqlite3
import subprocess
import threading
import time

START_KEY = 'lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b -'
START_ROWS = [
    '-KY-KE-GI-KI-OU-KI-GI-KE-KY', '*-HI*****-KA*', '-FU' * 9,
    '*' * 9, '*' * 9, '*' * 9, '+FU' * 9, '*+KA*****+HI*',
    '+KY+KE+GI+KI+OU+KI+GI+KE+KY',
]
MOVE_RE = re.compile(r'[+-](?:00|[1-9][1-9])[1-9][1-9](?:FU|KY|KE|GI|KI|KA|HI|OU|TO|NY|NK|NG|UM|RY)')
GOOD_ENDINGS = {'%TORYO', '%SENNICHITE', '%JISHOGI', '%KACHI'}
SCHEMA_VERSION = 1


def digest_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def key(sfen):
    return ' '.join(sfen.split()[:3])


class Rejected(ValueError):
    pass


def parse_csa(data, min_rating, min_plies):
    """Accept the standard-start, one-record-per-line subset used by Floodgate.

    Do not silently interpret a handicap, setup change, unknown command, or missing
    result as a normal completed game. Comments (including engine scores) are ignored.
    """
    names, rates, rows, moves = {}, {}, {}, []
    pi = False
    started = False
    ending = None
    event = ''
    for raw in data.decode('utf-8-sig', errors='replace').splitlines():
        line = raw.rstrip()
        if not line:
            continue
        if line.startswith("'black_rate:") or line.startswith("'white_rate:"):
            side = 'b' if line.startswith("'black_rate:") else 'w'
            try:
                rates[side] = float(line.rsplit(':', 1)[1])
            except ValueError:
                raise Rejected('invalid_rating') from None
            continue
        if line.startswith("'"):
            continue
        if line.startswith('$EVENT:'):
            event = line[7:]
        elif line.startswith('$') or line.startswith('V'):
            continue
        elif line.startswith(('N+', 'N-')):
            names['b' if line[1] == '+' else 'w'] = line[2:]
        elif line == 'PI' and not started and not rows:
            pi = True
        elif re.match(r'^P[1-9]', line) and not started and not pi:
            rank = int(line[1])
            if rank in rows:
                raise Rejected('duplicate_setup')
            rows[rank] = line[2:].replace(' ', '')
        elif line in ('P+', 'P-') and not started:
            continue
        elif line == '+' and not started:
            if not pi and [rows.get(i) for i in range(1, 10)] != START_ROWS:
                raise Rejected('nonstandard_start')
            started = True
        elif MOVE_RE.fullmatch(line) and started and ending is None:
            moves.append(line)
        elif re.fullmatch(r'T\d+(?:,\d+)?', line) and started:
            continue
        elif line.startswith('%') and started and ending is None:
            ending = line
        else:
            raise Rejected('unsupported_csa')
    if not event.startswith('wdoor+floodgate-'):
        raise Rejected('not_floodgate')
    if ending not in GOOD_ENDINGS:
        raise Rejected('unfinished_or_abnormal')
    if len(moves) < min_plies or len(moves) > 2048:
        raise Rejected('game_length')
    if set(names) != {'b', 'w'} or not all(names.values()):
        raise Rejected('missing_player')
    if set(rates) != {'b', 'w'} or not all(math.isfinite(v) for v in rates.values()):
        raise Rejected('missing_rating')
    if min(rates.values()) < min_rating:
        raise Rejected('low_rating')
    return names, rates, ending, moves


class LineProcess:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=None, text=True, encoding='utf-8', bufsize=1)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip('\r\n'))
        self.lines.put(None)

    def send(self, line):
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()

    def receive(self, timeout=60):
        try:
            line = self.lines.get(timeout=timeout)
        except queue.Empty:
            raise RuntimeError('Worker response timed out') from None
        if line is None:
            raise RuntimeError('Worker exited unexpectedly')
        return line

    def until(self, prefix, timeout=60):
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError('Engine response timed out')
            line = self.receive(remaining)
            lines.append(line)
            if line.startswith(prefix):
                return lines

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=5)
        self.process.stdout.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def validate_game(worker, moves, max_ply):
    worker.send('csa ' + ' '.join(moves))
    fields = worker.receive().split('\t')
    if fields[0] == 'error':
        raise Rejected('illegal_game')
    if fields[0] != 'ok' or len(fields) != 2 + min(max_ply, len(moves)):
        raise RuntimeError('Invalid validator response')
    return fields[1].split(), fields[2:]


def import_games(args):
    if args.work_db.exists():
        raise ValueError('Work DB already exists; use a new path to import a new corpus')
    files = sorted(args.input.rglob('*.csa'))
    if not files:
        raise ValueError('No .csa files found')
    args.work_db.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.work_db.with_suffix('.importing.sqlite')
    if temporary.exists():
        raise ValueError(f'Previous incomplete import exists: {temporary}')
    stats = Counter()
    corpus_hash = hashlib.sha256()
    seen = set()
    with sqlite3.connect(temporary) as db, LineProcess([str(args.validator.resolve()), str(args.max_ply)]) as worker:
        db.executescript('''
            CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE games (id TEXT PRIMARY KEY, source TEXT, source_sha256 TEXT,
                black TEXT, white TEXT, black_rating REAL, white_rating REAL,
                ending TEXT, moves TEXT, sfens TEXT, holdout INTEGER);
            CREATE TABLE analysis (id TEXT PRIMARY KEY, result TEXT NOT NULL);
        ''')
        for number, path in enumerate(files, 1):
            raw = path.read_bytes()
            name = path.relative_to(args.input).as_posix()
            raw_hash = hashlib.sha256(raw).hexdigest()
            corpus_hash.update((name + '\0' + raw_hash + '\n').encode())
            stats['files'] += 1
            try:
                names, rates, ending, csa = parse_csa(raw, args.min_rating, args.min_game_plies)
                game_id = hashlib.sha256(' '.join(csa).encode()).hexdigest()
                if game_id in seen:
                    raise Rejected('duplicate')
                moves, sfens = validate_game(worker, csa, args.max_ply)
                seen.add(game_id)
                holdout = int(int(game_id[:8], 16) % 10 == 0)
                db.execute('INSERT INTO games VALUES (?,?,?,?,?,?,?,?,?,?,?)',
                           (game_id, name, raw_hash, names['b'], names['w'], rates['b'], rates['w'],
                            ending, ' '.join(moves), json.dumps(sfens), holdout))
                stats['holdout_games' if holdout else 'training_games'] += 1
            except Rejected as ex:
                stats[str(ex)] += 1
            if number % 5000 == 0:
                db.commit()
                print(f'import {number}/{len(files)}: {dict(stats)}', flush=True)
        metadata = {
            'schema_version': SCHEMA_VERSION, 'source_url': args.source_url,
            'source_archive_sha256': args.source_sha256, 'corpus_sha256': corpus_hash.hexdigest(),
            'validator_sha256': digest_file(args.validator),
            'import_settings': {'max_ply': args.max_ply, 'min_rating': args.min_rating,
                                'min_game_plies': args.min_game_plies, 'holdout_modulus': 10},
            'import_stats': dict(stats),
        }
        db.executemany('INSERT INTO metadata VALUES (?,?)',
                       [(k, json.dumps(v, ensure_ascii=False)) for k, v in metadata.items()])
    # Windows also requires the SQLite handle to be closed before the rename.
    db.close()
    temporary.replace(args.work_db)
    print(json.dumps(metadata, ensure_ascii=False, indent=2))


def collect_candidates(db, args):
    positions = {}
    for black, white, text, encoded in db.execute(
            'SELECT black,white,moves,sfens FROM games WHERE holdout=0 ORDER BY id'):
        moves, sfens = text.split(), json.loads(encoded)
        pair = tuple(sorted((black, white)))
        visited = set()
        for ply, sfen in enumerate(sfens):
            k = key(sfen)
            if k in visited:
                continue  # Repeated positions in one game are not independent support.
            visited.add(k)
            pos = positions.setdefault(k, {'sfen': sfen, 'history': moves[:ply], 'moves': {}})
            if len(pos['history']) > ply:
                pos['sfen'], pos['history'] = sfen, moves[:ply]
            edge = pos['moves'].setdefault(moves[ply], {'pairs': Counter(), 'replies': Counter()})
            edge['pairs'][pair] += 1
            if ply + 1 < len(moves):
                edge['replies'][moves[ply + 1]] += 1
    candidates = []
    for k, pos in positions.items():
        entries = []
        for move, edge in pos['moves'].items():
            count = sum(edge['pairs'].values())
            if count < args.min_count or len(edge['pairs']) < args.min_pairs:
                continue
            if k == START_KEY and move not in args.first_moves:
                continue
            replies = sorted(edge['replies'], key=lambda x: (-edge['replies'][x], x))
            entries.append({'move': move, 'count': count, 'pair_count': len(edge['pairs']),
                            'weight': sum(min(n, args.pair_cap) for n in edge['pairs'].values()),
                            'ponder': replies[0] if replies else 'none'})
        entries.sort(key=lambda e: (-e['weight'], -e['count'], e['move']))
        if entries:
            candidates.append({'key': k, 'sfen': pos['sfen'], 'history': pos['history'],
                               'entries': entries[:args.max_candidates]})
    candidates.sort(key=lambda p: (-sum(e['weight'] for e in p['entries']),
                                   len(p['history']), p['key']))
    return candidates[:args.max_positions]


def parse_search(lines):
    """Keep exact scores by completed depth, preserving mate/centipawn distinction."""
    depths = defaultdict(dict)
    for line in lines:
        fields = line.split()
        if not line.startswith('info depth ') or 'score' not in fields or 'pv' not in fields:
            continue
        if 'lowerbound' in fields or 'upperbound' in fields:
            continue
        depth = int(fields[2])
        i = fields.index('score')
        kind, value = fields[i + 1:i + 3]
        if kind not in ('cp', 'mate') or value in ('+', '-'):
            continue
        pv = fields[fields.index('pv') + 1:]
        if not pv:
            continue
        number = int(value)
        score = number if kind == 'cp' else (30000 - abs(number)) * (1 if number > 0 else -1)
        depths[depth][pv[0]] = {'score': score, 'score_type': kind, 'score_value': number,
                              'depth': depth, 'pv': pv,
                              'nodes': int(fields[fields.index('nodes') + 1]) if 'nodes' in fields else 0}
    return depths


def analyze_position(pos, executable, nodes):
    # A fresh process per position makes analysis independent of worker scheduling/TT history.
    with LineProcess([str(executable.resolve())]) as engine:
        engine.send('usi')
        identity = [s for s in engine.until('usiok') if s.startswith('id name ')]
        engine.send('setoption name USI_OwnBook value false\n'
                    'setoption name USI_AnalyseMode value true\n'
                    'setoption name USI_LimitStrength value false\n'
                    'setoption name Threads value 1\nsetoption name Hash value 16\nisready')
        engine.until('readyok')
        engine.send('usinewgame\nposition startpos' +
                    (' moves ' + ' '.join(pos['history']) if pos['history'] else ''))
        engine.send(f'go nodes {nodes}')
        baseline = parse_search(engine.until('bestmove ', timeout=120))
        moves = [e['move'] for e in pos['entries']]
        engine.send(f'setoption name MultiPV value {len(moves)}')
        engine.send(f'go nodes {nodes} searchmoves ' + ' '.join(moves))
        restricted = parse_search(engine.until('bestmove ', timeout=120))
        engine.send('quit')
        complete = [d for d, scores in restricted.items() if set(moves) <= set(scores)]
        if not baseline or not complete:
            return {'error': 'incomplete_search', 'engine': identity}
        depth = max(complete)
        base = max(baseline[max(baseline)].values(), key=lambda x: x['score'])
        return {'engine': identity, 'baseline': base, 'candidates': restricted[depth]}


def evaluate_holdout(db, exported):
    per_ply = defaultdict(Counter)
    first_misses = Counter()
    games = 0
    for text, encoded in db.execute('SELECT moves,sfens FROM games WHERE holdout=1 ORDER BY id'):
        moves, sfens = text.split(), json.loads(encoded)
        first_miss = None
        games += 1
        for ply, sfen in enumerate(sfens):
            entries = exported.get(key(sfen), [])
            per_ply[ply + 1]['positions'] += 1
            if entries:
                per_ply[ply + 1]['hits'] += 1
                per_ply[ply + 1]['recorded_move_in_book'] += int(any(e['move'] == moves[ply] for e in entries))
            elif first_miss is None:
                first_miss = ply + 1
        first_misses[str(first_miss) if first_miss is not None else 'none_in_window'] += 1
    return {'games': games, 'method': 'replay held-out games; not playing-strength measurement',
            'per_ply': dict(sorted(per_ply.items())), 'first_miss': dict(sorted(first_misses.items()))}


def export_book(args):
    if not args.work_db.is_file():
        raise ValueError('Import a corpus first')
    with sqlite3.connect(args.work_db) as db:
        metadata = {k: json.loads(v) for k, v in db.execute('SELECT key,value FROM metadata')}
        if metadata['schema_version'] != SCHEMA_VERSION:
            raise ValueError('Unsupported work DB version')
        candidates = collect_candidates(db, args)
        if not any(p['key'] == START_KEY for p in candidates):
            raise ValueError('No supported standard first move; relax filters or add games')
        print(f'{len(candidates)} candidate positions', flush=True)
        analyses = {}
        engine_hash = digest_file(args.engine) if args.engine else None
        if args.engine:
            pending = []
            for pos in candidates:
                cache_id = hashlib.sha256(json.dumps(
                    [SCHEMA_VERSION, engine_hash, args.nodes, pos['history'],
                     [e['move'] for e in pos['entries']]], sort_keys=True).encode()).hexdigest()
                row = db.execute('SELECT result FROM analysis WHERE id=?', (cache_id,)).fetchone()
                if row:
                    analyses[pos['key']] = json.loads(row[0])
                else:
                    pending.append((cache_id, pos))
            print(f'analysis: {len(analyses)} cached, {len(pending)} pending', flush=True)
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                futures = {pool.submit(analyze_position, pos, args.engine, args.nodes): (cid, pos['key'])
                           for cid, pos in pending}
                try:
                    for n, future in enumerate(as_completed(futures), 1):
                        cid, k = futures[future]
                        result = future.result()
                        analyses[k] = result
                        db.execute('INSERT OR REPLACE INTO analysis VALUES (?,?)', (cid, json.dumps(result)))
                        db.commit()  # Checkpoint each completed position for resuming.
                        if n % 100 == 0 or n == len(pending):
                            print(f'analysis {n}/{len(pending)}', flush=True)
                except BaseException:
                    # On Ctrl-C or a worker failure, wait only for in-flight searches.
                    for future in futures:
                        future.cancel()
                    raise
        exported, provenance = {}, []
        rejected = Counter()
        for pos in candidates:
            entries = []
            analysis = analyses.get(pos['key'])
            for original in pos['entries']:
                entry = dict(original, score=0, depth=0)
                if args.engine:
                    if 'error' in analysis:
                        rejected['incomplete_search'] += 1
                        continue
                    measured = analysis['candidates'][entry['move']]
                    if min(measured['depth'], analysis['baseline']['depth']) < args.min_depth:
                        rejected['insufficient_depth'] += 1
                        continue
                    if (measured['score'] < analysis['baseline']['score'] - args.max_loss or
                            (measured['score_type'] == 'mate' and measured['score_value'] < 0)):
                        rejected['search_disagreement'] += 1
                        continue
                    entry.update(score=measured['score'], depth=measured['depth'])
                entries.append(entry)
            if entries:
                exported[pos['key']] = entries
                provenance.append({'sfen': pos['sfen'], 'history': pos['history'], 'entries': entries,
                                   'analysis': analysis})
        if START_KEY not in exported:
            raise ValueError('Analysis rejected all first moves; no book written')
        provenance.sort(key=lambda p: p['sfen'])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        text = ['#YANEURAOU-DB2016 1.00', '# Hayanagi opening book; see adjacent .json report',
                f'# HAYANAGI_MAX_PLY {metadata["import_settings"]["max_ply"]}',
                f'# NOE:{len(provenance)}']
        for pos in provenance:
            text.append('sfen ' + pos['sfen'])
            text.extend(f'{e["move"]} {e["ponder"]} {e["score"]} {e["depth"]} {e["count"]}'
                        for e in pos['entries'])
        content = ('\n'.join(text) + '\n').encode('ascii')
        report = dict(metadata, generator_sha256=digest_file(Path(__file__)), export_settings={
            'min_count': args.min_count, 'min_pairs': args.min_pairs, 'pair_cap': args.pair_cap,
            'max_positions': args.max_positions, 'max_candidates': args.max_candidates,
            'first_moves': args.first_moves, 'nodes_per_search': args.nodes if args.engine else 0,
            'searches_per_position': 2 if args.engine else 0, 'engine_sha256': engine_hash,
            'min_depth': args.min_depth, 'max_loss_cp': args.max_loss,
            'selection': 'pair-capped frequency order among candidates passing optional search screening'},
            book_sha256=hashlib.sha256(content).hexdigest(), positions=len(provenance),
            entries=sum(len(p['entries']) for p in provenance), rejected=dict(rejected),
            holdout=evaluate_holdout(db, exported),
            redistribution_status='Not established by the archive download page; local generated artifact',
            limitations=['No self-play training or playing-strength claim',
                         'Search disagreement is a shallow screening signal, not proof of a bad move',
                         'Player names are used as pair identities; versions/aliases may be related'])
        # Write supporting evidence before replacing the usable book.
        atomic_write(args.output.with_suffix('.json'), json.dumps(report, ensure_ascii=False, indent=2).encode() + b'\n')
        atomic_write(args.output.with_suffix('.positions.jsonl'), b''.join(
            (json.dumps(p, ensure_ascii=False, sort_keys=True) + '\n').encode() for p in provenance))
        atomic_write(args.output, content)
        print(json.dumps({k: report[k] for k in ['positions', 'entries', 'book_sha256', 'rejected']}, indent=2))


def atomic_write(path, data):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    temporary.replace(path)


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('must be positive')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    ingest = commands.add_parser('import', help='filter CSA games and validate all moves')
    ingest.add_argument('--input', type=Path, required=True)
    ingest.add_argument('--validator', type=Path, default=Path('build/hayanagi_book_positions'))
    ingest.add_argument('--work-db', type=Path, required=True)
    ingest.add_argument('--max-ply', type=positive, default=30)
    ingest.add_argument('--min-rating', type=float, default=3000)
    ingest.add_argument('--min-game-plies', type=positive, default=40)
    ingest.add_argument('--source-url', required=True)
    ingest.add_argument('--source-sha256', required=True)
    export = commands.add_parser('export', help='select, optionally analyze, and export DB2016')
    export.add_argument('--work-db', type=Path, required=True)
    export.add_argument('--output', type=Path, default=Path('book/standard_book.db'))
    export.add_argument('--min-count', type=positive, default=3)
    export.add_argument('--min-pairs', type=positive, default=2)
    export.add_argument('--pair-cap', type=positive, default=16)
    export.add_argument('--max-positions', type=positive, default=5000)
    export.add_argument('--max-candidates', type=positive, default=3)
    export.add_argument('--first-moves', nargs='+', default=['7g7f', '2g2f', '5g5f'])
    export.add_argument('--engine', type=Path, help='optional Hayanagi USI binary for tactical screening')
    export.add_argument('--nodes', type=positive, default=100000)
    export.add_argument('--workers', type=positive, default=4)
    export.add_argument('--min-depth', type=positive, default=3)
    export.add_argument('--max-loss', type=positive, default=150)
    args = parser.parse_args()
    if args.command == 'import':
        if args.max_ply > 512 or not math.isfinite(args.min_rating):
            parser.error('max-ply must be <=512 and min-rating must be finite')
        if not re.fullmatch('[0-9a-f]{64}', args.source_sha256):
            parser.error('source-sha256 must be a lowercase SHA-256 digest')
        import_games(args)
    else:
        if args.max_candidates > 32:
            parser.error('max-candidates must be <=32 (Hayanagi MultiPV limit)')
        export_book(args)


if __name__ == '__main__':
    main()
