"""Correctness tests for the interval operators (spec §5).

python tests/test_intervals.py [--tp N]   (exits non-zero on failure)

1. Every operator vs an independent brute-force Python reference on
   hand-written edge cases (empty input, dur = 0, ties, nested and abutting
   intervals, NULL bounds, NULL PER keys) and randomized inputs.
2. INTERVAL INTERSECTION (2- and 3-way) and INTERVAL FLATTEN vs TP main's
   PerfettoSQL Next (pipe syntax) on N real traces (default 4), full row
   multiset comparison.
3. Negative dur / ts raise errors like TP.
"""
import collections
import itertools
import os
import random
import subprocess
import sys
import csv
import io

import duckdb

HERE = os.path.dirname(os.path.abspath(__file__))
_SRC_EXT = os.path.join(HERE, '..', 'perfetto.duckdb_extension')


def _private_ext():
  # Load a private copy: other agents rebuild the shared file in place.
  import shutil
  import tempfile
  d = tempfile.mkdtemp(prefix='pf_intervals_')
  dst = os.path.join(d, 'perfetto.duckdb_extension')
  shutil.copy(_SRC_EXT, dst)
  return dst


EXT = _private_ext()
TP = os.environ.get(
    'TP_SHELL',
    os.path.expanduser('~/bt_bench/tp_arrow/out/rel/trace_processor_shell'))
RAW = os.environ.get('IVL_RAW_DIR',
                     os.path.expanduser('~/bt_bench/duck_intervals/raw600'))

FAILS = []


def check(name, got, want):
  g, w = collections.Counter(got), collections.Counter(want)
  if g != w:
    FAILS.append(name)
    print(f'FAIL {name}: got-want={list((g - w).elements())[:6]} '
          f'want-got={list((w - g).elements())[:6]}')
  return g == w


def connect():
  c = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  c.execute(f"LOAD '{EXT}'")
  return c


# ---------------------------------------------------------------------------
# Brute-force references. Rows: dicts with k (PER key), ts, dur, id.
# ---------------------------------------------------------------------------
def end(r):
  return r['ts'] + r['dur']


def valid(r):
  return r['ts'] is not None and r['dur'] is not None


def ov(a, b):
  """PerfettoSQL overlap: half-open, points intersect covering/equal."""
  ap, bp = a['dur'] == 0, b['dur'] == 0
  if ap and bp:
    return a['ts'] == b['ts']
  if ap:
    return b['ts'] <= a['ts'] < end(b)
  if bp:
    return a['ts'] <= b['ts'] < end(a)
  return a['ts'] < end(b) and b['ts'] < end(a)


def covers_instant(b, t):
  return b['ts'] == t if b['dur'] == 0 else b['ts'] <= t < end(b)


def groups(rows):
  g = collections.defaultdict(list)
  for r in rows:
    if valid(r):
      g[r['k']].append(r)
  return g


def ref_intersect(ops):
  gs = [groups(o) for o in ops]
  out = []
  for k in gs[0]:
    if not all(k in g for g in gs):
      continue
    for combo in itertools.product(*[g[k] for g in gs]):
      reg = dict(ts=combo[0]['ts'], dur=combo[0]['dur'])
      ok = True
      for r in combo[1:]:
        if not ov(reg, r):
          ok = False
          break
        s = max(reg['ts'], r['ts'])
        e = max(s, min(end(reg), end(r)))
        reg = dict(ts=s, dur=e - s)
      if ok:
        out.append((reg['ts'], reg['dur'], k) + tuple(r['id'] for r in combo))
  return out


def elementary(rows):
  """TP FLATTEN segmentation: yields (ts, dur, live_rows)."""
  bounds = sorted({r['ts'] for r in rows}
                  | {end(r) for r in rows if r['dur'] > 0})
  segs = []
  for t in sorted({r['ts'] for r in rows if r['dur'] == 0}):
    live = [
        r for r in rows if (r['dur'] == 0 and r['ts'] == t) or
        (r['dur'] > 0 and r['ts'] <= t < end(r))
    ]
    segs.append((t, 0, live))
  for a, b in zip(bounds, bounds[1:]):
    live = [r for r in rows if r['dur'] > 0 and r['ts'] <= a and end(r) >= b]
    if live:
      segs.append((a, b - a, live))
  return segs


def ref_flatten(rows):
  out = []
  for k, g in groups(rows).items():
    for ts, dur, live in elementary(g):
      sv = [r['v'] for r in live if r['v'] is not None]
      out.append((ts, dur, k, len(live), sum(sv) if sv else None))
  return out


def ref_union(ops):
  gs = [groups(o) for o in ops]
  out = []
  for k in set().union(*[set(g) for g in gs]):
    tagged = []
    for i, g in enumerate(gs):
      for r in g.get(k, []):
        tagged.append(dict(r, side=i))
    segs = elementary(tagged)

    # Per segment, combos; then coalesce adjacent positive segments.
    def combos(live):
      per = [
          sorted(r['id'] for r in live if r['side'] == i) or
          [None] for i in range(len(ops))
      ]
      return set(itertools.product(*per))

    pos = sorted((s for s in segs if s[1] > 0), key=lambda s: s[0])
    for ts, dur, live in segs:
      if dur == 0:
        for c in combos(live):
          out.append((ts, 0, k) + c)
    open_ = {}
    prev_end = None
    for ts, dur, live in pos:
      cs = combos(live)
      if prev_end != ts:
        for c, s in open_.items():
          out.append((s, prev_end - s, k) + c)
        open_ = {}
      nxt = {}
      for c in cs:
        nxt[c] = open_.pop(c, ts)
      for c, s in open_.items():
        out.append((s, ts - s, k) + c)
      open_ = nxt
      prev_end = ts + dur
    for c, s in open_.items():
      out.append((s, prev_end - s, k) + c)
  return out


def coverage(rows):
  """Merged positive coverage (abutting merges) + isolated points."""
  pos = sorted((r['ts'], end(r)) for r in rows if r['dur'] > 0)
  cov = []
  for s, e in pos:
    if cov and s <= cov[-1][1]:
      cov[-1][1] = max(cov[-1][1], e)
    else:
      cov.append([s, e])
  pts = sorted({
      r['ts'] for r in rows if r['dur'] == 0
      if not any(s <= r['ts'] < e for s, e in cov)
  })
  return cov, pts


def ref_merge(rows):
  out = []
  for k, g in groups(rows).items():
    # Union-find over overlapping-or-abutting pairs.
    parent = list(range(len(g)))

    def find(x):
      while parent[x] != x:
        parent[x] = parent[parent[x]]
        x = parent[x]
      return x

    for i, j in itertools.combinations(range(len(g)), 2):
      a, b = g[i], g[j]
      if a['ts'] <= end(b) and b['ts'] <= end(a):
        parent[find(i)] = find(j)
    comp = collections.defaultdict(list)
    for i in range(len(g)):
      comp[find(i)].append(g[i])
    for rs in comp.values():
      s = min(r['ts'] for r in rs)
      e = max(end(r) for r in rs)
      out.append((s, e - s, k, len(rs)))
  return out


def ref_split(rows, oprows):
  out = []
  og = groups(oprows)
  for r in rows:
    if not valid(r):
      continue
    ops = [o for o in og.get(r['k'], []) if ov(r, o)]
    if not ops:
      out.append((r['id'], r['ts'], r['dur'], None))
      continue
    if r['dur'] == 0:
      out += [(r['id'], r['ts'], 0, o['id']) for o in ops]
      continue
    cuts = sorted(
        {r['ts'], end(r)}
        | {x for o in ops for x in (o['ts'], end(o)) if r['ts'] < x < end(r)})
    for a, b in zip(cuts, cuts[1:]):
      if any(o['dur'] == 0 and o['ts'] == a for o in ops):
        out += [(r['id'], a, 0, o['id']) for o in ops if covers_instant(o, a)]
      hit = [o for o in ops if o['dur'] > 0 and o['ts'] <= a and end(o) >= b]
      out += [(r['id'], a, b - a, o['id']) for o in hit
             ] or [(r['id'], a, b - a, None)]
  return out


def ref_subtract(rows, oprows):
  out = []
  og = groups(oprows)
  for r in rows:
    if not valid(r):
      continue
    cov, pts = coverage(og.get(r['k'], []))
    if r['dur'] == 0:
      if not (any(s <= r['ts'] < e for s, e in cov) or r['ts'] in pts):
        out.append((r['id'], r['ts'], 0))
      continue
    # Instant-by-instant (integer timestamps), then run-length encode.
    run = None
    for t in range(r['ts'], end(r) + 1):
      inside = t < end(r) and not any(s <= t < e for s, e in cov)
      if inside and run is None:
        run = t
      if not inside and run is not None:
        out.append((r['id'], run, t - run))
        run = None
  return out


def rel_holds(rel, a, b):
  ap, bp = a['dur'] == 0, b['dur'] == 0
  if rel == 'overlapping':
    return ov(a, b)
  if rel == 'covering begin':
    return covers_instant(b, a['ts'])
  if rel == 'covering end':
    return covers_instant(b, end(a))
  if rel == 'covering bounds':
    if ap:
      return covers_instant(b, a['ts'])
    return not bp and b['ts'] <= a['ts'] and end(a) <= end(b)
  if rel == 'within bounds':
    if ap:
      return bp and b['ts'] == a['ts']
    if bp:
      return a['ts'] <= b['ts'] < end(a)
    return a['ts'] <= b['ts'] and end(b) <= end(a)


def ref_join(rows, oprows, rel, left):
  out = []
  og = groups(oprows)
  for r in rows:
    if not valid(r):
      continue
    m = [o['id'] for o in og.get(r['k'], []) if rel_holds(rel, r, o)]
    out += [(r['id'], x) for x in m]
    if left and not m:
      out.append((r['id'], None))
  return out


def ref_keep(rows, oprows, rel):
  keep = []
  og = groups(oprows)
  for r in rows:
    if not valid(r):
      continue
    cov, pts = coverage(og.get(r['k'], []))
    blobs = [dict(ts=s, dur=e - s) for s, e in cov
            ] + [dict(ts=p, dur=0) for p in pts]
    if any(rel_holds(rel, r, b) for b in blobs):
      keep.append(r['id'])
  return keep


def ref_find(rows, oprows, edge, direction, anchor, within):
  out = []
  og = groups(oprows)
  for r in rows:
    if anchor == 'begin' or (anchor == 'bounds' and direction == 'before'):
      x = r['ts']
    else:
      x = end(r)
    cands = []
    for o in og.get(r['k'], []):
      y = o['ts'] if edge == 'starting' else end(o)
      if direction == 'after' and y >= x and (within is None or
                                              y - x <= within):
        cands.append((y - x, o['ts'], o['dur'], o['id']))
      if direction == 'before' and y <= x and (within is None or
                                               x - y <= within):
        cands.append((x - y, o['ts'], o['dur'], o['id']))
    best = min(cands) if cands else None
    out.append((r['id'],) + ((best[1], best[2]) if best else (None, None)))
  return out


def ref_from_events(ev, stops, mode):
  out = []
  g = collections.defaultdict(list)
  for e in ev:
    g[e['k']].append(e)
  sg = collections.defaultdict(list)
  for s in stops:
    sg[s['k']].append(s['ts'])
  for k, es in g.items():
    es = sorted(es, key=lambda e: (e['ts'], e['id']))
    for i, e in enumerate(es):
      nxt = [es[i + 1]['ts']] if i + 1 < len(es) else []
      nxt += [t for t in sg.get(k, []) if t >= e['ts']]
      if nxt:
        out.append((e['id'], e['ts'], min(nxt) - e['ts']))
      elif mode == 'keep':
        out.append((e['id'], e['ts'], -1))
      elif mode is not None:
        out.append((e['id'], e['ts'], mode - e['ts']))
  return out


# ---------------------------------------------------------------------------
# Test data.
# ---------------------------------------------------------------------------
EDGE_A = [  # (k, ts, dur, id, v)
    (0, 0, 10, 1, 5),
    (0, 10, 10, 2, None),
    (0, 5, 0, 3, 1),
    (0, 10, 0, 4, 2),
    (0, 2, 6, 5, 3),
    (0, 2, 6, 6, 4),
    (0, 30, 5, 7, 1),
    (0, 35, 5, 8, 1),
    (0, None, 5, 9, 1),
    (0, 50, None, 10, 1),
    (None, 0, 4, 11, 7),
    (None, 2, 4, 12, 7),
    (1, 100, 0, 13, 1),
    (1, 100, 0, 14, 1),
    (0, 0, 40, 15, 2),
    (0, 12, 3, 16, 9),
]
EDGE_B = [
    (0, 5, 10, 101, 0),
    (0, 10, 0, 102, 0),
    (0, 20, 15, 103, 0),
    (0, 40, 1, 104, 0),
    (None, 3, 1, 105, 0),
    (1, 100, 0, 106, 0),
    (2, 0, 5, 107, 0),
    (0, 5, 0, 108, 0),
]


def rand_rows(rng, n, base_id):
  rows = []
  for i in range(n):
    k = rng.choice([0, 1, None]) if rng.random() < 0.9 else 2
    ts = rng.randrange(0, 60)
    dur = rng.choice([0, 0, rng.randrange(1, 25), rng.randrange(1, 6)])
    if rng.random() < 0.03:
      ts = None
    rows.append(
        (k, ts, dur, base_id + i, rng.choice([None, rng.randrange(0, 9)])))
  return rows


def load(c, name, rows):
  c.execute(
      f'CREATE OR REPLACE TABLE {name} (k INTEGER, ts BIGINT, dur BIGINT, id BIGINT, v BIGINT)'
  )
  if rows:
    c.executemany(f'INSERT INTO {name} VALUES (?, ?, ?, ?, ?)', rows)
  return [dict(k=r[0], ts=r[1], dur=r[2], id=r[3], v=r[4]) for r in rows]


def run_suite(c, tag, A, B, C):
  a = load(c, 'ta', A)
  b = load(c, 'tb', B)
  cc = load(c, 'tc', C)
  P = "per := ['k']"
  q = lambda s: c.sql(s).fetchall()
  check(
      f'{tag} intersect2',
      q(f"SELECT ts, dur, k, x.id, y.id FROM pf_interval_intersection(['ta','tb'], ['x','y'], {P})"
       ), ref_intersect([a, b]))
  check(
      f'{tag} intersect3',
      q(f"SELECT ts, dur, k, x.id, y.id, z.id FROM pf_interval_intersection(['ta','tb','tc'], ['x','y','z'], {P})"
       ), ref_intersect([a, b, cc]))
  if len(cc) <= 8:
    check(
        f'{tag} intersect5',
        q(f"SELECT ts, dur, k, v.id, w.id, x.id, y.id, z.id FROM pf_interval_intersection(['tc','tc','tb','tc','tc'], ['v','w','x','y','z'], {P})"
         ), ref_intersect([cc, cc, b, cc, cc]))
  check(
      f'{tag} intersect_noper', [
          r[:2] + (None,) + r[2:] for r in
          q("SELECT ts, dur, x.id, y.id FROM pf_interval_intersection(['ta','tb'], ['x','y'])"
           )
      ],
      ref_intersect([[dict(r, k=None) for r in a],
                     [dict(r, k=None) for r in b]]))
  check(
      f'{tag} union2',
      q(f"SELECT ts, dur, k, x.id, y.id FROM pf_interval_union(['ta','tb'], ['x','y'], {P})"
       ), ref_union([a, b]))
  check(
      f'{tag} flatten_sum',
      q(f"SELECT ts, dur, k, count, sum_v FROM pf_interval_flatten_sum('ta', ['v'], {P})"
       ), ref_flatten(a))
  check(
      f'{tag} flatten_generic',
      q(f"SELECT ts, dur, k, c, s FROM pf_interval_flatten('ta', 'count(*) AS c, sum(v) AS s', {P})"
       ), ref_flatten(a))
  check(
      f'{tag} merge_overlapping',
      q(f"SELECT ts, dur, k, n FROM pf_interval_merge_overlapping('ta', 'count(*) AS n', {P})"
       ), ref_merge(a))
  check(
      f'{tag} split',
      q(f"SELECT id, ts, dur, o.id FROM pf_interval_split('ta', 'tb', 'o', {P})"
       ), ref_split(a, b))
  check(f'{tag} subtract',
        q(f"SELECT id, ts, dur FROM pf_interval_subtract('ta', 'tb', {P})"),
        ref_subtract(a, b))
  for rel in [
      'overlapping', 'covering begin', 'covering end', 'covering bounds',
      'within bounds'
  ]:
    for left in (False, True):
      check(
          f'{tag} join {rel} left={left}',
          q(f"SELECT id, o.id FROM pf_interval_join('ta', 'tb', 'o', '{rel}', {P}, left_join := {left})"
           ), ref_join(a, b, rel, left))
    kept = ref_keep(a, b, rel)
    check(f'{tag} keep {rel}', [
        r[0] for r in q(
            f"SELECT id FROM pf_interval_keep_if('ta', 'tb', '{rel}', {P})")
    ], kept)
    check(f'{tag} drop {rel}', [
        r[0] for r in q(
            f"SELECT id FROM pf_interval_drop_if('ta', 'tb', '{rel}', {P})")
    ], [r['id'] for r in a if r['id'] not in kept])
  # FIND / FROM EVENTS / QUANTIZE operate on rows with non-NULL bounds.
  nn = "(SELECT * FROM {} WHERE ts IS NOT NULL AND dur IS NOT NULL AND k IS NOT NULL)"
  an = [r for r in a if valid(r) and r['k'] is not None]
  bn = [r for r in b if valid(r) and r['k'] is not None]
  for edge, direction, anchor, within in [('starting', 'after', 'end', None),
                                          ('starting', 'after', 'begin', 7),
                                          ('ending', 'before', 'begin', None),
                                          ('ending', 'before', 'bounds', 5),
                                          ('starting', 'before', 'end', None),
                                          ('ending', 'after', 'bounds', None)]:
    w = 'NULL' if within is None else str(within)
    got = q(
        f"SELECT id, o.ts, o.dur FROM pf_interval_find('{nn.format('ta')}', '{nn.format('tb')}', 'o', '{edge}', '{direction}', '{anchor}', within := {w}, {P}, left_join := true)"
    )
    check(f'{tag} find {edge} {direction} {anchor} {within}', got,
          ref_find(an, bn, edge, direction, anchor, within))
    got = q(
        f"SELECT id, o.ts, o.dur FROM pf_interval_find('{nn.format('ta')}', '{nn.format('tb')}', 'o', '{edge}', '{direction}', '{anchor}', within := {w}, {P})"
    )
    check(f'{tag} find-inner {edge} {direction} {anchor} {within}', got, [
        x for x in ref_find(an, bn, edge, direction, anchor, within)
        if x[1] is not None
    ])
  evs = [dict(r) for r in an]
  stops = [dict(k=r['k'], ts=r['ts']) for r in bn]
  for mode, kw in [(None, ''), ('keep', ', keep_last_open := true'),
                   (1000, ", close_last_at := '1000'")]:
    check(
        f'{tag} from_events {mode}',
        q(f"SELECT id, ts, dur FROM pf_intervals_from_events('{nn.format('ta')}', {P}{kw})"
         ), ref_from_events(evs, [], mode))
    check(
        f'{tag} from_events stop {mode}',
        q(f"SELECT id, ts, dur FROM pf_intervals_from_events('{nn.format('ta')}', {P}, stop := '{nn.format('tb')}'{kw})"
         ), ref_from_events(evs, stops, mode))
  # MERGE CONSECUTIVE BY v (rows in (ts, dur) order; ties made unique here).
  uniq = {}
  for r in an:
    uniq.setdefault((r['k'], r['ts'], r['dur']), r)
  rows = sorted(uniq.values(), key=lambda r: (r['k'], r['ts'], r['dur']))
  want = []
  for k, grp in itertools.groupby(rows, key=lambda r: r['k']):
    run = None
    for r in grp:
      if run and r['v'] == run['v'] and r['ts'] <= run['last_end']:
        run['e'] = max(run['e'], end(r))
        run['n'] += 1
        run['last_end'] = end(r)
      else:
        if run:
          want.append((run['s'], run['e'] - run['s'], k, run['v'], run['n']))
        run = dict(s=r['ts'], e=end(r), v=r['v'], n=1, last_end=end(r))
    if run:
      want.append((run['s'], run['e'] - run['s'], k, run['v'], run['n']))
  ids = ','.join(str(r['id']) for r in rows) or '-1'
  check(
      f'{tag} merge_consecutive',
      q(f"SELECT ts, dur, k, v, n FROM pf_interval_merge_consecutive('(SELECT * FROM ta WHERE id IN ({ids}))', ['v'], 'count(*) AS n', {P})"
       ), want)
  # QUANTIZE (zero and expression origin) and SET BOUNDS.
  for origin, o in [('zero', 0), ('3', 3)]:
    want = []
    for r in an:
      w = 7
      if r['dur'] == 0:
        b0 = (r['ts'] - o) // w
        want.append((r['id'], r['ts'], 0, o + b0 * w))
        continue
      for bk in range((r['ts'] - o) // w, (end(r) - 1 - o) // w + 1):
        s = max(r['ts'], o + bk * w)
        e = min(end(r), o + (bk + 1) * w)
        want.append((r['id'], s, e - s, o + bk * w))
    check(
        f'{tag} quantize {origin}',
        q(f"SELECT id, ts, dur, bucket FROM pf_interval_quantize('{nn.format('ta')}', 7, '{origin}')"
         ), want)
  check(
      f'{tag} set_bounds',
      q(f"SELECT id, ts, dur FROM pf_interval_set_bounds('{nn.format('ta')}', 'ts + 1', end_at := 'ts + dur + 5')"
       ), [(r['id'], r['ts'] + 1, r['dur'] + 4) for r in an])
  # FILL GAPS WITH (and alias fill_within): input passes through + fillers over uncovered `bg` carrying `bg` payload.
  within = [
      dict(k=0, ts=0, dur=70, id=0, bg_lbl='bg0'),
      dict(k=1, ts=90, dur=20, id=0, bg_lbl='bg1')
  ]
  c.execute(
      "CREATE OR REPLACE TABLE tw AS SELECT * FROM (VALUES (0, 0::BIGINT, 70::BIGINT, 'bg0'), (1, 90, 20, 'bg1')) t(k, ts, dur, bg_lbl)"
  )
  want = [(r['k'], r['ts'], r['dur'], r['id'], None) for r in a]
  want += [(w_['k'], s, d, None, w_['bg_lbl'])
           for w_ in within
           for (_, s, d) in ref_subtract([w_], a)]
  check(
      f'{tag} fill_gaps',
      q(f"SELECT k, ts, dur, id, bg_lbl FROM pf_interval_fill_gaps('ta', 'tw', {P})"
       ), want)
  check(
      f'{tag} fill_within',
      q(f"SELECT k, ts, dur, id, bg_lbl FROM pf_interval_fill_within('ta', 'tw', {P})"
       ), want)
  # FILL GAPS WITH open-ended input (dur = -1 covers to end of time) and bg_has_per := false.
  if tag == 'edge':
    c.execute(
        "CREATE OR REPLACE TABLE t_open AS SELECT * FROM (VALUES (0, 10::BIGINT, 20::BIGINT, 1::BIGINT), (0, 50, -1, 2), (1, 0, 15, 3)) t(k, ts, dur, id)"
    )
    c.execute(
        "CREATE OR REPLACE TABLE t_bg_noper AS SELECT * FROM (VALUES (0::BIGINT, 100::BIGINT, 'win')) t(ts, dur, bg_tag)"
    )
    got_open = q(
        "SELECT k, ts, dur, id, bg_tag FROM pf_interval_fill_gaps('t_open', 't_bg_noper', per := ['k'], bg_has_per := false)"
    )
    want_open = [
        (0, 10, 20, 1, None),
        (0, 50, -1, 2, None),
        (1, 0, 15, 3, None),
        (0, 0, 10, None, 'win'),
        (0, 30, 20, None, 'win'),
        (1, 15, 85, None, 'win'),
    ]
    check('edge fill_gaps open + bg_has_per=false', got_open, want_open)


def test_errors(c):
  load(c, 'te', [(0, 0, -1, 1, 0)])
  load(c, 'tf', [(0, 0, 5, 2, 0)])
  for name, sql in [('intersect',
                     "FROM pf_interval_intersection(['te','tf'], ['a','b'])"),
                    ('flatten', "FROM pf_interval_flatten_sum('te')"),
                    ('join', "FROM pf_interval_join('tf', 'te', 'o')")]:
    try:
      c.sql(sql).fetchall()
      FAILS.append(f'error {name}')
      print(f'FAIL error {name}: no error for dur < 0')
    except duckdb.Error as e:
      if 'below zero' not in str(e):
        FAILS.append(f'error {name}')
        print(f'FAIL error {name}: {e}')


def test_empty(c):
  load(c, 'ta', [])
  load(c, 'tb', EDGE_B)
  for sql in [
      "FROM pf_interval_intersection(['ta','tb'], ['a','b'], per := ['k'])",
      "FROM pf_interval_union(['ta','ta'], ['a','b'])",
      "FROM pf_interval_flatten('ta', 'count(*) AS c')",
      "FROM pf_interval_flatten_sum('ta', ['v'])",
      "FROM pf_interval_merge_overlapping('ta')",
      "FROM pf_interval_split('ta', 'tb', 'o')",
      "FROM pf_interval_join('ta', 'tb', 'o', left_join := true)",
      "FROM pf_interval_keep_if('ta', 'tb')"
  ]:
    check(f'empty {sql}', c.sql(sql).fetchall(), [])
  check('empty operand join left', [
      r[0] for r in c.sql(
          "SELECT id FROM pf_interval_join('tb', 'ta', 'o', left_join := true) WHERE o IS NULL"
      ).fetchall()
  ], [r[3] for r in EDGE_B])


# ---------------------------------------------------------------------------
# TP main oracle (PerfettoSQL Next pipe syntax) on real traces.
# ---------------------------------------------------------------------------
TP_SQL = """PERFETTO PRAGMA pipelines = 1;
CREATE PERFETTO TABLE fl AS FROM (SELECT ts, dur, track_id, depth FROM slice WHERE dur >= 0)
  |> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS c, SUM(depth) AS sd;
SELECT ts, dur, track_id, c, sd FROM fl;
CREATE PERFETTO TABLE ii AS INTERVAL INTERSECTION OF (
  (SELECT id, ts, dur, utid FROM thread_state WHERE dur >= 0) AS a,
  (SELECT s.id, s.ts, s.dur, t.utid FROM slice s JOIN thread_track t ON s.track_id = t.id WHERE s.dur >= 0) AS b)
  PER utid |> SELECT ts, dur, utid, a.id AS a_id, b.id AS b_id;
SELECT * FROM ii;
CREATE PERFETTO TABLE i3 AS INTERVAL INTERSECTION OF (
  (SELECT id, ts, dur, utid FROM thread_state WHERE dur >= 0) AS a,
  (SELECT s.id, s.ts, s.dur, t.utid FROM slice s JOIN thread_track t ON s.track_id = t.id WHERE s.dur >= 0) AS b,
  (SELECT id, ts, dur, utid FROM sched WHERE dur >= 0) AS c)
  PER utid |> SELECT ts, dur, utid, a.id AS a_id, b.id AS b_id, c.id AS c_id;
SELECT * FROM i3;
"""


def tp_sections(path):
  p = subprocess.run([TP, 'query', '--quiet', '-f', '-', path],
                     input=TP_SQL,
                     capture_output=True,
                     text=True,
                     check=True)
  secs, cur = [], []
  for row in csv.reader(io.StringIO(p.stdout)):
    if not row:
      if cur:
        secs.append(cur)
      cur = []
    else:
      cur.append(row)
  if cur:
    secs.append(cur)
  return [[tuple(None if v == '[NULL]' else int(v)
                 for v in r)
           for r in s[1:]]
          for s in secs]


def test_tp(c, n):
  if not os.path.exists(os.path.join(RAW, 'traces.txt')):
    print('skip TP oracle: no extracted traces')
    return
  picked = c.sql(f"""
    SELECT s.trace_idx FROM read_parquet('{RAW}/slice/*.parquet') s
    SEMI JOIN read_parquet('{RAW}/thread_state/*.parquet') t USING (trace_idx)
    SEMI JOIN read_parquet('{RAW}/sched/*.parquet') u USING (trace_idx)
    GROUP BY 1 HAVING count(*) BETWEEN 5000 AND 400000 ORDER BY 1 LIMIT {n}"""
                ).fetchall()
  paths = dict(l.split() for l in open(os.path.join(RAW, 'traces.txt')))
  for (idx,) in picked:
    fl, ii, i3 = tp_sections(paths[str(idx)])
    for t in ['slice', 'thread_track', 'thread_state', 'sched']:
      c.execute(
          f"CREATE OR REPLACE TEMP VIEW r_{t} AS SELECT * FROM read_parquet('{RAW}/{t}/*.parquet') WHERE trace_idx = {idx}"
      )
    c.execute(
        "CREATE OR REPLACE TEMP VIEW r_s AS SELECT ts, dur, track_id, depth FROM r_slice WHERE dur >= 0"
    )
    c.execute(
        "CREATE OR REPLACE TEMP VIEW r_a AS SELECT id, ts, dur, utid FROM r_thread_state WHERE dur >= 0"
    )
    c.execute(
        "CREATE OR REPLACE TEMP VIEW r_b AS SELECT s.id, s.ts, s.dur, t.utid FROM r_slice s JOIN r_thread_track t ON s.track_id = t.id WHERE s.dur >= 0"
    )
    c.execute(
        "CREATE OR REPLACE TEMP VIEW r_c AS SELECT id, ts, dur, utid FROM r_sched WHERE dur >= 0"
    )
    check(
        f'tp{idx} flatten_sum ({len(fl)} rows)',
        c.sql(
            "SELECT ts, dur, track_id, count, sum_depth FROM pf_interval_flatten_sum('r_s', ['depth'], per := ['track_id'])"
        ).fetchall(), fl)
    check(
        f'tp{idx} flatten_generic',
        c.sql(
            "SELECT ts, dur, track_id, c, sd FROM pf_interval_flatten('r_s', 'count(*) AS c, sum(depth) AS sd', per := ['track_id'])"
        ).fetchall(), fl)
    check(
        f'tp{idx} intersect2 ({len(ii)} rows)',
        c.sql(
            "SELECT ts, dur, utid, a.id, b.id FROM pf_interval_intersection(['r_a', 'r_b'], ['a', 'b'], per := ['utid'])"
        ).fetchall(), ii)
    check(
        f'tp{idx} intersect3 ({len(i3)} rows)',
        c.sql(
            "SELECT ts, dur, utid, a.id, b.id, c.id FROM pf_interval_intersection(['r_a', 'r_b', 'r_c'], ['a', 'b', 'c'], per := ['utid'])"
        ).fetchall(), i3)
    print(
        f'tp oracle trace {idx}: flatten {len(fl)}, intersect2 {len(ii)}, intersect3 {len(i3)} rows'
    )


def main():
  n_tp = int(sys.argv[sys.argv.index('--tp') + 1]) if '--tp' in sys.argv else 4
  c = connect()
  c.execute("SET threads = 8")
  run_suite(c, 'edge', EDGE_A, EDGE_B, EDGE_A[:5] + EDGE_B[:3])
  rng = random.Random(7)
  for i in range(40):
    run_suite(c, f'rand{i}', rand_rows(rng, rng.randrange(0, 40), 1),
              rand_rows(rng, rng.randrange(0, 30), 1000),
              rand_rows(rng, rng.randrange(1, 15), 5000))
  test_errors(c)
  test_empty(c)
  test_tp(c, n_tp)
  if FAILS:
    print(
        f'{len(FAILS)} FAILED: {sorted(set(f.split(" ", 1)[1] if f.startswith(("rand", "edge")) else f for f in FAILS))[:30]}'
    )
    sys.exit(1)
  print('all interval tests passed')


if __name__ == '__main__':
  main()
