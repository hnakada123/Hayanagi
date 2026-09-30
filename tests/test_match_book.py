#!/usr/bin/env python3
"""Regression tests for book/no-book paired match adjudication and accounting."""
import argparse
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import match_book as match

ENGINE = ROOT / 'build/hayanagi'
REFEREE = ROOT / 'build/hayanagi_match_referee'


class RefereeTests(unittest.TestCase):
    def test_move_validation_and_state(self):
        with match.Referee([str(REFEREE)]) as referee:
            state = referee.state('position startpos')
            self.assertEqual(len(state['legal']), 30)
            self.assertEqual(state['side'], 'b')
            with self.assertRaises(RuntimeError):
                referee.state('move 7g7e')
            state = referee.state('move 7g7f')
            self.assertEqual(state['side'], 'w')
            self.assertTrue(state['sfen'].endswith(' w - 2'))

    def test_repetition_is_draw_and_keeps_history(self):
        with match.Referee([str(REFEREE)]) as referee:
            referee.state('position startpos')
            for move in ['5i5h', '5a5b', '5h5i', '5b5a'] * 3:
                state = referee.state('move ' + move)
            self.assertEqual((state['outcome'], state['reason']), ('draw', 'repetition'))
            with self.assertRaises(RuntimeError):
                referee.state('move 7g7f')

    def test_checkmate_is_loss_for_side_to_move(self):
        with match.Referee([str(REFEREE)]) as referee:
            state = referee.state('position sfen 8k/6G1R/7G1/9/9/9/9/9/4K4 w - 2')
            self.assertEqual((state['side'], state['outcome'], state['reason']), ('w', 'loss', 'checkmate'))
            self.assertEqual(state['legal'], set())


class AccountingTests(unittest.TestCase):
    def test_prepare_excludes_previous_positions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            moves = ['7g7f', '3c3d', '2g2f', '8c8d', '2f2e', '8d8e', '2e2d', '2c2d']
            sfens = []
            with match.Referee([str(REFEREE)]) as referee:
                sfens.append(referee.state('position startpos')['sfen'])
                for move in moves:
                    sfens.append(referee.state('move ' + move)['sfen'])
            db_path = root / 'work.sqlite'
            with sqlite3.connect(db_path) as db:
                db.execute('CREATE TABLE metadata (key TEXT, value TEXT)')
                db.execute('INSERT INTO metadata VALUES (?, ?)', ('corpus_sha256', json.dumps('test')))
                db.execute('CREATE TABLE games (id TEXT, moves TEXT, sfens TEXT, holdout INTEGER)')
                db.execute('INSERT INTO games VALUES (?, ?, ?, 1)', ('game', ' '.join(moves), json.dumps(sfens)))
            excluded = root / 'previous.json'
            match.write_json(excluded, {'openings': [{'sfen': sfens[2]}, {'moves': []}]})
            output = root / 'new.json'
            match.prepare(SimpleNamespace(work_db=db_path, output=output, pairs=3,
                                          seed=10, exclude_openings=excluded))
            report = match.read_json(output)
            self.assertEqual(report['excluded_position_count'], 1)
            self.assertEqual({match.key(o['sfen']) for o in report['openings'][1:]},
                             {match.key(sfens[i]) for i in (4, 6, 8)})

    def test_result_perspective_including_terminal_win_and_draw(self):
        self.assertEqual(match.book_score('win', 'b', 'b'), 1)
        self.assertEqual(match.book_score('loss', 'w', 'b'), 1)
        self.assertEqual(match.book_score('win', 'w', 'b'), 0)
        self.assertEqual(match.book_score('loss', 'b', 'b'), 0)
        self.assertEqual(match.book_score('draw', 'w', 'b'), 0.5)

    def test_complete_pairs_and_draw_score(self):
        games = []
        for i, value in enumerate([1, 0.5, 0, 1]):
            games.append({'opening_id': str(i // 2), 'book_side': 'b' if i % 2 == 0 else 'w',
                          'book_score': value, 'permanent_exit_ply': 11,
                          'terminal_reason': 'repetition' if value == 0.5 else 'checkmate',
                          'book_hit_plies': [1, 3], 'moves': [str(i)]})
        report = match.match_statistics(games)
        self.assertEqual(report['complete_pairs'], 2)
        self.assertEqual(report['score_rate'], 0.625)
        self.assertEqual((report['book_wins'], report['book_losses'], report['draws']), (2, 1, 1))
        self.assertEqual(match.paired_interval([0, 1], samples=100), [0, 1])

    def test_exit_metrics_exclude_missing_or_mate_values(self):
        points = {'0': {'cp': 50}, '4': {'cp': -50}, '8': {'cp': -200}}
        windows = [{'game_id': 'one', 'exit_ply': 13,
                    'points': {'book': points, 'control_same_color': {'0': {'cp': None}, '4': None, '8': None}}}]
        report = match.exit_statistics(windows)
        self.assertEqual(report['book']['delta_after_8_plies']['mean'], -250)
        self.assertEqual(report['book']['delta_after_8_plies']['drops_at_least_200_cp'], 1)
        self.assertEqual(report['control_same_color']['entry_cp_count'], 0)

    def test_small_paired_run_resume_and_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'tiny.db'
            book.write_text('#YANEURAOU-DB2016 1.00\n# HAYANAGI_MAX_PLY 1\n'
                'sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1\n'
                '7g7f 3c3d 0 0 1\n')
            openings = root / 'openings.json'
            match.write_json(openings, {'openings': [{'id': 'holdout_001', 'group': 'holdout', 'moves': []}]})
            base = [sys.executable, '-B', str(ROOT / 'tools/match_book.py')]
            command = base + ['run', '--engine', str(ENGINE), '--referee', str(REFEREE),
                '--book', str(book), '--openings', str(openings), '--output', str(root / 'run'),
                '--nodes', '1000', '--max-plies', '12', '--workers', '2']
            subprocess.run(command, check=True, capture_output=True)
            again = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertIn('2 cached, 0 pending', again.stdout)
            report = match.read_json(root / 'run/summary.json')
            self.assertTrue(report['complete'])
            self.assertEqual(report['completed_games'], 2)
            games = [match.read_json(p) for p in (root / 'run/games').glob('*.json')]
            for game in games:
                self.assertTrue(all(e['player'] == 'book' for e in game['events'] if e['book_hit']))
                self.assertEqual(game['terminal_reason'], 'runner_ply_cap')
                self.assertEqual(game['book_score'], 0.5)
            subprocess.run(base + ['audit', '--engine', str(ENGINE), '--output', str(root / 'run'),
                           '--nodes', '2000', '--workers', '2'], check=True, capture_output=True)
            report = match.read_json(root / 'run/summary.json')
            self.assertEqual(report['conditions']['1000']['post_exit']['eligible_games'], 1)
            self.assertEqual(report['conditions']['1000']['post_exit']['book']['entry_cp_count'], 1)
            subprocess.run([sys.executable, '-B', str(ROOT / 'tools/validate_matches.py'),
                            '--output', str(root / 'run'), '--referee', str(REFEREE)],
                           check=True, capture_output=True)

    def test_engine_comparison_disables_both_books_and_resumes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            openings = root / 'openings.json'
            match.write_json(openings, {'openings': [{'id': 'holdout_001', 'group': 'holdout', 'moves': []}]})
            command = [sys.executable, '-B', str(ROOT / 'tools/match_search.py'),
                       '--engine', str(ENGINE), '--baseline', str(ENGINE), '--referee', str(REFEREE),
                       '--openings', str(openings), '--output', str(root / 'run'),
                       '--nodes', '1000', '--max-plies', '8', '--workers', '2']
            subprocess.run(command, check=True, capture_output=True)
            again = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertIn('2 cached, 0 pending', again.stdout)
            report = match.read_json(root / 'run/summary.json')
            stats = report['conditions']['1000']['holdout']
            self.assertEqual((stats['games'], stats['draws'], stats['score_rate']), (2, 2, 0.5))
            self.assertFalse(report['manifest']['books'])
            for path in (root / 'run/games').glob('*.json'):
                self.assertTrue(all(not e['book_hit'] for e in match.read_json(path)['events']))
            check = [sys.executable, '-B', str(ROOT / 'tools/validate_matches.py'),
                     '--output', str(root / 'run'), '--referee', str(REFEREE)]
            subprocess.run(check, check=True, capture_output=True)
            path = next((root / 'run/games').glob('*.json'))
            game = match.read_json(path)
            game['candidate_score'] = 1
            match.write_json(path, game)
            self.assertNotEqual(subprocess.run(check, capture_output=True).returncode, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', type=Path, default=ENGINE)
    parser.add_argument('--referee', type=Path, default=REFEREE)
    args, remaining = parser.parse_known_args()
    ENGINE, REFEREE = args.engine.resolve(), args.referee.resolve()
    unittest.main(argv=[sys.argv[0]] + remaining)
