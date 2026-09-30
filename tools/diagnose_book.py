#!/usr/bin/env python3
"""Diagnose frozen book matches without changing the engine or opening book.

Screen full loss trajectories, then compare the played move with deeper-search
alternatives at the same completed MultiPV depth. All scores are from Hayanagi;
this is a search-consistency diagnosis, not an external proof of move quality.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import time

from build_book import digest_file, parse_search, positive
from match_book import (Player, Referee, frozen_manifest, parallel_jobs,
                        position_command, read_json, write_json)

VERSION = 1


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def cp(score):
    return score['score'] if score and score['score_type'] == 'cp' else None


def complete_scores(lines, candidates=None):
    depths = parse_search(lines)
    complete = {d: scores for d, scores in depths.items()
                if candidates is None or set(candidates) <= set(scores)}
    if not complete:
        raise RuntimeError('No completed exact search scores')
    depth = max(complete)
    return {'depth': depth, 'scores': complete[depth],
            'last_depths': [{'depth': d, 'scores': complete[d]}
                            for d in sorted(complete)[-3:]]}


def gap_result(record, played):
    scores = record['scores']
    if played not in scores:
        raise ValueError('Played move missing from matched-depth comparison')
    best = max(scores, key=lambda m: scores[m]['score'])
    a, b = scores[played], scores[best]
    return {'played': played, 'alternative': best, 'depth': record['depth'],
            'played_score': a, 'alternative_score': b,
            'gap_cp': b['score'] - a['score'] if cp(a) is not None and cp(b) is not None else None,
            'avoids_losing_mate': a['score_type'] == 'mate' and a['score_value'] < 0
                                 and b['score'] > -29000,
            'misses_winning_mate': b['score_type'] == 'mate' and b['score_value'] > 0
                                   and a['score'] < 29000}


def significant(gap, threshold):
    return ((gap['gap_cp'] is not None and gap['gap_cp'] >= threshold)
            or gap['avoids_losing_mate'] or gap['misses_winning_mate'])


def make_plan(games, windows):
    """Freeze selection before any new deep analysis is available."""
    losses = {g['id'] for g in games if g['nodes_per_move'] == 50000
              and g['book_score'] == 0 and g['book_hit_plies']}
    early = set()
    for w in windows:
        p = w['points']['book']
        if all(p.get(o) and p[o].get('cp') is not None for o in ('0', '8')):
            if p['8']['cp'] - p['0']['cp'] <= -200:
                early.add(w['game_id'])
    histories, occurrences, trajectories, checkpoints = {}, [], {}, []

    def add(history):
        hid = fingerprint(history)
        histories[hid] = history
        return hid

    for g in games:
        trace = []
        exit_ply = g['permanent_exit_ply']
        for e in g['events']:
            if e['player'] != 'book':
                continue
            full_trace = g['id'] in losses and e['ply'] >= g['book_hit_plies'][0]
            early_trace = g['id'] in early and exit_ply <= e['ply'] <= exit_ply + 8
            if e['book_hit'] or full_trace or early_trace or e['ply'] == exit_ply:
                hid = add(g['moves'][:e['ply']-1])
                item = {'game_id': g['id'], 'history_id': hid, 'ply': e['ply'],
                        'move': e['move'], 'book_hit': e['book_hit'],
                        'exit_ply': exit_ply, 'match_score': e['score']}
                occurrences.append(item)
                if full_trace or early_trace:
                    trace.append(item)
        if trace:
            trajectories[g['id']] = trace
        if exit_ply is not None:
            for offset in (0, 8):
                length = exit_ply - 1 + offset
                if length < len(g['moves']):
                    checkpoints.append({'game_id': g['id'], 'offset': offset,
                                        'history_id': add(g['moves'][:length])})
    return {'loss_games_50000': sorted(losses), 'early_drop_games': sorted(early),
            'histories': histories, 'occurrences': occurrences,
            'trajectories': trajectories, 'exit_checkpoints': checkpoints}


class AnalysisCache:
    def __init__(self, args, engine_hash):
        self.args, self.engine_hash = args, engine_hash

    def request(self, history, nodes, candidates=None):
        return {'version': VERSION, 'engine_sha256': self.engine_hash, 'history': history,
                'nodes': nodes, 'candidates': sorted(set(candidates)) if candidates else None}

    def read(self, request):
        return read_json(self.args.output / 'analysis' / (fingerprint(request) + '.json'))

    def analyze(self, request):
        with Player(self.args.engine, analysis=True) as engine:
            candidates = request['candidates']
            if candidates:
                engine.send(f'setoption name MultiPV value {len(candidates)}')
            engine.send(position_command(request['history']))
            command = f'go nodes {request["nodes"]}'
            if candidates:
                command += ' searchmoves ' + ' '.join(candidates)
            started = time.monotonic()
            engine.send(command)
            lines = engine.until('bestmove ', timeout=300)
            if any(line.startswith('info string book hit ') for line in lines):
                raise RuntimeError('Diagnostic analysis unexpectedly used the book')
            try:
                result = complete_scores(lines, candidates)
            except RuntimeError:
                # The budget can expire in preliminary mate search or depth-1
                # quiescence before any root score completes. Never invent a CP score.
                result = {'depth': 0, 'scores': {}, 'last_depths': [],
                          'incomplete': True, 'raw_output': lines}
            result.update(request=request, bestmove=lines[-1].split()[1],
                          elapsed_ms=round((time.monotonic()-started)*1000, 3))
        return result

    def run(self, label, requests):
        unique = {fingerprint(r): r for r in requests}
        pending = [r for rid, r in unique.items()
                   if not (self.args.output / 'analysis' / (rid + '.json')).exists()]
        print(f'{label}: {len(unique)-len(pending)} cached, {len(pending)} pending', flush=True)
        for i, (request, result) in enumerate(parallel_jobs(pending, self.analyze, self.args.workers), 1):
            write_json(self.args.output / 'analysis' / (fingerprint(request)+'.json'), result)
            if i % 100 == 0 or i == len(pending):
                print(f'{label} {i}/{len(pending)}', flush=True)


def select_comparisons(plan, scan):
    selected = defaultdict(set)
    for o in plan['occurrences']:
        k = (o['history_id'], o['move'])
        if o['book_hit']:
            selected[k].add('book_move')
        if o['ply'] == o['exit_ply']:
            selected[k].add('first_normal_move')
        if o['game_id'] in plan['early_drop_games'] and o['exit_ply'] <= o['ply'] < o['exit_ply'] + 8:
            selected[k].add('early_drop_window')
    for gid, trace in plan['trajectories'].items():
        drops = []
        for before, after in zip(trace, trace[1:]):
            if after['ply'] != before['ply'] + 2 or before['book_hit']:
                continue
            a = scan[before['history_id']]['scores']
            b = scan[after['history_id']]['scores']
            if not a or not b:
                continue
            av, bv = max(a.values(), key=lambda x: x['score']), max(b.values(), key=lambda x: x['score'])
            # Do not nominate large losses that merely occur in already lost positions.
            if cp(av) is None or av['score'] < -200:
                continue
            decline = av['score'] - bv['score']
            if decline >= 100:
                drops.append((decline, before))
        nominees = sorted(drops, key=lambda d: (-d[0], d[1]['ply']))[:3]
        if drops:
            nominees.append(min(drops, key=lambda d: d[1]['ply']))
        for _, o in nominees:
            selected[(o['history_id'], o['move'])].add('loss_trajectory_drop')
    return [{'history_id': h, 'played': m, 'reasons': sorted(reasons)}
            for (h, m), reasons in sorted(selected.items())]


def compare(cache, plan, selected, nodes, seed_choices):
    discover = {s['history_id']: cache.request(plan['histories'][s['history_id']], nodes)
                for s in selected}
    cache.run(f'discover_{nodes}', discover.values())
    requests = {}
    for s in selected:
        hid = s['history_id']
        choices = set(seed_choices.get((hid, s['played']), []))
        choices.update((s['played'], cache.read(discover[hid])['bestmove']))
        requests[(hid, s['played'])] = cache.request(plan['histories'][hid], nodes, choices)
    cache.run(f'compare_{nodes}', requests.values())
    results = {}
    for s in selected:
        k = (s['history_id'], s['played'])
        record = cache.read(requests[k])
        if s['played'] not in record['scores']:
            raise RuntimeError(f'No complete candidate comparison: {requests[k]}')
        result = gap_result(record, s['played'])
        discovery = cache.read(discover[s['history_id']])
        result.update(history_id=s['history_id'], reasons=s['reasons'],
                      nodes_per_search=nodes, request_id=fingerprint(requests[k]),
                      unrestricted_analysis_id=fingerprint(discover[s['history_id']]),
                      unrestricted_depth=discovery['depth'],
                      unrestricted_score=discovery['scores'].get(discovery['bestmove']),
                      candidates=requests[k]['candidates'], last_depths=record['last_depths'])
        results[k] = result
    return results


def pair_decomposition(games, nodes):
    pairs = defaultdict(list)
    for g in games:
        if g['group'] == 'holdout' and g['nodes_per_move'] == nodes:
            pairs[g['opening_id']].append(g)
    result = {}
    for hit in (False, True):
        group = [p for p in pairs.values() if any(g['book_hit_plies'] for g in p) == hit]
        scores = Counter(g['book_score'] for p in group for g in p)
        result['book_used_in_pair' if hit else 'no_book_use_in_pair'] = {
            'pairs': len(group), 'wins': scores[1.0], 'losses': scores[0.0], 'draws': scores[0.5],
            'identical_move_sequences': sum(p[0]['moves'] == p[1]['moves'] for p in group)}
    return result


def first_divergences(games):
    pairs = defaultdict(dict)
    for g in games:
        pairs[(g['nodes_per_move'], g['opening_id'])][g['book_side']] = g
    result = []
    for (nodes, oid), pair in sorted(pairs.items()):
        a, b = pair['b'], pair['w']
        ply = next((i for i, (x, y) in enumerate(zip(a['moves'], b['moves']), 1) if x != y), None)
        item = {'nodes_per_move': nodes, 'opening_id': oid,
                'pair_book_points': a['book_score'] + b['book_score'],
                'identical_moves': a['moves'] == b['moves'], 'first_different_ply': ply}
        if ply is not None:
            side = 'b' if ply % 2 else 'w'
            book = pair[side]
            control = pair['w' if side == 'b' else 'b']
            event = next(e for e in book['events'] if e['ply'] == ply)
            item.update(history=book['moves'][:ply-1], book_game=book['id'],
                        book_player_move=book['moves'][ply-1],
                        control_move=control['moves'][ply-1], used_book=event['book_hit'])
        result.append(item)
    return result


def score_coverage(games):
    result = {}
    for nodes in (10000, 50000):
        counts = Counter()
        examples = []
        for g in games:
            if g['nodes_per_move'] != nodes:
                continue
            for e in g['events']:
                if e['book_hit']:
                    continue
                counts[e['player'] + '_searched_moves'] += 1
                if e['score'] is None:
                    counts[e['player'] + '_without_completed_score'] += 1
                    examples.append({'game_id': g['id'], 'ply': e['ply'], 'move': e['move']})
        result[str(nodes)] = {'counts': dict(counts), 'missing_score_moves': examples,
                             'includes_startpos_games': True}
    return result


def terminal_followup(args, plan, scan, selected, engine_hash, states):
    # Adjacent-own-turn screening cannot see a loss after the final own move.
    # Keep this addition separate from the original frozen selection.
    already = {(s['history_id'], s['played']) for s in selected}
    nominees = []
    for gid in plan['loss_games_50000']:
        o = plan['trajectories'][gid][-1]
        record = scan[o['history_id']]
        value = record['scores'].get(record['bestmove'])
        if value and value['score'] >= -200 and (o['history_id'], o['move']) not in already:
            nominees.append(o)
    extra_args = argparse.Namespace(**{**vars(args), 'output': args.output / 'terminal_followup'})
    frozen_manifest(extra_args.output / 'manifest.json', {
        'engine_sha256': engine_hash, 'compare_nodes': args.compare_nodes,
        'confirm_nodes': args.confirm_nodes,
        'selection': 'Otherwise unselected final own moves of traced losses, scan score >= -200 or winning mate; terminal loss has no next own turn',
        'nominees': [{'game_id': o['game_id'], 'ply': o['ply'], 'move': o['move']} for o in nominees]})
    cache = AnalysisCache(extra_args, engine_hash)
    requests = [{'history_id': o['history_id'], 'played': o['move'],
                 'reasons': ['terminal_loss_without_next_own_turn']} for o in nominees]
    initial = compare(cache, plan, requests, args.compare_nodes, {})
    confirmed = compare(cache, plan, requests, args.confirm_nodes,
                        {k: r['candidates'] for k, r in initial.items()})
    report = {'supplement_to_frozen_primary_selection': True, 'cases': []}
    for o in nominees:
        k = (o['history_id'], o['move'])
        report['cases'].append({'game_id': o['game_id'], 'ply': o['ply'],
                                'history': plan['histories'][o['history_id']],
                                'sfen': states[o['history_id']]['sfen'],
                                'match_score': o['match_score'],
                                'initial': initial[k], 'confirmed': confirmed[k]})
    write_json(args.output / 'terminal_followup.json', report)
    return report


def run(args):
    manifest = read_json(args.matches / 'manifest.json')
    engine_hash = digest_file(args.engine)
    if engine_hash != manifest['engine_sha256'] or digest_file(args.referee) != manifest['referee_sha256']:
        raise ValueError('Use the frozen match engine and referee binaries')
    games = [read_json(p) for p in sorted((args.matches / 'games').glob('*.json'))]
    if len(games) != manifest['expected_games']:
        raise ValueError('Match set is incomplete')
    windows = read_json(args.matches / 'exit_windows.json')
    config = {'version': VERSION, 'engine_sha256': engine_hash,
              'referee_sha256': manifest['referee_sha256'], 'book_sha256': manifest['book_sha256'],
              'games_sha256': fingerprint(games), 'windows_sha256': fingerprint(windows),
              'scan_nodes': args.scan_nodes, 'compare_nodes': args.compare_nodes,
              'confirm_nodes': args.confirm_nodes, 'threads': 1, 'hash_mb': 16,
              'book_gap_threshold_cp': 150, 'normal_gap_threshold_cp': 200,
              'selection': 'all book moves and first exits; full own-turn trajectories of 50000-node book-hit losses; all prior >=200cp early drops; top 3 and first >=100cp trajectory drop while before >=-200cp',
              'confirmation': 'all book moves; all initially >=200cp or mate-choice ordinary moves',
              'score_perspective': 'side to move; paired MultiPV at a shared completed depth',
              'fresh_process_per_search': True}
    frozen_manifest(args.output / 'manifest.json', config)
    plan = make_plan(games, windows)
    frozen_manifest(args.output / 'plan.json', plan)
    print(f'plan: {len(plan["histories"])} positions, {len(plan["loss_games_50000"])} book-hit losses, '
          f'{len(plan["early_drop_games"])} early-drop games', flush=True)
    # Check every root and played move with the existing rules before analysis.
    with Referee([str(args.referee.resolve())]) as referee:
        states = {hid: referee.state(position_command(h)) for hid, h in plan['histories'].items()}
    for o in plan['occurrences']:
        if o['move'] not in states[o['history_id']]['legal']:
            raise ValueError(f'Illegal diagnostic move: {o}')
    cache = AnalysisCache(args, engine_hash)
    requests = {hid: cache.request(h, args.scan_nodes) for hid, h in plan['histories'].items()}
    cache.run('scan', requests.values())
    scan = {hid: cache.read(r) for hid, r in requests.items()}
    selected = select_comparisons(plan, scan)
    frozen_manifest(args.output / 'selected.json', selected)
    seeds = {(s['history_id'], s['played']): [scan[s['history_id']]['bestmove']] for s in selected}
    initial = compare(cache, plan, selected, args.compare_nodes, seeds)
    confirm = [s for s in selected if 'book_move' in s['reasons']
               or significant(initial[(s['history_id'], s['played'])], 200)]
    seeds = {k: r['candidates'] for k, r in initial.items()}
    confirmed = compare(cache, plan, confirm, args.confirm_nodes, seeds)
    by_game = {g['id']: g for g in games}
    details = []
    for s in selected:
        k = (s['history_id'], s['played'])
        item = dict(s, history=plan['histories'][s['history_id']],
                    sfen=states[s['history_id']]['sfen'],
                    initial=initial[k], confirmed=confirmed.get(k))
        item['occurrences'] = [o for o in plan['occurrences']
                               if (o['history_id'], o['move']) == k]
        details.append(item)
    write_json(args.output / 'comparisons.json', details)
    trajectories = {}
    for gid, trace in plan['trajectories'].items():
        rows = []
        for o in trace:
            record = scan[o['history_id']]
            best = record['scores'].get(record['bestmove'])
            rows.append(dict(o, scan_bestmove=record['bestmove'], scan_score=best))
        trajectories[gid] = rows
    write_json(args.output / 'trajectories.json', trajectories)
    exits = []
    for checkpoint in plan['exit_checkpoints']:
        r = scan[checkpoint['history_id']]
        exits.append(dict(checkpoint, score=r['scores'].get(r['bestmove'])))
    write_json(args.output / 'exit_checkpoints.json', exits)
    write_json(args.output / 'first_divergences.json', first_divergences(games))
    write_json(args.output / 'search_score_coverage.json', score_coverage(games))

    book_details = [d for d in details if 'book_move' in d['reasons']]
    normal_details = [d for d in details if any(not o['book_hit'] for o in d['occurrences'])]
    suspicious_books = [d for d in book_details if significant(d['confirmed'], 150)]
    confirmed_normal = [d for d in normal_details if d['confirmed'] and significant(d['confirmed'], 200)]
    attributed = []
    for gid in plan['loss_games_50000']:
        b = [d for d in suspicious_books if any(o['game_id'] == gid and o['book_hit'] for o in d['occurrences'])]
        n = [d for d in confirmed_normal if any(o['game_id'] == gid and not o['book_hit'] for o in d['occurrences'])]
        entry = next((e['score'] for e in exits if e['game_id'] == gid and e['offset'] == 0), None)
        entry_deep = next((d['initial']['unrestricted_score'] for d in details
                           if any(o['game_id'] == gid and o['ply'] == o['exit_ply']
                                  for o in d['occurrences'])), None)
        avoidable = [d for d in n if d['confirmed']['alternative_score']['score'] >= -200]
        early_normal = any(o['game_id'] == gid and not o['book_hit'] and
                           o['ply'] < by_game[gid]['permanent_exit_ply'] + 8
                           for d in n for o in d['occurrences'])
        attributed.append({'game_id': gid, 'group': by_game[gid]['group'],
                           'book_move_gap_ge_150': bool(b), 'normal_move_gap_ge_200': bool(n),
                           'normal_move_gap_in_first_8_plies': early_normal,
                           'normal_move_gap_with_nonlosing_alternative': bool(avoidable),
                           'exit_scan_cp': cp(entry),
                           'exit_unrestricted_compare_cp': cp(entry_deep),
                           'classification': 'both' if b and n else 'book_move' if b else
                                             'normal_move' if n else 'unresolved'})
    write_json(args.output / 'loss_classification.json', attributed)
    exit_values = [a['exit_unrestricted_compare_cp'] for a in attributed
                   if a['exit_unrestricted_compare_cp'] is not None]
    result = {'complete': True, 'manifest': config,
              'scan_positions': len(scan), 'compared_position_moves': len(details),
              'scan_positions_without_completed_score': sum(not r['scores'] for r in scan.values()),
              'confirmed_position_moves': len(confirmed),
              'book_moves_compared': len(book_details),
              'book_moves_gap_ge_150_at_confirmation': len(suspicious_books),
              'ordinary_moves_gap_ge_200_at_confirmation': len(confirmed_normal),
              'book_gap_cp_max': max((d['confirmed']['gap_cp'] for d in book_details
                                     if d['confirmed']['gap_cp'] is not None), default=None),
              'loss_classification_50000_holdout': dict(Counter(a['classification'] for a in attributed if a['group']=='holdout')),
              'loss_classification_startpos': [a for a in attributed if a['group']=='startpos'],
              'loss_exits_50000': {'games': len(attributed), 'includes_startpos': True,
                                   'nodes': args.compare_nodes, 'cp_count': len(exit_values),
                                   'min_cp': min(exit_values) if exit_values else None,
                                   'mean_cp': sum(exit_values)/len(exit_values) if exit_values else None,
                                   'at_or_below_minus200': sum(v <= -200 for v in exit_values)},
              'pair_decomposition': {str(n): pair_decomposition(games, n) for n in (10000, 50000)},
              'limitations': [
                  'Retrospective diagnosis of the tested games, not a new independent strength test',
                  'All analyses use the same Hayanagi evaluator, with book disabled',
                  'Shared completed depth removes the root depth mismatch but not selective-search or evaluation errors',
                  'Normal-move candidates are selected by a shallow scan, not an exhaustive comparison of every move',
                  'A search gap identifies a candidate error, not proof that changing that move reverses the game result',
                  'Mate values are kept separate from centipawn differences',
                  'Post-treatment subsets and trajectory screening must not be interpreted as independent causal effects']}
    if digest_file(args.engine) != engine_hash:
        raise RuntimeError('Engine changed during diagnosis')
    supplement = terminal_followup(args, plan, scan, selected, engine_hash, states)
    result['terminal_followup_cases'] = len(supplement['cases'])
    result['terminal_followup_confirmed_choices'] = sum(significant(c['confirmed'], 200)
                                                       for c in supplement['cases'])
    write_json(args.output / 'summary.json', result)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--matches', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--engine', type=Path, default=Path('build/hayanagi'))
    p.add_argument('--referee', type=Path, default=Path('build/hayanagi_match_referee'))
    p.add_argument('--scan-nodes', type=positive, default=500000)
    p.add_argument('--compare-nodes', type=positive, default=2000000)
    p.add_argument('--confirm-nodes', type=positive, default=10000000)
    p.add_argument('--workers', type=positive, default=8)
    run(p.parse_args())


if __name__ == '__main__':
    main()
