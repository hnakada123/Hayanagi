#!/usr/bin/env python3
"""Legal replay of ordinary-search failures, with full repetition history."""
import argparse
import json
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_book import digest_file, parse_search
from match_book import Player, Referee, position_command, write_json

CASES = {c['id']: c for c in json.loads(
    Path(__file__).with_name('fixtures').joinpath('search_regressions.json').read_text())}
REPORTS = []


class SearchRegressions(unittest.TestCase):
    def search(self, case, threads=1, nodes=None, extra=''):
        nodes = case['nodes'] if nodes is None else nodes
        with Player(ARGS.engine, analysis=True) as engine:
            engine.send(f'setoption name Threads value {threads}')
            engine.send(position_command(case['history']))
            engine.send(f'go nodes {nodes} {extra}')
            lines = engine.until('bestmove ')
        move = lines[-1].split()[1]
        scores = [score for moves in parse_search(lines).values() for score in moves.values()]
        REPORTS.append({'case': case['id'], 'threads': threads, 'nodes': nodes,
                        'extra': extra, 'bestmove': move, 'output': lines})
        with Referee([str(ARGS.referee.resolve())]) as ref:
            root = ref.state(position_command(case['history']))
            self.assertIn(move, root['legal'])
            for score in scores:
                ref.state(position_command(case['history']))
                for pv_move in score['pv']:
                    state = ref.state('move ' + pv_move)
                if score['score_type'] == 'mate' and score['score_value'] > 0:
                    self.assertEqual(state['reason'], 'checkmate')
            state = ref.state(position_command(case['history'] + [move]))
            self.assertNotEqual(state['outcome'], 'win', 'Selected an immediate rules loss')
        for line in lines:
            count = re.search(r' nodes (\d+)', line)
            if count:
                # Parallel accounting flushes at most one partial batch per worker.
                self.assertLessEqual(int(count[1]), nodes + threads * 256)
        return move, scores

    def test_perpetual_check_is_not_mate(self):
        case = CASES['perpetual_check']
        with Referee([str(ARGS.referee.resolve())]) as ref:
            bad = ref.state(position_command(case['history'] + [case['old_move']]))
            self.assertEqual((bad['outcome'], bad['reason']), ('win', 'perpetual_check'))
        for threads in (1, 4):
            with self.subTest(threads=threads):
                move, _ = self.search(case, threads)
                self.assertNotEqual(move, case['old_move'])

    def test_avoids_mate_in_one_even_at_depth_one(self):
        case = CASES['mate_in_one']
        with Referee([str(ARGS.referee.resolve())]) as ref:
            bad = ref.state(position_command(case['history'] + [case['old_move'], 'R*7h']))
            self.assertEqual((bad['outcome'], bad['reason']), ('loss', 'checkmate'))
        for threads in (1, 4):
            with self.subTest(threads=threads):
                move, scores = self.search(case, threads, extra='depth 1')
                self.assertTrue(scores)
                self.assertNotEqual(move, case['old_move'])
                self.assertEqual(max(s['depth'] for s in scores), 1)
                with Referee([str(ARGS.referee.resolve())]) as ref:
                    history = case['history'] + [move]
                    state = ref.state(position_command(history))
                    for reply in state['legal']:
                        after = ref.state(position_command(history + [reply]))
                        self.assertFalse(after['outcome'] == 'loss' and after['reason'] == 'checkmate', reply)

    def test_quiescence_leaves_budget_for_a_complete_iteration(self):
        for threads in (1, 4):
            with self.subTest(threads=threads):
                _, scores = self.search(CASES['quiescence_budget'], threads)
                self.assertTrue(scores, 'No root score at 50,000 nodes')
                self.assertGreaterEqual(max(s['depth'] for s in scores), 1)

    def test_avoids_exposed_knight_and_blocked_rook_retreat(self):
        for name in ('bishop_drop', 'rook_retreat'):
            with self.subTest(case=name):
                case = CASES[name]
                move, scores = self.search(case)
                self.assertTrue(scores)
                self.assertNotEqual(move, case['old_move'])
                # Fixed-node single-thread search must remain deterministic.
                repeated, repeated_scores = self.search(case)
                self.assertEqual(repeated, move)
                self.assertEqual(repeated_scores, scores)

    def test_tiny_budget_and_restricted_fallback(self):
        case = CASES['mate_in_one']
        for threads in (1, 4):
            for nodes in (1, 32, 256):
                with self.subTest(threads=threads, nodes=nodes):
                    move, _ = self.search(case, threads, nodes, 'searchmoves G*8h')
                    self.assertEqual(move, 'G*8h')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--referee', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    ARGS, remaining = parser.parse_known_args()
    result = unittest.main(argv=[sys.argv[0]] + remaining, verbosity=2, exit=False).result
    if ARGS.report:
        write_json(ARGS.report, {'engine_sha256': digest_file(ARGS.engine),
                                 'referee_sha256': digest_file(ARGS.referee),
                                 'success': result.wasSuccessful(), 'searches': REPORTS})
    sys.exit(0 if result.wasSuccessful() else 1)
