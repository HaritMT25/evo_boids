#!/usr/bin/env python3
"""ocean_wiring_summary.py DIR [DIR...] [--course] — summarise tools/ocean_eye_exp's per-fish wiring
dumps (<ARM>_s<SEED>.wiring.csv, from --dump-wiring) next to the same runs' behavioural test
(<ARM>_s<SEED>_behav.csv, --behav) and interval CSV (<ARM>_s<SEED>.csv). Plain python 3.

Per run (the final snapshot), evolved fish:
  n                live fish
  none L R bi+ bi- bi=   fraction per horizontal class (EYE_LEFT/RIGHT_* -> STEER_X; see the
                   dump's description in ocean_eye_exp.cpp): none, left_only, right_only,
                   bi_toward, bi_away, bi_same
  rnd1s rndbi      one-sided / bilateral fractions for random genomes of the same lengths
  dom(share)       the most common non-none class and its share of all fish
  feat             the dominant feature (hdom: largest summed |gain| to STEER_X) among the fish of
                   that class, with its share
  lin              distinct lineages among them; top = the largest lineage's share
  food_h fish_h    behavioural indices, evolved - random (a sweep: food_h > 0.5)
  both one bias    per-side behaviour, evolved: fraction of fish turning toward food on both sides /
                   on exactly one side / on one side with the other served by a turning bias in
                   empty space (frac_food_both / frac_food_one / frac_one_bias; '-' with an older
                   driver)
  search pop       search efficiency (clearance_norm) and pop, means over the last 20 % of the run
  t25 t50          first snapshot at which the dominant class holds >= 25 % / 50 % of the fish
  maxbi            the largest bilateral fraction (bi+ + bi- + bi=) at any snapshot
With --course: per snapshot, the fractions with a left wire (left_only or bilateral) / a right wire
(right_only or bilateral) / both — when the second side appears relative to the first.
Per arm: the sweep count and, for sweeping vs other seeds, the median one-sided and bilateral
fractions and the dominant features."""
import csv, glob, os, re, statistics as st, sys
from collections import Counter, defaultdict

HC = ['none', 'left_only', 'right_only', 'bi_toward', 'bi_away', 'bi_same']

def med(v):
    v = [x for x in v if x == x]
    return st.median(v) if v else float('nan')

def read_dump(path):
    S, F = {}, defaultdict(list)
    fcols = scols = None
    with open(path) as fh:
        for line in fh:
            line = line.rstrip('\n')
            if line.startswith('#F,'): fcols = line[1:].split(','); continue
            if line.startswith('#S,'): scols = line[1:].split(','); continue
            if not line or line[0] in '#P': continue
            t = line.split(',')
            if t[0] == 'S' and scols: d = dict(zip(scols, t)); S[(int(d['step']), d['who'])] = d
            elif t[0] == 'F' and fcols: d = dict(zip(fcols, t)); F[int(d['step'])].append(d)
    return S, F

def behav_of(path):
    if not os.path.exists(path): return {}
    rs = list(csv.DictReader(open(path)))
    ev = [r for r in rs if r['genomes'] == 'evolved']; rn = [r for r in rs if r['genomes'] == 'random']
    if not ev or not rn: return {}
    out = {}
    for k in ('food_h', 'fish_h'): out[k] = float(ev[0][k]) - float(rn[0][k])
    for k in ('frac_food_both', 'frac_food_one', 'frac_one_bias'):
        if k in ev[0]: out[k] = float(ev[0][k])
    return out

def tail_means(path):
    if not os.path.exists(path): return float('nan'), float('nan')
    rs = list(csv.DictReader(open(path)))
    if not rs: return float('nan'), float('nan')
    n = max(int(r['step']) for r in rs)
    sel = [r for r in rs if int(r['step']) > 0.8 * n]
    return (sum(float(r['clearance_norm']) for r in sel) / len(sel), sum(float(r['pop_mean']) for r in sel) / len(sel))

def run_summary(dump):
    base = dump[:-len('.wiring.csv')]
    m = re.match(r'(.+)_s(\d+)$', os.path.basename(base))
    if not m:
        print('(skipped %s: not named <ARM>_s<SEED>.wiring.csv)' % dump, file=sys.stderr)
        return None
    S, F = read_dump(dump)
    steps = sorted({s for s, _ in S})
    if not steps:
        print('(skipped %s: no snapshot)' % dump, file=sys.stderr)
        return None
    last = steps[-1]
    def fr(s, who, k):
        d = S.get((s, who))
        return int(d[k]) / int(d['n']) if d and int(d['n']) > 0 else float('nan')
    r = {'arm': m.group(1), 'seed': int(m.group(2)), 'n': int(S[(last, 'evolved')]['n'])}
    for c in HC: r[c] = fr(last, 'evolved', 'h_' + c)
    r['one'] = r['left_only'] + r['right_only']; r['bi'] = r['bi_toward'] + r['bi_away'] + r['bi_same']
    r['rnd1s'] = fr(last, 'random', 'h_left_only') + fr(last, 'random', 'h_right_only')
    r['rndbi'] = sum(fr(last, 'random', 'h_' + c) for c in ('bi_toward', 'bi_away', 'bi_same'))
    nonnone = {c: r[c] for c in HC[1:]}
    r['dom'] = max(nonnone, key=nonnone.get)
    fish = F.get(last, [])
    dfish = [f for f in fish if f['hclass'] == r['dom']]
    fc = Counter(f['hdom'] for f in dfish)
    r['feat'] = fc.most_common(1)[0] if fc else ('-', 0)
    r['feat_share'] = r['feat'][1] / len(dfish) if dfish else float('nan')
    lin = Counter(f['lineage'] for f in dfish)
    r['lin'] = len(lin); r['top'] = lin.most_common(1)[0][1] / len(dfish) if dfish else float('nan')
    r['course'] = []
    for s in steps:
        L = fr(s, 'evolved', 'h_left_only'); R = fr(s, 'evolved', 'h_right_only')
        B = sum(fr(s, 'evolved', 'h_' + c) for c in ('bi_toward', 'bi_away', 'bi_same'))
        r['course'].append((s, L + B, R + B, B, fr(s, 'evolved', 'h_' + r['dom'])))
    r['t25'] = next((s for s, *_, d in r['course'] if d >= 0.25), None)
    r['t50'] = next((s for s, *_, d in r['course'] if d >= 0.50), None)
    r['maxbi'] = max(c[3] for c in r['course'])
    r.update(behav_of(base + '_behav.csv'))
    r['search'], r['pop'] = tail_means(base + '.csv')
    r['sweep'] = r.get('food_h', float('nan')) > 0.5
    return r

def fmt(v, nd=2):
    if v is None: return '-'
    if isinstance(v, float): return 'nan' if v != v else ('%.' + str(nd) + 'f') % v
    return str(v)

def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith('--')] or ['.']
    course = '--course' in sys.argv
    runs = [run_summary(p) for d in dirs for p in sorted(glob.glob(os.path.join(d, '*.wiring.csv')))]
    runs = [r for r in runs if r]
    if not runs: print('no <ARM>_s<SEED>.wiring.csv'); return
    by = defaultdict(list)
    for r in runs: by[r['arm']].append(r)
    hdr = ('%-6s %4s %4s %5s %5s %5s %5s %5s %5s %5s %5s %-17s %-11s %3s %5s %6s %6s %5s %5s %5s %6s %5s %7s %7s %5s'
           % ('arm', 'seed', 'n', 'none', 'L', 'R', 'bi+', 'bi-', 'bi=', 'rnd1s', 'rndbi', 'dom(share)', 'feat',
              'lin', 'top', 'food_h', 'fish_h', 'both', 'one', 'bias', 'search', 'pop', 't25', 't50', 'maxbi'))
    for arm in sorted(by):
        print('== %s' % arm); print(hdr)
        for r in sorted(by[arm], key=lambda r: r['seed']):
            print('%-6s %4d %4d %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %-17s %-11s %3d %5.2f %6s %6s %5s %5s %5s %6s %5.0f %7s %7s %5.2f%s'
                  % (arm, r['seed'], r['n'], r['none'], r['left_only'], r['right_only'], r['bi_toward'], r['bi_away'],
                     r['bi_same'], r['rnd1s'], r['rndbi'], '%s(%.2f)' % (r['dom'], r[r['dom']]),
                     '%s(%s)' % (r['feat'][0], fmt(r['feat_share'])), r['lin'], r['top'], fmt(r.get('food_h')),
                     fmt(r.get('fish_h')), fmt(r.get('frac_food_both')), fmt(r.get('frac_food_one')),
                     fmt(r.get('frac_one_bias')), fmt(r['search'], 3), r['pop'], fmt(r['t25']), fmt(r['t50']),
                     r['maxbi'], '  SWEEP' if r['sweep'] else ''))
            if course:
                print('        course (step: left wire / right wire / both): ' +
                      ' '.join('%dk:%.2f/%.2f/%.2f' % (s // 1000, L, R, B) for s, L, R, B, _ in r['course']))
    print('\n== per arm: sweeping (food_h > 0.5) vs other seeds; medians over seeds')
    for arm in sorted(by):
        sw = [r for r in by[arm] if r['sweep']]; ns = [r for r in by[arm] if not r['sweep']]
        def cell(g):
            if not g: return '-'
            return ('one-sided %.2f bilateral %.2f; classes %s; features %s'
                    % (med([r['one'] for r in g]), med([r['bi'] for r in g]),
                       dict(Counter(r['dom'] for r in g)), dict(Counter(r['feat'][0] for r in g))))
        print('%-6s sweeps %d/%d | sweeping: %s | other: %s' % (arm, len(sw), len(by[arm]), cell(sw), cell(ns)))

if __name__ == '__main__':
    main()
