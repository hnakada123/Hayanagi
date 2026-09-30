#!/usr/bin/env python3
"""生成定跡の全候補・予想応手と、選別記録との一致を再検証する。"""
import argparse
import json
from pathlib import Path

from build_book import digest_file, key, positive
from match_book import Referee, parallel_jobs, position_command, read_json, write_json


def validate_chunk(referee, records, settings):
    count = 0
    with Referee([str(referee.resolve())]) as ref:
        for record in records:
            state = ref.state(position_command(record['history']))
            if state['sfen'] != record['sfen'] or state['outcome'] != 'none':
                raise ValueError('Provenance history differs from SFEN/nonterminal state')
            for entry in record['entries']:
                state = ref.state(position_command(record['history']))
                if entry['move'] not in state['legal']:
                    raise ValueError('Illegal book move')
                child = ref.state('move ' + entry['move'])
                if entry['ponder'] != 'none' and (child['outcome'] != 'none' or entry['ponder'] not in child['legal']):
                    raise ValueError('Illegal book ponder')
                if entry['count'] < settings['min_count'] or entry['pair_count'] < settings['min_pairs']:
                    raise ValueError('Insufficient corpus support')
                analysis = record['analysis']
                if analysis:
                    measured = analysis['candidates'][entry['move']]
                    base = analysis['baseline']
                    if (entry['score'] != measured['score'] or entry['depth'] != measured['depth'] or
                            min(measured['depth'], base['depth']) < settings['min_depth'] or
                            measured['score'] < base['score'] - settings['max_loss_cp'] or
                            (measured['score_type'] == 'mate' and measured['score_value'] < 0)):
                        raise ValueError('Entry contradicts analysis/filter')
                    if settings.get('analysis_version', 1) >= 2:
                        if measured['depth'] != base['depth'] or base['depth'] != analysis['comparison_depth']:
                            raise ValueError('Comparison depth mismatch')
                count += 1
    return count


def run(args):
    report = read_json(args.book.with_suffix('.json'))
    provenance = args.book.with_suffix('.positions.jsonl')
    records = [json.loads(line) for line in provenance.read_text().splitlines()]
    if report['book_sha256'] != digest_file(args.book):
        raise ValueError('Book hash differs from report')
    expected, actual, current = {}, {}, None
    reviewed = {key(p['sfen']) for p in report.get('deep_review', {}).get('input', {}).get('positions', [])}
    for record in records:
        k = key(record['sfen'])
        if 'nodes_per_search' in record:
            budget = (report['deep_review']['nodes_per_search'] if k in reviewed
                      else report['export_settings']['nodes_per_search'])
            if record['nodes_per_search'] != budget:
                raise ValueError('Incorrect search budget in provenance')
        if k in expected:
            raise ValueError('Duplicate position in provenance')
        expected[k] = [' '.join(str(e[name]) for name in ('move', 'ponder', 'score', 'depth', 'count'))
                       for e in record['entries']]
    limits = []
    for line in args.book.read_text().splitlines():
        if line.startswith('# HAYANAGI_MAX_PLY '):
            limits.append(int(line.split()[-1]))
        elif line.startswith('sfen '):
            current = key(line[5:])
            if current in actual:
                raise ValueError('Duplicate position in book')
            actual[current] = []
        elif line and not line.startswith('#'):
            if current is None:
                raise ValueError('Move before SFEN')
            actual[current].append(line)
    if actual != expected or len(records) != report['positions'] or limits != [report['import_settings']['max_ply']]:
        raise ValueError('Book and provenance differ')
    chunks = [records[i::args.workers] for i in range(args.workers)]
    entries = sum(count for _, count in parallel_jobs(
        chunks, lambda chunk: validate_chunk(args.referee, chunk, report['export_settings']), args.workers))
    if entries != report['entries']:
        raise ValueError('Wrong entry count')
    result = {'success': True, 'book_sha256': digest_file(args.book),
              'provenance_sha256': digest_file(provenance), 'referee_sha256': digest_file(args.referee),
              'checked_positions': len(records), 'checked_candidates_and_ponder': entries,
              'analysis_and_filter_consistent': True,
              'limitation': 'Legal replay uses Hayanagi Position; this is not an independent strength evaluation'}
    write_json(args.book.with_suffix('.validation.json'), result)
    print(result, flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--book', type=Path, required=True)
    parser.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    parser.add_argument('--workers', type=positive, default=4)
    run(parser.parse_args())
