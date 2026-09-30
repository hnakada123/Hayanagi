#!/usr/bin/env python3
"""Checks for move-attribution selection and exact, matched-depth scoring."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import diagnose_book as d


def score(value, kind='cp'):
    return {'score': value if kind == 'cp' else (30000-abs(value))*(1 if value > 0 else -1),
            'score_type': kind, 'score_value': value}


class DiagnosisTests(unittest.TestCase):
    def test_only_shared_complete_depth_and_exact_scores_are_used(self):
        lines = ['info depth 5 score cp 100 pv 7g7f 3c3d',
                 'info depth 5 score cp 50 pv 2g2f 8c8d',
                 'info depth 6 score cp 300 pv 7g7f 3c3d',
                 'info depth 6 score cp 250 lowerbound pv 2g2f 8c8d']
        result = d.complete_scores(lines, ['7g7f', '2g2f'])
        self.assertEqual(result['depth'], 5)
        self.assertEqual(d.gap_result(result, '2g2f')['gap_cp'], 50)
        with self.assertRaises(RuntimeError):
            d.complete_scores(lines, ['7g7f', '5g5f'])

    def test_scores_are_mover_relative_even_when_negative(self):
        r = {'depth': 8, 'scores': {'played': score(-500), 'better': score(-100)}}
        gap = d.gap_result(r, 'played')
        self.assertEqual(gap['gap_cp'], 400)
        self.assertTrue(d.significant(gap, 200))
        self.assertEqual(d.gap_result(r, 'better')['gap_cp'], 0)

    def test_mates_are_not_subtracted_as_cp(self):
        r = {'depth': 8, 'scores': {'played': score(-5, 'mate'), 'better': score(-100)}}
        gap = d.gap_result(r, 'played')
        self.assertIsNone(gap['gap_cp'])
        self.assertTrue(gap['avoids_losing_mate'])
        self.assertTrue(d.significant(gap, 200))

    def test_trace_selection_excludes_unexposed_losses_and_includes_book_wins(self):
        def game(gid, points, hits):
            return {'id': gid, 'nodes_per_move': 50000, 'book_score': points,
                    'book_hit_plies': hits, 'permanent_exit_ply': 3 if hits else None,
                    'moves': ['7g7f', '3c3d', '2g2f', '8c8d', '2f2e', '8d8e'],
                    'events': [{'player': 'book', 'ply': ply, 'move': move,
                                'book_hit': ply in hits, 'score': None}
                               for ply, move in [(1, '7g7f'), (3, '2g2f'), (5, '2f2e')]]}
        plan = d.make_plan([game('hit_loss', 0, [1]), game('no_hit_loss', 0, []),
                            game('hit_win', 1, [1])], [])
        self.assertEqual(plan['loss_games_50000'], ['hit_loss'])
        self.assertNotIn('no_hit_loss', plan['trajectories'])
        self.assertTrue(any(o['game_id']=='hit_win' and o['book_hit'] for o in plan['occurrences']))

    def test_trajectory_nomination_avoids_already_lost_positions(self):
        trace = [{'game_id': 'g', 'history_id': h, 'ply': ply, 'move': 'x',
                  'book_hit': False, 'exit_ply': 3} for h, ply in [('a',3),('b',5),('c',7),('d',9)]]
        plan = {'occurrences': trace, 'trajectories': {'g': trace}, 'early_drop_games': []}
        scan = {h: {'scores': {'x': score(v)}} for h,v in [('a',20),('b',10),('c',-300),('d',-2000)]}
        selected = d.select_comparisons(plan, scan)
        nominees = {s['history_id'] for s in selected if 'loss_trajectory_drop' in s['reasons']}
        self.assertEqual(nominees, {'b'})

    def test_missing_scan_scores_are_not_treated_as_zero(self):
        trace = [{'game_id': 'g', 'history_id': h, 'ply': ply, 'move': 'x',
                  'book_hit': False, 'exit_ply': 3} for h, ply in [('a',3),('b',5)]]
        plan = {'occurrences': trace, 'trajectories': {'g': trace}, 'early_drop_games': []}
        scan = {'a': {'scores': {}}, 'b': {'scores': {'x': score(-500)}}}
        selected = d.select_comparisons(plan, scan)
        self.assertFalse(any('loss_trajectory_drop' in s['reasons'] for s in selected))


if __name__ == '__main__':
    unittest.main()
