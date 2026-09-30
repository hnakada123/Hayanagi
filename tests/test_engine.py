#!/usr/bin/env python3
"""USI and tsume regression tests; Python standard library only.

Usage: python3 tests/test_engine.py build/hayanagi [--baseline OLD_ENGINE]
"""
import argparse
from pathlib import Path
import queue
import re
import subprocess
import threading
import time
import tempfile
import unittest

PROBLEMS = [
    ('9/9/6R1+R/5k3/9/7+S1/9/9/9 b 2b4g3s4n4l18p 1',
     '3c5c+ 4d4e 1c4c P*4d 4c4d'),
    ('9/9/9/R8/9/9/1k2+S4/9/3+N5 b RG2b3g3s3n4l18p 1',
     'G*9g 8g8h R*8g 8h9i 9g9h'),
    ('9/9/9/9/9/9/7+S1/5G2k/9 b RSr2b3g2s4n4l18p 1',
     'R*1g 1h2i S*3h 2i3i 4h4i'),
    ('5k3/9/9/3+P1B1N1/9/9/9/9/9 b RSrb4g3s3n4l17p 1',
     'S*3b 4a4b R*4a 4b5b 4d5c+'),
    ('1l7/k8/9/G8/3+R5/9/9/9/9 b R2b3g4s4n3l18p 1',
     'R*9c 9b8b 6e6b P*7b 9d8c'),
]


class Engine:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding='utf-8', bufsize=1)
        self.output = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.output.put(line.strip())
        self.output.put(None)

    def send(self, commands):
        self.process.stdin.write(commands + '\n')
        self.process.stdin.flush()

    def until(self, prefix, timeout=15):
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f'Timeout waiting for {prefix!r}: {lines}')
            try:
                line = self.output.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError(f'Timeout waiting for {prefix!r}: {lines}') from None
            if line is None:
                raise AssertionError(f'Engine exited waiting for {prefix!r}: {lines}')
            lines.append(line)
            if line.startswith(prefix):
                return lines

    def ready(self):
        self.send('isready')
        return self.until('readyok')

    def perft(self, depth, divide=False):
        self.send(f'perft depth {depth}' + (' divide' if divide else ''))
        lines = self.until('info string perft depth ')
        fields = lines[-1].split()[3:]
        stats = dict(zip(fields[::2], fields[1::2]))
        return lines, {key: int(stats[key]) for key in
                       ('nodes', 'captures', 'promotions', 'checks', 'mates')}

    def close(self):
        try:
            if self.process.poll() is None:
                self.send('quit')
                self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.reader.join(timeout=5)
            self.process.stdin.close()
            self.process.stdout.close()
        if self.process.returncode != 0:
            diagnostics = []
            while not self.output.empty():
                line = self.output.get_nowait()
                if line is not None:
                    diagnostics.append(line)
            raise AssertionError(f'Engine exited with {self.process.returncode}: {diagnostics[-30:]}')


class EngineTests(unittest.TestCase):
    executable = None
    baseline = None
    threads = 1

    def setUp(self):
        self.engine = Engine(self.executable)
        self.addCleanup(self.engine.close)
        self.engine.send('usi')
        self.usi_lines = self.engine.until('usiok')
        self.engine.send('setoption name USI_OwnBook value false')
        self.engine.send(f'setoption name Threads value {self.threads}')
        self.engine.ready()

    def tsume_mode(self):
        self.engine.send('setoption name TsumeMode value true')

    def solve(self, sfen, side='attack', depth=5, moves='', millis=5000):
        self.engine.send('position sfen ' + sfen + (' moves ' + moves if moves else ''))
        self.engine.send(f'go tsume {side} depth {depth} movetime {millis}')
        line = self.engine.until('tsume ')[-1]
        fields = line.split()
        self.assertGreaterEqual(len(fields), 2, line)
        return fields[1], dict(zip(fields[2::2], fields[3::2]))

    def test_usi_and_startpos_perft(self):
        self.assertIn('option name TsumeMode type check default false', self.usi_lines)
        names = [line for line in self.usi_lines if line.startswith('id name ')]
        self.assertEqual(len(names), 1, self.usi_lines)
        self.assertRegex(names[0], r'^id name Hayanagi \d+\.\d+\.\d+$')
        self.engine.send('usinewgame\nposition startpos')
        for depth, nodes in ((1, 30), (2, 900), (3, 25470)):
            with self.subTest(depth=depth):
                _, stats = self.engine.perft(depth)
                self.assertEqual(stats['nodes'], nodes)

    def test_normal_game_moves_are_legal(self):
        history = ['7g7f', '3c3d', '2g2f', '8c8d']
        for ply in range(12):
            with self.subTest(ply=ply):
                self.engine.send('position startpos moves ' + ' '.join(history))
                lines, _ = self.engine.perft(1, divide=True)
                legal = {line.split(':')[0] for line in lines
                         if re.match(r'^(?:[1-9][a-i][1-9][a-i]\+?|[PLNSGBR]\*[1-9][a-i]):', line)}
                self.assertTrue(legal)
                self.engine.send('go nodes 2000 movetime 1000')
                bestmove = self.engine.until('bestmove ')[-1].split()[1]
                self.assertIn(bestmove, legal)
                history.append(bestmove)

    def test_normal_perft_matches_baseline(self):
        if self.baseline is None:
            self.skipTest('Pass --baseline to compare with the previous engine')
        old = Engine(self.baseline)
        self.addCleanup(old.close)
        positions = [
            'position startpos',
            'position startpos moves 7g7f 3c3d 2g2f 8c8d',
            'position startpos moves 7g7f 3c3d 8h2b+ 3a2b 2g2f',
            'position sfen 4k4/9/9/9/4R4/9/9/9/4K4 w G2P 1',
        ]
        for position in positions:
            with self.subTest(position=position):
                old.send(position)
                self.engine.send(position)
                self.assertEqual(old.perft(3)[1], self.engine.perft(3)[1])

    def test_single_thread_search_matches_baseline(self):
        if self.baseline is None:
            self.skipTest('Pass --baseline to compare search results')
        old = Engine(self.baseline)
        self.addCleanup(old.close)
        positions = ['position startpos',
                     'position startpos moves 7g7f 3c3d 8h2b+ 3a2b 2g2f',
                     'position sfen 4k4/9/9/9/4R4/9/9/9/4K4 w G2P 1']
        for position in positions:
            output = []
            for engine in (old, self.engine):
                engine.send('setoption name Threads value 1\nsetoption name Hash value 16\n'
                            'setoption name USI_OwnBook value false\n' + position + '\ngo depth 4')
                output.append([re.sub(r' time \d+ nps \d+| seldepth \d+| hashfull \d+| cpuload \d+', '', line)
                               for line in engine.until('bestmove ')])
            self.assertEqual(output[0], output[1], position)

    def test_five_problems_and_reference_solutions(self):
        self.tsume_mode()
        for sfen, sequence in PROBLEMS:
            with self.subTest(sfen=sfen):
                status, result = self.solve(sfen)
                self.assertEqual(status, 'mate')
                self.assertEqual(result['plies'], '5')
                moves = sequence.split()
                for count in (1, 3, 5):
                    status, result = self.solve(sfen, side='defense', depth=5 - count,
                                                moves=' '.join(moves[:count]))
                    self.assertEqual(status, 'mate')
                    self.assertLessEqual(int(result['plies']), 5 - count)
                    if count == 5:
                        self.assertEqual(result['move'], 'none')

    def test_alternative_and_escape(self):
        self.tsume_mode()
        status, result = self.solve(PROBLEMS[1][0], 'defense', 4, 'R*9g')
        self.assertEqual(status, 'mate')
        self.assertEqual(result['plies'], '4')
        status, result = self.solve(PROBLEMS[2][0], 'defense', 4, 'R*1i')
        self.assertEqual(status, 'nomate')
        self.assertEqual(result['move'], '1h1i')

    def test_depth_limit_is_not_nomate(self):
        self.tsume_mode()
        status, result = self.solve(PROBLEMS[3][0], 'defense', 4, 'S*4b')
        self.assertEqual(status, 'depthlimit')
        self.assertNotEqual(result['move'], 'none')
        status, result = self.solve(PROBLEMS[3][0], 'defense', 6, 'S*4b')
        self.assertEqual(status, 'mate')
        self.assertEqual(result['plies'], '6')

    def test_white_attacker_and_pawn_drop_mate(self):
        self.tsume_mode()
        sfen = '8k/6G2/7G1/9/9/9/9/9/9 b PR 1'
        status, _ = self.solve(sfen, 'defense', 1, 'P*1b')
        self.assertEqual(status, 'invalid')
        status, result = self.solve(sfen, 'defense', 1, 'R*1b')
        self.assertEqual(status, 'mate')
        self.assertEqual(result['plies'], '0')
        status, result = self.solve('9/9/9/9/9/9/1g7/2g6/K8 w r 1', depth=1)
        self.assertEqual(status, 'mate')
        self.assertEqual(result['plies'], '1')

    def test_invalid_sfen_and_mode_isolation(self):
        # Missing attacker king is accepted only when explicitly enabled.
        self.assertEqual(self.solve(PROBLEMS[0][0])[0], 'invalid')
        self.tsume_mode()
        for sfen in (
            '9/9/9/9/9/9/9/9/9 b - 1',
            'kk7/9/9/9/9/9/9/9/9 b - 1',
            'k8/9/9/9/9/9/9/9/9 b 999999999999P 1',
            'k8/9/9/9/9/9/9/9/+9 b - 1',
            'k8/9/9/9/9/9/9/9/9 b - 0',
        ):
            with self.subTest(sfen=sfen):
                self.assertEqual(self.solve(sfen)[0], 'invalid')
        self.engine.send('position sfen ' + PROBLEMS[0][0] + '\ngo depth 1')
        self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')
        self.engine.send('setoption name TsumeMode value false\nusinewgame\nposition startpos')
        self.assertEqual(self.engine.perft(1)[1]['nodes'], 30)

    def test_timeout_stop_and_new_position(self):
        self.tsume_mode()
        self.assertEqual(self.solve(PROBLEMS[0][0], 'defense', 30, '3c4c', 1)[0], 'timeout')
        self.engine.send('go tsume defense depth 30 movetime 600000\nstop')
        self.assertTrue(self.engine.until('tsume ')[-1].startswith('tsume cancelled '))
        self.engine.send('go tsume defense depth 30 movetime 600000')
        # Replacing the position cancels the old search without publishing its result.
        self.assertEqual(self.solve(PROBLEMS[0][0])[0], 'mate')
        self.engine.send('go tsume attack depth 31 movetime 600000\nquit')
        self.engine.process.wait(timeout=5)
        self.assertEqual(self.engine.process.returncode, 0)

    def test_parallel_perft_preserves_divide_and_statistics(self):
        for position in ('position startpos',
                         'position startpos moves 7g7f 3c3d 8h2b+ 3a2b 2g2f',
                         'position sfen 4k4/9/9/9/4R4/9/9/9/4K4 w G2P 1'):
            self.engine.send(position)
            for depth in (0, 1, 3):
                self.engine.send('setoption name Threads value 1')
                expected_lines, expected = self.engine.perft(depth, divide=True)
                for threads in (2, 4, 8, 1):
                    with self.subTest(position=position, depth=depth, threads=threads):
                        self.engine.send(f'setoption name Threads value {threads}')
                        lines, actual = self.engine.perft(depth, divide=True)
                        self.assertEqual(actual, expected)
                        self.assertEqual(lines[:-1], expected_lines[:-1])
                        self.assertEqual(self.engine.perft(depth)[1], expected)

    def test_parallel_search_reuse_multipv_and_stop(self):
        for threads in (4, 2, 8, 1, 4):
            self.engine.send(f'setoption name Threads value {threads}\nposition startpos')
            self.engine.send('setoption name MultiPV value 3\ngo depth 3')
            lines = self.engine.until('bestmove ')
            for pv in (1, 2, 3):
                self.assertTrue(any('depth 3 ' in line and f'multipv {pv} ' in line
                                    for line in lines), lines)
            self.engine.send('go infinite')
            self.engine.until('info depth ')
            self.engine.send('stop')
            self.engine.until('bestmove ', timeout=3)
            self.engine.send('setoption name MultiPV value 1\ngo nodes 1000')
            self.engine.until('bestmove ')
        self.engine.send('setoption name USI_Ponder value true\ngo ponder depth 2')
        self.engine.until('info depth 2 ')
        self.engine.send('ponderhit')
        self.engine.until('bestmove ', timeout=3)

    def test_parallel_mate_precheck_preserves_result(self):
        # 通常探索にも攻方玉を含む詰み局面を渡し、事前の王手探索を通す。
        for problem, _ in PROBLEMS[:4]:
            board, rest = problem.split(' ', 1)
            ranks = board.split('/')
            ranks[-1] = '4K4' if ranks[-1] == '9' else '3+NK4'
            self.engine.send('position sfen ' + '/'.join(ranks) + ' ' + rest)
            reference = None
            for threads in (1, 4, 2):
                self.engine.send(f'setoption name Threads value {threads}\ngo depth 5')
                lines = self.engine.until('bestmove ')
                self.assertTrue(any('score mate 5 ' in line for line in lines), lines)
                if reference is None:
                    reference = lines[-1]
                self.assertEqual(lines[-1], reference)

    def test_parallel_node_totals_include_unflushed_batches(self):
        self.engine.send('setoption name MultiPV value 30\nposition startpos')
        for threads in (1, 4, 8):
            self.engine.send(f'setoption name Threads value {threads}\ngo depth 1')
            lines = self.engine.until('bestmove ')
            counts = [int(re.search(r' nodes (\d+)', line).group(1))
                      for line in lines if line.startswith('info depth ')]
            # 30 fallback evaluations plus 30 depth-1 leaves. Every worker's
            # partial batch and the coordinator's fallback work must be counted.
            self.assertEqual(counts, [60] * 30)

    def assert_no_result(self, timeout=0.1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.engine.output.get(timeout=max(0.001, deadline - time.monotonic()))
            except queue.Empty:
                return
            self.assertIsNotNone(line)
            self.assertFalse(line.startswith(('bestmove ', 'checkmate ', 'tsume ')), line)

    def test_standard_hash_and_crlf(self):
        self.engine.send('  setoption name USI_Hash value 32\r\nusi\r')
        lines = self.engine.until('usiok')
        for name in ('Hash', 'USI_Hash'):
            self.assertIn(f'option name {name} type spin default 32 min 1 max 65536', lines)
        self.engine.send('setoption name Hash value 16\nusi')
        self.assertIn('option name USI_Hash type spin default 16 min 1 max 65536',
                      self.engine.until('usiok'))

    def test_standard_mate_returns_complete_legal_line(self):
        # 標準 go mate は TsumeMode を設定せず、攻方玉なしでも使える。
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}')
            for sfen, _ in PROBLEMS:
                with self.subTest(threads=threads, sfen=sfen):
                    self.engine.send('position sfen ' + sfen + '\ngo mate 5000')
                    lines = self.engine.until('checkmate ')
                    self.assertFalse(any(line.startswith('bestmove ') for line in lines), lines)
                    moves = lines[-1].split()[1:]
                    self.assertEqual(len(moves), 5, lines)
                    # 各着手が合法であることを position の適用と終端の詰みで検証する。
                    self.engine.send('setoption name TsumeMode value true')
                    status, result = self.solve(sfen, 'defense', 1, ' '.join(moves))
                    self.assertEqual((status, result.get('plies')), ('mate', '0'))
                    self.engine.send('setoption name TsumeMode value false')
        self.engine.send('position sfen 9/9/9/9/9/9/1g7/2g6/K8 w r 1\ngo mate infinite')
        self.assertRegex(self.engine.until('checkmate ')[-1], r'^checkmate \S+$')

    def test_standard_mate_nomate_timeout_and_stop(self):
        self.engine.send('position startpos\ngo mate infinite')
        self.assertEqual(self.engine.until('checkmate ')[-1], 'checkmate nomate')
        # 0 は探索手数ではなく、即時に期限を迎えるミリ秒指定。
        self.engine.send('position sfen ' + PROBLEMS[0][0] + '\ngo mate 0')
        self.assertEqual(self.engine.until('checkmate ')[-1], 'checkmate timeout')
        # 深さ 5 では未解決の局面。stop には bestmove ではなく checkmate を返す。
        difficult = 'position sfen 9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1'
        self.engine.send(difficult + '\ngo mate 1')
        self.assertEqual(self.engine.until('checkmate ')[-1], 'checkmate timeout')
        self.engine.send(difficult + '\ngo mate infinite\nstop')
        lines = self.engine.until('checkmate ', timeout=3)
        self.assertEqual(lines[-1], 'checkmate timeout')
        self.assertFalse(any(line.startswith('bestmove ') for line in lines))
        self.engine.send(difficult + '\ngo mate 600000\nposition startpos\ngo mate 1000')
        self.assertEqual(self.engine.until('checkmate '), ['checkmate nomate'])

    def test_signed_mate_scores_and_search_information(self):
        position = 'position sfen 9/9/6R1+R/5k3/9/7+S1/9/9/4K4 b 2b4g3s4n4l18p 1'
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}\n' + position + '\ngo depth 5')
            lines = self.engine.until('bestmove ')
            self.assertTrue(any('score mate 5 ' in line for line in lines), lines)
            self.engine.send(position + ' moves 3c5c+ 4d4e 1c4c\ngo depth 2')
            lines = self.engine.until('bestmove ')
            self.assertTrue(any('score mate -2 ' in line for line in lines), lines)
            for line in lines[:-1]:
                depth = re.search(r'depth (\d+) seldepth (\d+)', line)
                self.assertIsNotNone(depth, line)
                self.assertGreaterEqual(int(depth[2]), int(depth[1]))
                fullness = re.search(r' hashfull (\d+)', line)
                self.assertIsNotNone(fullness, line)
                self.assertIn(int(fullness[1]), range(1001))

    def test_infinite_and_ponder_wait_even_after_mate(self):
        positions = [
            'position sfen 9/9/6R1+R/5k3/9/7+S1/9/9/4K4 b 2b4g3s4n4l18p 1',
            'position sfen 8k/6G2/7G1/9/9/9/9/9/4K4 b R 1 moves R*1b',
        ]
        for position in positions:
            for command, finish in (('go infinite', 'stop'), ('go ponder depth 5', 'ponderhit')):
                with self.subTest(position=position, command=command):
                    self.engine.send(position + '\n' + command)
                    lines = self.engine.until('info depth ')
                    self.assertFalse(any(line.startswith('bestmove ') for line in lines), lines)
                    self.assert_no_result()
                    self.engine.send(finish)
                    self.engine.until('bestmove ', timeout=3)
                    self.assert_no_result(0.02)

    def test_gameover_cancels_search_and_pending_ponder(self):
        for outcome in ('win', 'lose', 'draw'):
            self.engine.send('position startpos\ngo ponder depth 1')
            self.engine.until('info depth 1 ')
            self.engine.send(f'gameover {outcome}\nponderhit\nisready')
            self.assertEqual(self.engine.until('readyok'), ['readyok'])
            self.assert_no_result(0.02)
        self.engine.send('position sfen 9/9/9/9/9/2k6/4r4/9/9 b RBSLb4g3s4n3l18p 1\ngo mate infinite')
        self.engine.send('gameover lose\nisready')
        self.assertEqual(self.engine.until('readyok'), ['readyok'])
        self.engine.send('usinewgame\nposition startpos\ngo depth 1')
        self.assertNotEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')

    def test_book_and_infinite_with_directory_spaces(self):
        with tempfile.TemporaryDirectory(prefix='hayanagi book ') as directory:
            Path(directory, 'standard_book.db').write_text(
                '#YANEURAOU-DB2016 1.00\n'
                'sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1\n'
                '7g7f 3c3d 10 1 1\n', encoding='utf-8')
            self.engine.send(f'setoption name BookDir value {directory}\n'
                             'setoption name USI_OwnBook value true\nisready')
            self.assertTrue(any('book loaded ' in line for line in self.engine.until('readyok')))
            self.engine.send('position startpos\ngo depth 1')
            self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove 7g7f')
            self.engine.send('go infinite nodes 1000')
            self.engine.until('info depth ')
            self.assert_no_result()
            self.engine.send('stop')
            self.engine.until('bestmove ', timeout=3)

    def test_invalid_position_does_not_reuse_previous_position(self):
        self.engine.send('position startpos\nposition sfen invalid\ngo depth 1')
        self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')
        self.engine.send('go mate 1000')
        self.assertEqual(self.engine.until('checkmate ')[-1], 'checkmate timeout')

    def test_try_rule_win_is_not_reported_as_checkmate(self):
        self.engine.send('setoption name EnteringKingRule value TryRule\n'
                         'position sfen 9/4K4/9/9/9/9/9/9/k8 b - 1\ngo depth 2')
        lines = self.engine.until('bestmove ')
        self.assertEqual(lines[-1], 'bestmove 5b5a')
        self.assertTrue(any('score cp 28999 ' in line for line in lines), lines)
        self.assertFalse(any('score mate ' in line for line in lines), lines)

    def test_zero_remaining_time_and_large_clock_values(self):
        self.engine.send('position startpos\ngo btime 0 wtime 0 byoyomi 0')
        self.engine.until('bestmove ', timeout=3)
        self.engine.send('setoption name SlowMover value 1000\n'
                         'go btime 2147483647 wtime 2147483647 byoyomi 2147483647 nodes 1000')
        self.engine.until('bestmove ', timeout=3)

    def test_periodic_information_and_ready_during_search(self):
        self.engine.send('position startpos\ngo infinite')
        deadline = time.monotonic() + 5
        while True:
            lines = self.engine.until('info depth ', timeout=max(0.01, deadline - time.monotonic()))
            if ' currmove ' in lines[-1]:
                self.assertIn(' hashfull ', lines[-1])
                self.assertNotIn(' score ', lines[-1])
                break
            self.assertLess(time.monotonic(), deadline)
        # isready と探索情報の出力が混ざって壊れないことも確認する。
        self.engine.send('\n'.join(['isready'] * 30))
        for _ in range(30):
            self.assertEqual(self.engine.until('readyok')[-1], 'readyok')
        self.engine.send('stop')
        self.engine.until('bestmove ', timeout=3)

    def test_searchmoves_restriction_and_multipv(self):
        allowed = {'7g7f', '2g2f'}
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}\n'
                             'setoption name MultiPV value 32\nposition startpos\n'
                             'go searchmoves 7g7f 2g2f 7g7f invalid 9a9b depth 3')
            lines = self.engine.until('bestmove ')
            self.assertIn(lines[-1].split()[1], allowed)
            pvs = [line for line in lines if ' pv ' in line]
            self.assertEqual(len(pvs), 6, lines)
            self.assertEqual({line.split(' pv ')[1].split()[0] for line in pvs}, allowed)
            self.assertTrue(all(re.search(r' multipv [12] ', line) for line in pvs), lines)
            for empty in ('', 'invalid 9a9b'):
                self.engine.send('go depth 1 searchmoves ' + empty)
                self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')
            self.engine.send('go depth 1')
            self.assertNotEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')
        # 探索用の枝刈りで省略される不成も、明示的に指定した場合は探索する。
        self.engine.send('setoption name GenerateAllLegalMoves value false\n'
                         'position sfen 4k4/9/4P4/9/9/9/9/9/4K4 b - 1\n'
                         'go depth 1 searchmoves 5c5b')
        self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove 5c5b')

    def test_searchmoves_mate_precheck_and_early_stop(self):
        position = 'position sfen 9/9/6R1+R/5k3/9/7+S1/9/9/4K4 b 2b4g3s4n4l18p 1'
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}\n' + position +
                             '\ngo depth 5 nodes 100 searchmoves 1c1b')
            self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove 1c1b')
            self.engine.send('position startpos\ngo infinite searchmoves 2g2f\nstop')
            self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove 2g2f')

    def test_searchmoves_book_filter(self):
        with tempfile.TemporaryDirectory(prefix='hayanagi restricted book ') as directory:
            Path(directory, 'standard_book.db').write_text(
                '#YANEURAOU-DB2016 1.00\n'
                'sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1\n'
                '7g7f 3c3d 10 1 1\n', encoding='utf-8')
            self.engine.send(f'setoption name BookDir value {directory}\n'
                             'setoption name USI_OwnBook value true\nisready')
            self.engine.until('readyok')
            for move in ('7g7f', '2g2f'):
                self.engine.send(f'position startpos\ngo depth 1 searchmoves {move}')
                lines = self.engine.until('bestmove ')
                self.assertEqual(lines[-1], 'bestmove ' + move)
                self.assertEqual(any('book hit' in line for line in lines), move == '7g7f')

    def test_copyprotection_and_strength_options(self):
        for option in ('USI_LimitStrength', 'USI_AnalyseMode'):
            self.assertIn(f'option name {option} type check default false', self.usi_lines)
        self.assertIn('option name USI_Strength type spin default 1 min -15 max 6', self.usi_lines)
        self.engine.send('usi\nisready')
        lines = self.engine.until('readyok')
        index = lines.index('usiok')
        self.assertEqual(lines[index + 1:], ['copyprotection checking', 'copyprotection ok', 'readyok'])
        self.assertFalse(any(line.startswith('registration ') for line in lines))

    def test_strength_limits_and_explicit_limits(self):
        self.engine.send('debug on\nsetoption name USI_LimitStrength value true')
        # 範囲外は上下限へ丸め、0は1級相当、不正値は既定値に戻す。
        cases = [(-999, 1, 256), (-15, 1, 256), (-10, 2, 1536),
                 (-5, 4, 8192), (-1, 5, 32768), (0, 5, 32768),
                 (1, 6, 49152), (6, 7, 262144), (999, 7, 262144), ('invalid', 6, 49152)]
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}')
            for strength, depth, nodes in cases:
                with self.subTest(threads=threads, strength=strength):
                    self.engine.send(f'setoption name USI_Strength value {strength}\n'
                                     'position startpos\ngo depth 20 nodes 1000 searchmoves 7g7f 2g2f')
                    lines = self.engine.until('bestmove ')
                    self.assertTrue(any(f'depth_limit {depth} node_limit {min(nodes, 1000)}' in line
                                        for line in lines), lines)
                    infos = [line for line in lines if line.startswith('info depth ')]
                    self.assertTrue(infos, lines)
                    self.assertTrue(all(int(line.split()[2]) <= depth for line in infos), infos)
                    self.assertTrue(all(int(re.search(r' nodes (\d+)', line)[1]) <= min(nodes, 1000) + threads
                                        for line in infos), infos)
                    self.assertIn(lines[-1], ('bestmove 7g7f', 'bestmove 2g2f'))
        self.engine.send('go depth 1 nodes 1 movetime 1 searchmoves 2g2f')
        lines = self.engine.until('bestmove ')
        self.assertIn('time_limit_ms 1 ', '\n'.join(lines))
        self.assertIn('depth_limit 1 node_limit 1', '\n'.join(lines))
        self.assertEqual(lines[-1], 'bestmove 2g2f')

    def test_strength_disabled_and_waiting_modes(self):
        self.engine.send('setoption name USI_Strength value -15\nposition startpos\ngo depth 3')
        lines = self.engine.until('bestmove ')
        self.assertTrue(any(line.startswith('info depth 3 ') for line in lines), lines)
        self.engine.send('setoption name USI_LimitStrength value true')
        for command, finish in (('go infinite', 'stop'), ('go ponder depth 5', 'ponderhit')):
            self.engine.send('position startpos\n' + command + ' searchmoves 2g2f')
            self.engine.until('info depth 1 ')
            self.assertFalse(any(line.startswith('bestmove ') for line in self.engine.ready()))
            self.engine.send(finish)
            self.assertEqual(self.engine.until('bestmove ', timeout=3)[-1], 'bestmove 2g2f')
        # 専用の詰将棋解答には対局用の棋力制限をかけない。
        self.engine.send('position sfen ' + PROBLEMS[0][0] + '\ngo mate 5000')
        self.assertEqual(self.engine.until('checkmate ')[-1], 'checkmate ' + PROBLEMS[0][1])

    def test_analyse_and_strength_bypass_book_without_losing_settings(self):
        with tempfile.TemporaryDirectory(prefix='hayanagi analyse book ') as directory:
            Path(directory, 'standard_book.db').write_text(
                '#YANEURAOU-DB2016 1.00\n'
                'sfen lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1\n'
                '7g7f 3c3d 10 1 1\n', encoding='utf-8')
            self.engine.send(f'setoption name BookDir value {directory}\n'
                             'setoption name USI_OwnBook value true\n'
                             'setoption name USI_Strength value -15')
            self.engine.ready()
            cases = [(False, False, True, None), (True, True, False, 3),
                     (False, True, False, 1), (False, False, True, None)]
            for analyse, limited, book_hit, depth in cases:
                self.engine.send(f'setoption name USI_AnalyseMode value {str(analyse).lower()}\n'
                                 f'setoption name USI_LimitStrength value {str(limited).lower()}\n'
                                 'position startpos\ngo depth 3 searchmoves 7g7f')
                lines = self.engine.until('bestmove ')
                self.assertEqual(any('book hit' in line for line in lines), book_hit, lines)
                self.assertEqual(lines[-1], 'bestmove 7g7f')
                if depth is not None:
                    depths = [int(line.split()[2]) for line in lines if line.startswith('info depth ')]
                    self.assertEqual(max(depths), depth, lines)

    def test_analyse_resignation_ponder_and_option_snapshot(self):
        position = 'position sfen 4k4/9/9/9/4R4/9/9/9/4K4 w - 1'
        self.engine.send('setoption name ResignValue value 1\n'
                         'setoption name USI_Ponder value true\n' + position + '\ngo depth 2')
        self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')
        self.engine.send('debug on\nsetoption name USI_AnalyseMode value true\n' + position + '\ngo depth 2 infinite')
        lines = self.engine.until('info depth 2 ')
        self.assertIn('depth_limit 2 node_limit 0', '\n'.join(lines))
        self.engine.send('debug off')
        self.assertFalse(any(line.startswith('bestmove ') for line in self.engine.ready()))
        # 探索中の設定変更は次のgoから反映し、開始時の解析設定を維持する。
        self.engine.send('setoption name USI_AnalyseMode value false\nstop')
        bestmove = self.engine.until('bestmove ')[-1]
        self.assertNotEqual(bestmove, 'bestmove resign')
        self.assertNotIn(' ponder ', bestmove)
        self.engine.send(position + ' moves ' + bestmove.split()[1])
        self.assertEqual(self.engine.ready(), ['readyok'])
        self.engine.send(position + '\ngo depth 2')
        self.assertEqual(self.engine.until('bestmove ')[-1], 'bestmove resign')

    def test_debug_and_movestogo_time_allocation(self):
        self.assertFalse(any('debug ' in line for line in self.engine.ready()))
        self.engine.send('debug on\nposition startpos')
        for moves, expected in ((1, 60000), (10, 6000), (30, 2000), (0, 2000), (-2, 2000)):
            self.engine.send(f'go btime 60000 wtime 60000 movestogo {moves} nodes 1')
            lines = self.engine.until('bestmove ')
            self.assertTrue(any(f'debug search time_limit_ms {expected} ' in line for line in lines), lines)
        self.engine.send('go btime 60000 wtime 60000 movestogo 10 movetime 70 nodes 1')
        self.assertTrue(any('debug search time_limit_ms 70 ' in line for line in self.engine.until('bestmove ')))
        self.engine.send('go infinite searchmoves 7g7f 2g2f')
        self.engine.until('info depth ')
        self.assertTrue(any('debug received isready' in line for line in self.engine.ready()))
        self.engine.send('debug off\nisready')
        self.assertFalse(any('debug ' in line for line in self.engine.until('readyok')))
        self.engine.send('stop')
        self.engine.until('bestmove ', timeout=3)

    def test_refutations_option_and_legal_replies(self):
        self.assertIn('option name USI_ShowRefutations type check default false', self.usi_lines)
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}\nsetoption name Hash value 1\n'
                             'setoption name USI_ShowRefutations value true\nposition startpos\n'
                             'go depth 3 searchmoves 7g7f 2g2f')
            lines = self.engine.until('bestmove ')
            replies = [line.split()[2:] for line in lines if line.startswith('info refutation ')]
            self.assertEqual(len(replies), 2, lines)
            self.assertEqual({moves[0] for moves in replies}, {'7g7f', '2g2f'})
            self.assertTrue(all(len(moves) >= 2 for moves in replies), replies)
            for moves in replies:
                self.engine.send('position startpos moves ' + ' '.join(moves))
                self.assertEqual(self.engine.ready(), ['readyok'])
            self.engine.send('setoption name USI_ShowRefutations value false\nposition startpos\ngo depth 2')
            self.assertFalse(any('refutation ' in line for line in self.engine.until('bestmove ')))

    def test_score_bounds_precede_exact_research(self):
        self.engine.send('setoption name Threads value 1\nsetoption name Hash value 16\n'
                         'position sfen 4k4/9/9/9/4R4/9/9/9/4K4 w G2P 1\ngo depth 5')
        lines = self.engine.until('bestmove ')
        bounds = [(i, line) for i, line in enumerate(lines) if 'bound ' in line]
        self.assertTrue(bounds, lines)
        for index, line in bounds:
            depth = re.search(r'depth (\d+)', line)[1]
            value = int(re.search(r'score cp (-?\d+)', line)[1])
            exact = next((candidate for candidate in lines[index + 1:]
                          if candidate.startswith('info depth ' + depth + ' ') and
                          ' score cp ' in candidate and 'bound ' not in candidate), None)
            self.assertIsNotNone(exact, lines)
            final = int(re.search(r'score cp (-?\d+)', exact)[1])
            if 'upperbound' in line:
                self.assertGreaterEqual(value, final)
            else:
                self.assertLessEqual(value, final)

    def test_currline_all_workers_and_cpu_load(self):
        self.assertIn('option name USI_ShowCurrLine type check default false', self.usi_lines)
        allowed = {'7g7f', '2g2f', '5g5f', '3g3f'}
        for threads in (1, 4):
            self.engine.send(f'setoption name Threads value {threads}\nsetoption name Hash value 1\n'
                             'setoption name USI_ShowCurrLine value true\nposition startpos\n'
                             'go infinite searchmoves 7g7f 2g2f 5g5f 3g3f')
            lines = self.engine.until('info currline 1', timeout=5)
            if threads > 1:
                lines += self.engine.until(f'info currline {threads}', timeout=3)
            self.engine.send('stop')
            lines += self.engine.until('bestmove ', timeout=3)
            current = [line.split()[2:] for line in lines if line.startswith('info currline ')]
            self.assertEqual([int(fields[0]) for fields in current], list(range(1, threads + 1)), lines)
            metrics = [line for line in lines if ' currmovenumber ' in line]
            self.assertTrue(metrics, lines)
            for line in metrics:
                self.assertIn(int(re.search(r'currmovenumber (\d+)', line)[1]), range(1, 5))
                self.assertIn(int(re.search(r'cpuload (\d+)', line)[1]), range(1, 1001))
                self.assertIn(re.search(r'currmove (\S+)', line)[1], allowed)
            # 初手の直後や null move の探索中は、合法な接頭辞が1手だけの場合もある。
            self.assertTrue(any(len(fields) > 1 for fields in current), current)
            for fields in current:
                if len(fields) == 1:  # 担当する候補手が終了したワーカー
                    continue
                self.assertIn(fields[1], allowed)
                self.engine.send('position startpos moves ' + ' '.join(fields[1:]))
                self.assertEqual(self.engine.ready(), ['readyok'])
        self.engine.send('setoption name USI_ShowCurrLine value false\nposition startpos\ngo depth 3')
        self.assertFalse(any('currline ' in line for line in self.engine.until('bestmove ')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--threads', type=int, default=1)
    args, test_args = parser.parse_known_args()
    EngineTests.executable = args.engine.resolve()
    EngineTests.baseline = args.baseline.resolve() if args.baseline else None
    EngineTests.threads = args.threads
    unittest.main(argv=['test_engine.py'] + test_args, verbosity=2)
