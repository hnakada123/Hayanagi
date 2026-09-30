#!/usr/bin/env python3
"""Opening-book import/export and USI integration regression tests (standard library).

Usage: python3 -B tests/test_book.py [--engine build/hayanagi]
       [--validator build/hayanagi_book_positions]
"""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_book as book
from test_engine import Engine

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / 'build/hayanagi'
VALIDATOR = ROOT / 'build/hayanagi_book_positions'
START = book.START_KEY + ' 1'


def csa(moves='+7776FU -3334FU +2726FU -8384FU', ending='%TORYO', name='A'):
    return (f'V2\nN+{name}\nN-B\n$EVENT:wdoor+floodgate-300-10F+test\nPI\n+\n'
            "'black_rate:A:3500\n'white_rate:B:3600\n" + '\n'.join(moves.split()) +
            '\n' + ending + '\n').encode()


class ImportTests(unittest.TestCase):
    def test_standard_start_and_ratings(self):
        names, ratings, end, moves = book.parse_csa(csa(), 3000, 4)
        self.assertEqual(names, {'b': 'A', 'w': 'B'})
        self.assertEqual(ratings['w'], 3600)
        self.assertEqual(end, '%TORYO')
        self.assertEqual(len(moves), 4)
        rows = [
            'P1-KY-KE-GI-KI-OU-KI-GI-KE-KY', 'P2 * -HI *  *  *  *  * -KA * ',
            'P3' + '-FU' * 9, 'P4' + ' * ' * 9, 'P5' + ' * ' * 9, 'P6' + ' * ' * 9,
            'P7' + '+FU' * 9, 'P8 * +KA *  *  *  *  * +HI * ',
            'P9+KY+KE+GI+KI+OU+KI+GI+KE+KY',
        ]
        explicit = csa().replace(b'PI\n', ('\n'.join(rows) + '\n').encode())
        self.assertEqual(book.parse_csa(explicit, 3000, 4)[3], moves)

    def test_reject_abnormal_incomplete_handicap_and_missing_rating(self):
        bad = [csa(ending='%TIME_UP'), csa(ending=''), csa().replace(b'PI\n', b'PI82HI\n'),
               csa().replace(b"'white_rate:B:3600\n", b''),
               csa().replace(b'3500', b'nan'), csa().replace(b'3500', b'1500'),
               csa() + b'+2625FU\n', csa().replace(b'PI\n', b'PI\nP+00FU\n')]
        for raw in bad:
            with self.subTest(raw=raw), self.assertRaises(book.Rejected):
                book.parse_csa(raw, 3000, 4)

    def test_full_game_validation_including_moves_beyond_book_window(self):
        with book.LineProcess([str(VALIDATOR), '2']) as worker:
            moves, sfens = book.validate_game(worker, ['+7776FU', '-3334FU', '+2726FU'], 2)
            self.assertEqual(moves, ['7g7f', '3c3d', '2g2f'])
            self.assertEqual(len(sfens), 2)
            self.assertEqual(sfens[0], START)
            with self.assertRaises(book.Rejected):
                book.validate_game(worker, ['+7776FU', '-3334FU', '+7776FU'], 2)

    def test_promotions_captures_drops_and_side_validation(self):
        with book.LineProcess([str(VALIDATOR), '30']) as worker:
            moves, _ = book.validate_game(worker,
                ['+7776FU', '-3334FU', '+8822UM', '-3122GI', '+0055KA'], 30)
            self.assertEqual(moves, ['7g7f', '3c3d', '8h2b+', '3a2b', 'B*5e'])
            for tokens in (['-7776FU'], ['+7776TO'], ['+0055OU']):
                with self.assertRaises(book.Rejected):
                    book.validate_game(worker, tokens, 30)

    def test_deterministic_import_export_and_deduplication(self):
        sequences = ['+7776FU -3334FU +2726FU -8384FU',
                     '+2726FU -8384FU +7776FU -3334FU',
                     '+7776FU -8384FU +2726FU -3334FU']
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for i, moves in enumerate(sequences):
                (root / f'{i}.csa').write_bytes(csa(moves, name=str(i)))
            (root / 'duplicate.csa').write_bytes(csa(sequences[0], name='duplicate'))
            (root / 'invalid.csa').write_bytes(csa(ending='%TIME_UP'))
            dbpath = root / 'work.sqlite'
            command = [sys.executable, '-B', str(ROOT / 'tools/build_book.py')]
            subprocess.run(command + ['import', '--input', str(root), '--validator', str(VALIDATOR),
                           '--work-db', str(dbpath), '--min-game-plies', '4',
                           '--source-url', 'https://example.invalid/test-fixture', '--source-sha256', '0' * 64],
                           check=True, capture_output=True)
            with sqlite3.connect(dbpath) as db:
                stats = json.loads(db.execute("SELECT value FROM metadata WHERE key='import_stats'").fetchone()[0])
                self.assertEqual(stats['duplicate'], 1)
                self.assertEqual(stats['unfinished_or_abnormal'], 1)
                self.assertEqual(db.execute('SELECT count(*) FROM games').fetchone()[0], 3)
            for filename in ('one.db', 'two.db'):
                subprocess.run(command + ['export', '--work-db', str(dbpath), '--output', str(root / filename),
                               '--min-count', '1', '--min-pairs', '1'], check=True, capture_output=True)
            first = (root / 'one.db').read_bytes()
            self.assertEqual(first, (root / 'two.db').read_bytes())
            report = json.loads((root / 'one.json').read_text())
            self.assertEqual(report['book_sha256'], hashlib.sha256(first).hexdigest())
            self.assertIn(b'# HAYANAGI_MAX_PLY 30', first)
            self.assertGreater(report['positions'], 0)
            screened = command + ['export', '--work-db', str(dbpath),
                '--output', str(root / 'screened.db'), '--min-count', '1', '--min-pairs', '1',
                '--max-positions', '1', '--engine', str(ENGINE), '--nodes', '20000',
                '--min-depth', '1', '--max-loss', '1000', '--workers', '2']
            subprocess.run(screened, check=True, capture_output=True)
            measured = (root / 'screened.db').read_bytes()
            measured_report = json.loads((root / 'screened.json').read_text())
            self.assertEqual(measured_report['positions'], 1)
            details = json.loads((root / 'screened.positions.jsonl').read_text())
            self.assertGreater(details['entries'][0]['depth'], 0)
            self.assertIsNotNone(details['analysis'])
            repeated = subprocess.run(screened, check=True, capture_output=True, text=True)
            self.assertIn('1 cached, 0 pending', repeated.stdout)
            self.assertEqual(measured, (root / 'screened.db').read_bytes())
            review = root / 'review.json'
            review_data = {'engine_sha256': book.digest_file(ENGINE),
                           'positions': [{'sfen': details['sfen'], 'history': details['history']}]}
            review.write_text(json.dumps(review_data))
            deeper = screened + ['--review-positions', str(review), '--review-nodes', '40000']
            result = subprocess.run(deeper, check=True, capture_output=True, text=True)
            self.assertIn('0 cached, 1 pending', result.stdout)
            self.assertEqual(json.loads((root / 'screened.positions.jsonl').read_text())['nodes_per_search'], 40000)
            self.assertEqual(json.loads((root / 'screened.json').read_text())['deep_review']['positions'], 1)
            self.assertIn('1 cached, 0 pending', subprocess.run(deeper, check=True, capture_output=True, text=True).stdout)
            review_data['engine_sha256'] = 'wrong engine'
            review.write_text(json.dumps(review_data))
            refused = subprocess.run(deeper, capture_output=True, text=True)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn('different engine', refused.stderr)

    def test_pair_cap_and_holdout_do_not_change_training_support(self):
        with sqlite3.connect(':memory:') as db:
            db.execute('CREATE TABLE games(id INTEGER,black TEXT,white TEXT,moves TEXT,sfens TEXT,holdout INTEGER)')
            for i in range(30):
                db.execute('INSERT INTO games VALUES (?,?,?,?,?,?)',
                           (i, 'A', 'B', '7g7f 3c3d', json.dumps([START]), 0))
            for i, name in enumerate(['C', 'D', 'E'], 30):
                db.execute('INSERT INTO games VALUES (?,?,?,?,?,?)',
                           (i, name, 'B', '2g2f 8c8d', json.dumps([START]), 0))
            db.execute('INSERT INTO games VALUES (?,?,?,?,?,?)',
                       (99, 'F', 'G', '9g9f 9c9d', json.dumps([START]), 1))
            args = SimpleNamespace(min_count=1, min_pairs=1, pair_cap=1, max_candidates=3,
                                   max_positions=100, first_moves=['7g7f', '2g2f', '9g9f'])
            pos = book.collect_candidates(db, args)[0]
            self.assertEqual([e['move'] for e in pos['entries']], ['2g2f', '7g7f'])
            self.assertEqual(pos['entries'][1]['count'], 30)

    def test_search_bounds_partial_depth_and_mate(self):
        parsed = book.parse_search([
            'info depth 4 multipv 1 score cp 20 nodes 100 pv 7g7f 3c3d',
            'info depth 4 multipv 2 score cp 10 nodes 100 pv 2g2f 8c8d',
            'info depth 5 score cp 90 lowerbound nodes 200 pv 7g7f 3c3d',
            'info depth 6 multipv 1 score mate -3 nodes 300 pv 2g2f 8c8d',
        ])
        self.assertNotIn(5, parsed)
        self.assertEqual(len(parsed[4]), 2)
        self.assertEqual(parsed[6]['2g2f']['score'], -29997)
        self.assertEqual(parsed[6]['2g2f']['score_type'], 'mate')

    def test_actual_hayanagi_screening(self):
        pos = {'history': [], 'entries': [{'move': '7g7f'}, {'move': '2g2f'}]}
        result = book.analyze_position(pos, ENGINE, 20000)
        self.assertNotIn('error', result)
        self.assertEqual(set(result['candidates']), {'7g7f', '2g2f'})
        self.assertGreater(result['baseline']['depth'], 0)
        self.assertTrue(all(s['depth'] == result['baseline']['depth'] for s in result['candidates'].values()))
        self.assertEqual(result['comparison_depth'], result['baseline']['depth'])
        self.assertIn(result['discovery']['pv'][0], result['comparison_moves'])

    def test_reserve_multipv_slot_for_discovered_move(self):
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/build_book.py'),
                                 'export', '--work-db', 'unused.sqlite', '--engine', str(ENGINE),
                                 '--max-candidates', '32'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('reserve one MultiPV slot', result.stderr)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.engine = Engine(ENGINE)
        self.addCleanup(self.engine.close)
        self.engine.send('usi')
        lines = self.engine.until('usiok')
        self.assertEqual([s for s in lines if s.startswith('option name BookFile ')],
                         ['option name BookFile type combo default hayanagi_book.db'
                          ' var no_book var hayanagi_book.db'])

    def load(self, extra='', ponder='3c3d'):
        Path(self.directory.name, 'hayanagi_book.db').write_text(
            '#YANEURAOU-DB2016 1.00\n' + extra + 'sfen ' + START + '\n' +
            f'7g7f {ponder} 10 5 10\n', encoding='ascii')
        self.engine.send(f'setoption name BookDir value {self.directory.name}')
        self.engine.ready()

    def search(self, position='position startpos'):
        self.engine.send(position + '\ngo nodes 5000')
        return self.engine.until('bestmove ')

    def test_ply_normalization_and_book_limit(self):
        self.load('# HAYANAGI_MAX_PLY 30\n')
        lines = self.search('position sfen ' + book.START_KEY + ' 17')
        self.assertTrue(any('book hit' in s for s in lines), lines)
        self.assertEqual(lines[-1], 'bestmove 7g7f')
        for ply in (31, 101):
            lines = self.search('position sfen ' + book.START_KEY + f' {ply}')
            self.assertFalse(any('book hit' in s for s in lines), lines)

    def test_no_book_disables_already_loaded_book(self):
        self.load()
        self.assertTrue(any('book hit' in s for s in self.search()))
        self.engine.send('setoption name BookFile value no_book')
        self.engine.ready()
        self.assertFalse(any('book hit' in s for s in self.search()))
        self.engine.send('setoption name BookFile value hayanagi_book.db')
        self.engine.ready()
        self.assertTrue(any('book hit' in s for s in self.search()))

    def test_invalid_ponder_is_omitted(self):
        self.load(ponder='7g7f')
        self.engine.send('setoption name USI_Ponder value true')
        self.assertEqual(self.search()[-1], 'bestmove 7g7f')

    def test_terminal_draw_takes_precedence(self):
        self.load()
        self.engine.send('setoption name MaxMovesToDraw value 1')
        lines = self.search('position sfen ' + book.START_KEY + ' 2')
        self.assertFalse(any('book hit' in s for s in lines), lines)
        self.assertTrue(any('terminal move_limit draw' in s for s in lines), lines)

    def test_repetition_history_takes_precedence(self):
        self.load()
        cycle = '5i5h 5a5b 5h5i 5b5a '
        lines = self.search('position startpos moves ' + cycle * 3)
        self.assertFalse(any('book hit' in s for s in lines), lines)
        self.assertTrue(any('terminal repetition draw' in s for s in lines), lines)

    def test_missing_book_falls_back_to_search(self):
        self.engine.send(f'setoption name BookDir value {self.directory.name}')
        self.engine.ready()
        lines = self.search()
        self.assertFalse(any('book hit' in s for s in lines), lines)
        self.assertTrue(any(s.startswith('info depth ') for s in lines), lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', type=Path, default=ENGINE)
    parser.add_argument('--validator', type=Path, default=VALIDATOR)
    options, remaining = parser.parse_known_args()
    ENGINE, VALIDATOR = options.engine.resolve(), options.validator.resolve()
    unittest.main(argv=[sys.argv[0]] + remaining)
