#!/usr/bin/env python3
"""同梱条件、ZIPの再現性、別ディレクトリからの既定定跡読込を検証する。"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_release as package
from build_book import START_KEY, digest_file


class PackageTests(unittest.TestCase):
    def test_preview_layout_and_unconfirmed_release_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'input.db'
            book.write_text(f'#YANEURAOU-DB2016 1.00\nsfen {START_KEY} 1\n7g7f none 0 0 1\n')
            book.with_suffix('.json').write_text(json.dumps({'book_sha256': digest_file(book),
                                                            'source_archive_sha256': 'fixture'}))
            rights = root / 'rights.json'
            rights.write_text(json.dumps({'status': 'unconfirmed', 'source_archive_sha256': 'fixture'}))
            args = SimpleNamespace(engine=ENGINE, book=book, redistribution=rights,
                                   purpose='release', output=root / 'package.zip')
            with self.assertRaisesRegex(ValueError, 'redistribution is unconfirmed'):
                package.build_package(args)
            self.assertFalse(args.output.exists())
            args.purpose = 'preview'
            with contextlib.redirect_stdout(io.StringIO()): package.build_package(args)
            original = digest_file(args.output)
            with contextlib.redirect_stdout(io.StringIO()): package.build_package(args)
            self.assertEqual(original, digest_file(args.output))
            extracted = root / '展開 先'
            with zipfile.ZipFile(args.output) as archive:
                archive.extractall(extracted)
                for info in archive.infolist():
                    path = extracted / info.filename
                    path.chmod((info.external_attr >> 16) & 0o777)
                    if path.name == 'manifest.json': manifest = json.loads(path.read_text())
                engine = next(extracted.rglob('hayanagi.exe' if os.name == 'nt' else 'hayanagi'))
                for name, sha in manifest['files'].items():
                    self.assertEqual(digest_file(engine.parent / name), sha)
            outside = root / 'unrelated cwd'
            outside.mkdir()
            result = subprocess.run([str(engine)], input='usi\nisready\nposition startpos\ngo nodes 50\nquit\n',
                                    text=True, capture_output=True, check=True, cwd=outside)
            self.assertIn('book loaded ', result.stdout)
            self.assertIn('bestmove 7g7f', result.stdout)
            self.assertFalse(manifest['book_redistribution_confirmed'])
            self.assertTrue(manifest['book_included'])

    def test_book_hash_and_permission_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'book.db'
            book.write_text('fixture')
            book.with_suffix('.json').write_text(json.dumps({'book_sha256': 'incorrect', 'source_archive_sha256': 'a'}))
            rights = root / 'rights.json'
            rights.write_text(json.dumps({'status': 'confirmed', 'source_archive_sha256': 'b'}))
            args = SimpleNamespace(engine=ENGINE, book=book, redistribution=rights,
                                   purpose='preview', output=root / 'book.zip')
            with self.assertRaisesRegex(ValueError, 'Book differs'): package.build_package(args)
            book.with_suffix('.json').write_text(json.dumps({'book_sha256': digest_file(book), 'source_archive_sha256': 'a'}))
            with self.assertRaisesRegex(ValueError, 'different source archive'): package.build_package(args)

    def test_engine_only_release(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'engine.zip'
            args = SimpleNamespace(engine=ENGINE, book=None, redistribution=None, purpose='release', output=path)
            with contextlib.redirect_stdout(io.StringIO()): report = package.build_package(args)
            self.assertFalse(report['book_included'])
            with zipfile.ZipFile(path) as archive:
                self.assertFalse(any('/book/' in name for name in archive.namelist()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', type=Path, default=ROOT / 'build/hayanagi')
    args, remaining = parser.parse_known_args()
    ENGINE = args.engine.resolve()
    unittest.main(argv=[sys.argv[0]] + remaining)
