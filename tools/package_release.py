#!/usr/bin/env python3
"""配置確認用ZIPまたは配布用ZIPを作り、同梱物のハッシュを記録する。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import zipfile

from build_book import digest_file
from match_book import read_json

ROOT = Path(__file__).resolve().parents[1]


def build_package(args):
    engine_hash = digest_file(args.engine)
    version = subprocess.run([str(args.engine.resolve()), '--version'], check=True,
                             capture_output=True, text=True).stdout.strip()
    match = re.fullmatch(r'Hayanagi (\d+\.\d+\.\d+)', version)
    if not match:
        raise ValueError(f'Unexpected engine version: {version!r}')
    contents = {('hayanagi.exe' if args.engine.suffix.lower() == '.exe' else 'hayanagi'): args.engine,
                'README.md': ROOT / 'README.md'}
    metadata, permission = None, None
    confirmed = False
    if args.book:
        metadata = read_json(args.book.with_suffix('.json'))
        if digest_file(args.book) != metadata['book_sha256']:
            raise ValueError('Book differs from its generation report')
        permission = read_json(args.redistribution)
        if permission['source_archive_sha256'] != metadata['source_archive_sha256']:
            raise ValueError('Redistribution review refers to a different source archive')
        confirmed = (permission['status'] == 'confirmed' and permission.get('release_book_ready') is True
                     and permission.get('permission_evidence_url') and permission.get('permission_text'))
        if args.purpose == 'release' and not confirmed:
            raise ValueError('Generated-book redistribution is unconfirmed; use --purpose preview for local layout verification')
        contents.update({'book/standard_book.db': args.book,
                         'book/standard_book.json': args.book.with_suffix('.json'),
                         'book/NOTICE.txt': ROOT / 'book/NOTICE.txt',
                         'book/redistribution.json': args.redistribution})
    hashes = {name: digest_file(path) for name, path in sorted(contents.items())}
    payloads = {name: path.read_bytes() for name, path in contents.items()}
    if any(hashlib.sha256(payloads[name]).hexdigest() != sha for name, sha in hashes.items()):
        raise RuntimeError('Package input changed while reading')
    executable = 'hayanagi.exe' if args.engine.suffix.lower() == '.exe' else 'hayanagi'
    if hashes[executable] != engine_hash:
        raise RuntimeError('Engine changed while checking its version')
    if args.book and (json.loads(payloads['book/standard_book.json']) != metadata or
                      json.loads(payloads['book/redistribution.json']) != permission or
                      hashes['book/standard_book.db'] != metadata['book_sha256']):
        raise RuntimeError('Book or permission metadata changed while validating')
    usage = [version, f'{platform.system()} / {platform.machine()}', '',
             'ZIP全体を展開し、将棋GUIのUSIエンジン登録で同梱の実行ファイルを選んでください。']
    if args.book:
        usage.extend(['実行ファイルとbookフォルダーの位置関係を保ってください。',
                      '標準設定（USI_OwnBook=true、BookDir=book、BookFile=standard_book.db）で定跡を読み込みます。',
                      '出典と配布条件はbook/NOTICE.txtおよびbook/redistribution.jsonを参照してください。'])
    else:
        usage.append('このZIPに定跡は含まれていません。定跡を使う場合は、GUIからBookDirとBookFileを指定してください。')
    if args.purpose == 'preview':
        usage.append('このZIPはローカルの配置確認用です。公開用の配布条件確認を完了したことを示すものではありません。')
    payloads['USAGE.txt'] = ('\n'.join(usage) + '\n').encode('utf-8')
    hashes['USAGE.txt'] = hashlib.sha256(payloads['USAGE.txt']).hexdigest()
    report = {'schema_version': 1, 'engine_version': version, 'purpose': args.purpose,
              'platform': platform.system(), 'machine': platform.machine(),
              'book_included': args.book is not None,
              'book_redistribution_confirmed': bool(confirmed),
              'files': hashes,
              'instructions': 'Extract the entire directory. Keep book/ next to the executable. Select that executable in the USI GUI.'}
    label = f'hayanagi-{match[1]}-{platform.system().lower()}-{platform.machine()}-{args.purpose}'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temp = args.output.with_name(args.output.name + '.tmp')
    payloads['manifest.json'] = (json.dumps(report, ensure_ascii=False, indent=2) + '\n').encode()
    with zipfile.ZipFile(temp, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payloads.items()):
            item = zipfile.ZipInfo(f'{label}/{name}', (1980, 1, 1, 0, 0, 0))
            item.create_system = 3
            item.external_attr = (0o100755 if name in ('hayanagi', 'hayanagi.exe') else 0o100644) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, data)
    temp.replace(args.output)
    print(json.dumps({'output': str(args.output), 'sha256': digest_file(args.output), **report}, ensure_ascii=False, indent=2))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, default=Path('build/hayanagi'))
    parser.add_argument('--book', type=Path)
    parser.add_argument('--redistribution', type=Path, default=ROOT / 'book/redistribution.json')
    parser.add_argument('--purpose', choices=('preview', 'release'), default='release')
    parser.add_argument('--output', type=Path, required=True)
    build_package(parser.parse_args())
