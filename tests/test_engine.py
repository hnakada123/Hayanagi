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


class EngineTests(unittest.TestCase):
    executable = None
    baseline = None

    def setUp(self):
        self.engine = Engine(self.executable)
        self.addCleanup(self.engine.close)
        self.engine.send('usi')
        self.usi_lines = self.engine.until('usiok')
        self.engine.send('setoption name USI_OwnBook value false')
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


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    parser.add_argument('--baseline', type=Path)
    args, test_args = parser.parse_known_args()
    EngineTests.executable = args.engine.resolve()
    EngineTests.baseline = args.baseline.resolve() if args.baseline else None
    unittest.main(argv=['test_engine.py'] + test_args, verbosity=2)
