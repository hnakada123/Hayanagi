#!/usr/bin/env python3
"""３条件比較の時計処理、先後交換、再開を検証する。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import compare_books as league
import validate_book_comparison as audit
import validate_generated_book as book_audit
from build_book import START_KEY, digest_file


class LeagueTests(unittest.TestCase):
    def test_clock_does_not_round_elapsed_time_before_charging(self):
        player = argparse.Namespace(send=lambda line: None,
                                    until=lambda prefix, timeout: ['bestmove 7g7f'])
        with patch('match_book.time.monotonic', side_effect=[10.0, 10.0010001]):
            result = league.Player.search_command(player, [], 'go btime 100 wtime 100 byoyomi 20')
        self.assertGreater(result['elapsed_ms'], 1)
        self.assertEqual(league.charge_clock(100, result['elapsed_ms'], 20)[1], 2)

    def test_generated_book_provenance_and_ponder(self):
        with tempfile.TemporaryDirectory() as directory:
            book = Path(directory) / 'book.db'
            entry = {'move': '7g7f', 'ponder': '3c3d', 'score': 0, 'depth': 0, 'count': 3, 'pair_count': 2}
            report = {'positions': 1, 'entries': 1, 'import_settings': {'max_ply': 30},
                      'export_settings': {'min_count': 3, 'min_pairs': 2}}
            def write():
                book.write_text('#YANEURAOU-DB2016 1.00\n# HAYANAGI_MAX_PLY 30\n'
                                f'sfen {START_KEY} 1\n7g7f {entry["ponder"]} 0 0 3\n')
                report['book_sha256'] = digest_file(book)
                league.write_json(book.with_suffix('.json'), report)
                book.with_suffix('.positions.jsonl').write_text(json.dumps(
                    {'sfen': START_KEY + ' 1', 'history': [], 'entries': [entry], 'analysis': None}) + '\n')
            write()
            args = argparse.Namespace(book=book, referee=REFEREE, workers=1)
            self.assertEqual(book_audit.run(args)['checked_candidates_and_ponder'], 1)
            entry['ponder'] = '7g7f'
            write()
            with self.assertRaisesRegex(ValueError, 'Illegal book ponder'):
                book_audit.run(args)

    def test_byoyomi_is_not_added_to_remaining_main_time(self):
        self.assertEqual(league.charge_clock(1000, 3.1, 100), (996, 4, False))
        self.assertEqual(league.charge_clock(50, 120, 100), (0, 120, False))
        self.assertEqual(league.charge_clock(0, 100, 100), (0, 100, False))
        self.assertEqual(league.charge_clock(0, 100.1, 100), (0, 101, True))

    def test_all_pairings_and_clock_subset(self):
        suite = {'openings': [{'id': 'startpos', 'group': 'startpos', 'moves': []},
                             {'id': 'holdout_001', 'group': 'holdout', 'moves': []},
                             {'id': 'holdout_002', 'group': 'holdout', 'moves': []}]}
        jobs = league.jobs_for(suite, 50000, 1)
        self.assertEqual(len(jobs), 30)
        for mode, expected in [('nodes', 18), ('clock', 12)]:
            group = [j for j in jobs if j['mode'] == mode]
            self.assertEqual(len(group), expected)
            self.assertEqual({(j['a'], j['b']) for j in group}, set(league.PAIRS))
            self.assertEqual({j['a_side'] for j in group}, {'b', 'w'})
        suite['openings'][1]['id'] = '../escape'
        with self.assertRaises(ValueError):
            league.jobs_for(suite, 50000, 1)

    def test_small_league_and_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'tiny.db'
            book.write_text('#YANEURAOU-DB2016 1.00\n# HAYANAGI_MAX_PLY 1\n'
                            f'sfen {START_KEY} 1\n7g7f none 0 0 1\n')
            opening = root / 'openings.json'
            opening.write_text(json.dumps({'openings': [{'id': 'holdout_001', 'group': 'holdout',
                                                        'moves': [], 'sfen': START_KEY + ' 1'}]}))
            command = [sys.executable, '-B', str(ROOT / 'tools/compare_books.py'), '--engine', str(ENGINE),
                       '--referee', str(REFEREE), '--current', str(book), '--candidate', str(book),
                       '--openings', str(opening), '--output', str(root / 'run'), '--nodes', '200',
                       '--main-ms', '100', '--byoyomi-ms', '20', '--margin-ms', '10',
                       '--clock-pairs', '1', '--max-plies', '4', '--node-workers', '2', '--clock-workers', '1']
            subprocess.run(command, check=True, capture_output=True)
            result = league.read_json(root / 'run/summary.json')
            self.assertEqual(result['completed_games'], 12)
            self.assertTrue(result['complete'])
            options = argparse.Namespace(output=root / 'run', current=book, candidate=book,
                                         referee=REFEREE, workers=2, archive_games=True)
            original_validation = audit.run(options)
            self.assertEqual(original_validation['games'], 12)
            self.assertTrue((root / 'run/games.zip').is_file())
            previous = root / 'prior.json'
            previous.write_text(json.dumps({'request': {'history': []}}))
            options.prior_analysis = [previous]
            novelty = audit.novelty_report(options, league.read_json(opening),
                [league.read_json(p) for p in (root / 'run/games').glob('*.json')])
            self.assertEqual(novelty['overlapping_ids'], ['holdout_001'])
            self.assertEqual(novelty['eligible_pair_count'], 0)
            self.assertTrue(all(s['games'] == 0 for s in novelty['comparisons'].values()))
            for stats in result['comparisons'].values():
                self.assertEqual(stats['holdout']['complete_pairs'], 1)
            for path in (root / 'run/games').glob('*.json'):
                game = league.read_json(path)
                remaining = {'b': 100, 'w': 100}
                for event in game['events']:
                    if event['variant'] == 'none': self.assertFalse(event['book_hit'])
                    if game['mode'] == 'clock':
                        self.assertEqual(event['clock_before_ms'], remaining)
                        remaining[event['side']], spent, expired = league.charge_clock(
                            remaining[event['side']], event['elapsed_ms'], 20)
                        self.assertEqual(event['clock_after_ms'], remaining)
                        self.assertEqual(event['charged_ms'], spent)
                        self.assertEqual(event['accepted'], not expired)
            again = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(again.stdout.count('0 pending'), 2)

            # 時計と定跡ヒットの改ざんを、それぞれ再生検証が拒否する。
            manifest = league.read_json(root / 'run/manifest.json')
            jobs = league.jobs_for(league.read_json(opening), 200, 1)
            job = next(j for j in jobs if j['mode'] == 'nodes')
            game = league.read_json(root / 'run/games' / (job['id'] + '.json'))
            books = {'current': audit.read_book(book), 'candidate': audit.read_book(book)}
            bad = copy.deepcopy(game)
            bad['events'][0]['book_hit'] = False
            with self.assertRaisesRegex(ValueError, 'incorrect book hit'):
                audit.verify(options, bad, job, manifest, books)

            # 最初の指し手が時間切れなら盤面へ適用せず、その手番の負けにする。
            timed = copy.deepcopy(game)
            timed_job = dict(job, mode='clock', nodes=None, id='synthetic_timeout')
            timed.update({k: timed_job[k] for k in ('id', 'mode', 'nodes')})
            event = timed['events'][0]
            event.update(elapsed_ms=120.1, accepted=False, charged_ms=121,
                         clock_before_ms={'b': 100, 'w': 100}, clock_after_ms={'b': 0, 'w': 100},
                         command='go btime 100 wtime 100 byoyomi 20')
            timed.update(events=[event], moves=[], plies=0, final_sfen=START_KEY + ' 1',
                         terminal_reason='time_forfeit', remaining_ms={'b': 0, 'w': 100}, a_score=0.0)
            self.assertEqual(audit.verify(options, timed, timed_job, manifest, books)['reason'], 'time_forfeit')
            event['clock_after_ms']['b'] = 1
            with self.assertRaisesRegex(ValueError, 'wrong clock charge'):
                audit.verify(options, timed, timed_job, manifest, books)
            (root / 'run/games').rename(root / 'run/loose-games-backup')
            options.archive_games = False
            restored = audit.run(options)
            self.assertEqual(restored['game_files_fingerprint_sha256'], original_validation['game_files_fingerprint_sha256'])
            self.assertEqual(restored['games_archive_sha256'], original_validation['games_archive_sha256'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', type=Path, default=ROOT / 'build/hayanagi')
    parser.add_argument('--referee', type=Path, default=ROOT / 'build/hayanagi_match_referee')
    args, remaining = parser.parse_known_args()
    ENGINE, REFEREE = args.engine.resolve(), args.referee.resolve()
    unittest.main(argv=[sys.argv[0]] + remaining)
