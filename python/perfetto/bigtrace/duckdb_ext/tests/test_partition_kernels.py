# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Per-trace contract tests for the partition kernels.

Run: python tests/test_partition_kernels.py   (exits non-zero on failure)

The interval / tree operators must give identical results whatever the type
of the trace partition key (ENUM as in the bigtrace server, VARCHAR,
INTEGER), whatever the physical row order (TP's ts / id order, shuffled),
and whatever the thread count (parts of one trace spread over threads).
Brute-force Python references pin the results on small inputs; the
invariance checks run on inputs large enough to span many row groups.
Also covers empty input, single rows and NULLs.
"""
import csv
import os
import random
import sys
import tempfile

import duckdb

from _ext import EXT

FAILS = []


def check(name, got, want):
  if sorted(got, key=repr) != sorted(want, key=repr):
    FAILS.append(name)
    print(f'FAIL {name}: got {len(got)} rows, want {len(want)}')
    print('  got  ', sorted(got, key=repr)[:5])
    print('  want ', sorted(want, key=repr)[:5])


def connect(threads=1):
  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute(f"LOAD '{EXT}'")
  con.execute(f'SET threads = {threads}')
  return con


# ---------------------------------------------------------------------------
# Data: per trace, nested slices on a few tracks (TP id / ts order), plus
# disjoint "thread state" style intervals per lane.
# ---------------------------------------------------------------------------
def gen_slices(rng, ntraces, per_trace):
  rows = []  # (trace, id, parent_id, ts, dur, track_id, depth)
  for t in range(ntraces):
    stack = []
    ts = 0
    for i in range(per_trace):
      ts += rng.randrange(0, 5)
      while stack and stack[-1][1] <= ts:
        stack.pop()
      parent = stack[-1][0] if stack else None
      lim = stack[-1][1] - ts if stack else 50
      dur = rng.choice([0, rng.randrange(1, max(2, lim))])
      track = rng.randrange(3)
      rows.append((t, i, parent, ts, dur, track, len(stack)))
      if dur > 0:
        stack.append((i, ts + dur))
  return rows


def load(con, name, rows, cols, key_type, shuffle=False, rng=None):
  rows = list(rows)
  if shuffle:
    rng.shuffle(rows)
  con.execute(f'DROP TABLE IF EXISTS {name}')
  decl = ', '.join(f'{c} BIGINT' for c in cols[1:])
  con.execute(f'CREATE TABLE {name} (tr_raw BIGINT, {decl})')
  if rows:
    with tempfile.NamedTemporaryFile('w', suffix='.csv', delete=False) as f:
      csv.writer(f).writerows(rows)
    con.execute(f"COPY {name} FROM '{f.name}' (HEADER false, NULLSTR '')")
    os.unlink(f.name)
  key = {
      'enum': "tr_raw::VARCHAR::tr_enum",
      'varchar': "'t' || tr_raw",
      'int': 'tr_raw::INTEGER'
  }[key_type]
  con.execute(
      f'CREATE OR REPLACE TABLE {name} AS SELECT {key} AS tr, * EXCLUDE (tr_raw) FROM {name}'
  )


def key_back(v):
  return int(v[1:]) if isinstance(v, str) and v.startswith('t') else int(v)


# References ----------------------------------------------------------------
def ref_accumulate(rows, up):
  by = {}
  for t, i, p, *_ in rows:
    by.setdefault(t, {})[i] = p
  out = []
  dur = {(r[0], r[1]): r[4] for r in rows}
  for t, par in by.items():
    tot = {i: dur[(t, i)] for i in par}
    if up:
      for i in sorted(par, reverse=True):
        if par[i] is not None:
          tot[par[i]] += tot[i]
    else:
      for i in sorted(par):
        if par[i] is not None:
          tot[i] += tot[par[i]]
    out += [(t, i, v) for i, v in tot.items()]
  return out


def ref_keep_desc(rows, marks):
  par = {(r[0], r[1]): r[2] for r in rows}
  out = []
  for t, i, *_ in rows:
    p = par[(t, i)]
    while p is not None:
      if (t, p) in marks:
        out.append((t, i))
        break
      p = par[(t, p)]
  return out


def ref_intersect(a, b):
  ep = lambda s, d: s + max(d, 1)
  out = []
  for x in a:
    for y in b:
      if x[0] != y[0] or x[5] != y[5]:
        continue
      if x[3] < ep(y[3], y[4]) and y[3] < ep(x[3], x[4]):
        s = max(x[3], y[3])
        e = max(s, min(x[3] + x[4], y[3] + y[4]))
        out.append((x[0], x[5], s, e - s, x[1], y[1]))
  return out


def ref_flatten(rows):
  out = []
  lanes = {}
  for r in rows:
    if r[4] >= 0:
      lanes.setdefault((r[0], r[5]), []).append((r[3], r[4]))
  for (t, k), iv in lanes.items():
    pts = sorted({s for s, _ in iv} | {s + d for s, d in iv if d > 0})
    for a, b2 in zip(pts, pts[1:]):
      c = sum(1 for s, d in iv if d > 0 and s <= a and s + d >= b2)
      if c:
        out.append((t, k, a, b2 - a, c))
    for p in sorted({s for s, d in iv if d == 0}):
      c = sum(
          1 for s, d in iv if (d == 0 and s == p) or (d > 0 and s <= p < s + d))
      out.append((t, k, p, 0, c))
  return out


Q = {
    'acc_up':
        "SELECT tr, id, total FROM pf_tree_accumulate_up('s', 'dur', per := ['tr'])",
    'acc_down':
        "SELECT tr, id, total FROM pf_tree_accumulate_down('s', 'dur', per := ['tr'])",
    'keep_desc':
        "SELECT tr, id FROM pf_tree_keep_if('s', 'descendant', 'm', per := ['tr'], full_row := false)",
    'flatten':
        "SELECT tr, track_id, ts, dur, c FROM pf_interval_flatten('s', 'count(*) AS c', per := ['tr', 'track_id'])",
    'ii2': (
        "SELECT tr, track_id, ts, dur, a.id, b.id FROM pf_interval_intersection("
        "['(SELECT * FROM s WHERE depth = 0)', '(SELECT * FROM s WHERE depth = 1)'], ['a', 'b'], "
        "per := ['tr', 'track_id'])"),
}
COLS = ['tr', 'id', 'parent_id', 'ts', 'dur', 'track_id', 'depth']


def run_all(con):
  out = {}
  for name, q in Q.items():
    out[name] = [
        tuple(key_back(v) if j == 0 else v
              for j, v in enumerate(r))
        for r in con.execute(q).fetchall()
    ]
  return out


def setup(con, rows, marks, key_type, shuffle, rng):
  ntr = max([r[0] for r in rows] + [0]) + 1
  con.execute('DROP TYPE IF EXISTS tr_enum')
  con.execute(
      f"CREATE TYPE tr_enum AS ENUM ({', '.join(repr(str(i)) for i in range(ntr))})"
  )
  load(con, 's', rows, COLS, key_type, shuffle, rng)
  load(con, 'm', [(t, i) for t, i in marks], ['tr', 'id'], key_type)


def main():
  rng = random.Random(7)
  # 1. Small inputs vs brute-force references, every key type and order.
  rows = gen_slices(rng, 4, 120)
  marks = {(r[0], r[1]) for r in rows if r[6] == 1 and rng.random() < 0.3}
  want = {
      'acc_up':
          ref_accumulate(rows, True),
      'acc_down':
          ref_accumulate(rows, False),
      'keep_desc':
          ref_keep_desc(rows, marks),
      'flatten':
          ref_flatten(rows),
      'ii2':
          ref_intersect([r for r in rows if r[6] == 0],
                        [r for r in rows if r[6] == 1]),
  }
  for key_type in ['enum', 'varchar', 'int']:
    for shuffle in [False, True]:
      con = connect(1)
      setup(con, rows, marks, key_type, shuffle, rng)
      got = run_all(con)
      for name in Q:
        check(f'ref {name} key={key_type} shuffled={shuffle}', got[name],
              want[name])

  # 2. Large inputs (many row groups per trace): 1 vs 8 threads, sorted vs
  # shuffled storage order, ENUM key.
  rows = gen_slices(rng, 6, 60000)
  marks = {(r[0], r[1]) for r in rows if r[6] == 2 and rng.random() < 0.05}
  base = None
  for threads, shuffle in [(1, False), (8, False), (8, True)]:
    con = connect(threads)
    setup(con, rows, marks, 'enum', shuffle, rng)
    got = run_all(con)
    if base is None:
      base = got
      continue
    for name in Q:
      check(f'large {name} threads={threads} shuffled={shuffle}', got[name],
            base[name])

  # 3. Edge cases: empty input, single rows, NULLs.
  con = connect(4)
  setup(con, [], set(), 'enum', False, rng)
  for name, q in Q.items():
    check(f'empty {name}', con.execute(q).fetchall(), [])
  setup(con, [(0, 0, None, 5, 3, 1, 0)], set(), 'enum', False, rng)
  got = run_all(con)
  check('single acc_up', got['acc_up'], [(0, 0, 3)])
  check('single flatten', got['flatten'], [(0, 1, 5, 3, 1)])
  check('single ii2', got['ii2'], [])
  con.execute(
      'CREATE OR REPLACE TABLE n (tr INTEGER, id BIGINT, parent_id BIGINT, ts BIGINT, dur BIGINT, track_id BIGINT, depth BIGINT)'
  )
  con.execute(
      'INSERT INTO n VALUES (0, 0, NULL, 0, 10, 1, 0), (0, 1, 0, 2, 3, NULL, 1), '
      '(0, 2, 0, NULL, 3, 1, 1), (0, 3, 0, 4, NULL, 1, 1)')
  check(
      'null-lane flatten',
      con.execute(
          "SELECT track_id, ts, dur, c FROM pf_interval_flatten('n', 'count(*) AS c', per := ['tr', 'track_id'])"
      ).fetchall(), [(1, 0, 10, 1), (None, 2, 3, 1)])
  check(
      'null-value accumulate',
      con.execute(
          "SELECT id, total FROM pf_tree_accumulate_up('(SELECT tr, id, parent_id, CASE WHEN id = 1 THEN NULL ELSE dur END AS dur FROM n)', 'dur', per := ['tr'])"
      ).fetchall(), [(0, 13), (1, 0), (2, 3), (3, 0)])

  test_doc_examples(con)
  test_adversarial_edge_cases(con)
  test_pipe_syntax(con)

  print(f'{len(FAILS)} failures'
        if FAILS else 'all partition kernel tests passed')
  sys.exit(1 if FAILS else 0)


def check_err(con, name, sql, substr):
  try:
    con.execute(sql).fetchall()
    FAILS.append(name)
    print(f'FAIL {name}: expected error containing {substr!r}, got success')
  except Exception as e:
    if substr.lower() not in str(e).lower():
      FAILS.append(name)
      print(f'FAIL {name}: expected {substr!r} in {e!r}')


def test_doc_examples(con):
  con.execute('DROP TYPE IF EXISTS _bt_trace_uuid CASCADE')
  con.execute("CREATE TYPE _bt_trace_uuid AS ENUM ('t1', 't2', 't3')")

  # Synthetic multi-trace Perfetto tables for README, intervals.md, trees.md, graphs.md
  con.execute("""
    CREATE OR REPLACE TABLE trace_bounds (
      _trace_uuid _bt_trace_uuid, start_ts BIGINT, end_ts BIGINT
    );
    INSERT INTO trace_bounds VALUES
      ('t1', 100, 500),
      ('t2', 200, 600);

    CREATE OR REPLACE TABLE thread_track (
      _trace_uuid _bt_trace_uuid, track_id BIGINT, utid BIGINT
    );
    INSERT INTO thread_track VALUES
      ('t1', 10, 1),
      ('t2', 20, 2);

    CREATE OR REPLACE TABLE process_track (
      _trace_uuid _bt_trace_uuid, track_id BIGINT, upid BIGINT
    );
    INSERT INTO process_track VALUES
      ('t1', 10, 100),
      ('t2', 20, 200);

    CREATE OR REPLACE TABLE slice (
      _trace_uuid _bt_trace_uuid,
      id BIGINT,
      parent_id BIGINT,
      ts BIGINT,
      dur BIGINT,
      track_id BIGINT,
      utid BIGINT,
      depth BIGINT,
      name VARCHAR,
      category VARCHAR
    );
    INSERT INTO slice VALUES
      ('t1', 0, NULL, 100, 100, 10, 1, 0, 'Choreographer#doFrame', 'ui'),
      ('t1', 1, 0,    110,  40, 10, 1, 1, 'measure', 'ui'),
      ('t1', 2, 1,    120,  10, 10, 1, 2, 'binder transaction', 'binder'),
      ('t1', 3, 0,    160,  30, 10, 1, 1, 'DebugLog', 'log'),
      ('t1', 4, 3,    165,  10, 10, 1, 2, 'LockContention', 'lock'),
      ('t2', 0, NULL, 200,  80, 20, 2, 0, 'Choreographer#doFrame', 'ui'),
      ('t2', 1, 0,    210,  30, 20, 2, 1, 'Choreographer#doFrame', 'ui'),
      ('t2', 2, 1,    215,  15, 20, 2, 2, 'binder transaction', 'binder');

    CREATE OR REPLACE TABLE sched (
      _trace_uuid _bt_trace_uuid, id BIGINT, ts BIGINT, dur BIGINT, cpu BIGINT, utid BIGINT
    );
    INSERT INTO sched VALUES
      ('t1', 0, 100, 50, 0, 1),
      ('t1', 1, 160, 40, 1, 1),
      ('t2', 0, 205, 50, 0, 2);

    CREATE OR REPLACE TABLE thread_state (
      _trace_uuid _bt_trace_uuid, id BIGINT, ts BIGINT, dur BIGINT, utid BIGINT, state VARCHAR
    );
    INSERT INTO thread_state VALUES
      ('t1', 0, 100, 30, 1, 'Running'),
      ('t1', 1, 130, 20, 1, 'Running'),
      ('t1', 2, 150, 10, 1, 'S'),
      ('t1', 3, 160, 40, 1, 'Running'),
      ('t2', 0, 205, 50, 2, 'Running');
  """)

  # --- docs/intervals.md examples ---
  # 1. 2-way slice x sched intersection per (_trace_uuid, utid)
  got_2way = con.execute("""
    WITH thread_slices AS (
      SELECT s._trace_uuid, s.id, s.ts, s.dur, s.name, tt.utid
      FROM slice AS s JOIN thread_track AS tt USING (_trace_uuid, track_id)
      WHERE s.dur > 0
    )
    SELECT _trace_uuid::VARCHAR, sl.name, SUM(dur)
    FROM pf_interval_intersection(
      ['thread_slices', '(SELECT * FROM sched WHERE dur > 0)'],
      ['sl', 'sc'],
      per := ['_trace_uuid', 'utid'],
      full_row := true
    )
    GROUP BY _trace_uuid, sl.name
  """).fetchall()
  check('doc intervals 2-way slice x sched', got_2way, [
      ('t1', 'Choreographer#doFrame', 90),
      ('t1', 'measure', 40),
      ('t1', 'binder transaction', 10),
      ('t1', 'DebugLog', 30),
      ('t1', 'LockContention', 10),
      ('t2', 'Choreographer#doFrame', 80),
      ('t2', 'binder transaction', 15),
  ])

  # 2. 3-way slice x thread_state x sched intersection
  got_3way = con.execute("""
    WITH thread_slices AS (
      SELECT s._trace_uuid, s.id, s.ts, s.dur, s.name, tt.utid
      FROM slice AS s JOIN thread_track AS tt USING (_trace_uuid, track_id)
      WHERE s.dur > 0
    )
    SELECT _trace_uuid::VARCHAR, utid, sl.name, st.state, sc.cpu, SUM(dur)
    FROM pf_interval_intersection(
      [
        'thread_slices',
        '(SELECT * FROM thread_state WHERE dur > 0)',
        '(SELECT * FROM sched WHERE dur > 0)'
      ],
      ['sl', 'st', 'sc'],
      per := ['_trace_uuid', 'utid'],
      full_row := true
    )
    GROUP BY _trace_uuid, utid, sl.name, st.state, sc.cpu
  """).fetchall()
  check('doc intervals 3-way', got_3way, [
      ('t1', 1, 'Choreographer#doFrame', 'Running', 0, 50),
      ('t1', 1, 'Choreographer#doFrame', 'Running', 1, 40),
      ('t1', 1, 'measure', 'Running', 0, 40),
      ('t1', 1, 'binder transaction', 'Running', 0, 10),
      ('t1', 1, 'DebugLog', 'Running', 1, 30),
      ('t1', 1, 'LockContention', 'Running', 1, 10),
      ('t2', 2, 'Choreographer#doFrame', 'Running', 0, 80),
      ('t2', 2, 'binder transaction', 'Running', 0, 15),
  ])

  # 3. Self-intersection of slice per (_trace_uuid, track_id)
  got_self = con.execute("""
    SELECT _trace_uuid::VARCHAR, track_id, a.id, b.id, SUM(dur)
    FROM pf_interval_intersection(
      ['(SELECT * FROM slice WHERE dur > 0)', '(SELECT * FROM slice WHERE dur > 0)'],
      ['a', 'b'],
      per := ['_trace_uuid', 'track_id'],
      full_row := true
    )
    WHERE a.depth < b.depth
    GROUP BY _trace_uuid, track_id, a.id, b.id
  """).fetchall()
  check('doc intervals self-intersect', got_self, [
      ('t1', 10, 0, 1, 40),
      ('t1', 10, 0, 2, 10),
      ('t1', 10, 1, 2, 10),
      ('t1', 10, 0, 3, 30),
      ('t1', 10, 0, 4, 10),
      ('t1', 10, 3, 4, 10),
      ('t2', 20, 0, 1, 30),
      ('t2', 20, 0, 2, 15),
      ('t2', 20, 1, 2, 15),
  ])

  # 4. Flatten & flatten_sum + quantize with per := ['_trace_uuid']
  got_fsum = con.execute("""
    WITH proc_slices AS (
      SELECT s._trace_uuid, pt.upid, s.ts, s.dur, (s.depth + 1) AS weight
      FROM slice AS s JOIN process_track AS pt USING (_trace_uuid, track_id)
      WHERE s.dur > 0
    )
    SELECT _trace_uuid::VARCHAR, upid, SUM(dur * sum_weight)
    FROM pf_interval_flatten_sum('proc_slices', ['weight'], per := ['_trace_uuid', 'upid'])
    GROUP BY _trace_uuid, upid
  """).fetchall()
  check('doc intervals flatten_sum', got_fsum, [
      ('t1', 100, 100 * 1 + 40 * 2 + 10 * 3 + 30 * 2 + 10 * 3),
      ('t2', 200, 80 * 1 + 30 * 2 + 15 * 3),
  ])

  got_quant = con.execute("""
    SELECT _trace_uuid::VARCHAR, cpu, bucket, SUM(dur)
    FROM pf_interval_quantize('sched', 50, 'trace_start', bounds := 'trace_bounds', per := ['_trace_uuid'])
    GROUP BY _trace_uuid, cpu, bucket
  """).fetchall()
  check('doc intervals quantize bounds _trace_uuid', got_quant, [
      ('t1', 0, 100, 50),
      ('t1', 1, 150, 40),
      ('t2', 0, 200, 45),
      ('t2', 0, 250, 5),
  ])

  # --- docs/trees.md examples ---
  # Exclusive self_dur + accumulate_up recovery
  got_self_dur = con.execute("""
    WITH child_dur AS (
      SELECT _trace_uuid, parent_id AS id, SUM(dur) AS children_dur
      FROM slice WHERE parent_id IS NOT NULL GROUP BY _trace_uuid, parent_id
    ),
    slice_self AS (
      SELECT s._trace_uuid, s.id, s.parent_id, s.name, s.dur,
             s.dur - COALESCE(c.children_dur, 0) AS self_dur
      FROM slice AS s LEFT JOIN child_dur AS c USING (_trace_uuid, id)
    )
    SELECT s._trace_uuid::VARCHAR, s.id, s.dur, s.self_dur, a.total
    FROM slice_self AS s
    JOIN pf_tree_accumulate_up('slice_self', 'self_dur', per := ['_trace_uuid']) AS a
      USING (_trace_uuid, id)
    WHERE s.parent_id IS NULL
  """).fetchall()
  check('doc trees self_dur recovery', got_self_dur, [
      ('t1', 0, 100, 30, 100),
      ('t2', 0, 80, 50, 80),
  ])

  # Multi-aggregate UP and DOWN
  got_acc_multi = con.execute("""
    SELECT _trace_uuid::VARCHAR, id, subtree_dur, subtree_count, max_child_dur, slowest_descendant
    FROM pf_tree_accumulate(
      'slice', 'up',
      ['SUM(dur) AS subtree_dur', 'COUNT(*) AS subtree_count', 'MAX(dur) AS max_child_dur', 'ARG_MAX(name, dur) AS slowest_descendant'],
      per := ['_trace_uuid']
    )
    WHERE parent_id IS NULL
  """).fetchall()
  check('doc trees accumulate multi UP', got_acc_multi, [
      ('t1', 0, 190, 5, 100, 'Choreographer#doFrame'),
      ('t2', 0, 125, 3, 80, 'Choreographer#doFrame'),
  ])

  got_acc_down = con.execute("""
    SELECT _trace_uuid::VARCHAR, id, root_slice_name, call_path, path_nodes
    FROM pf_tree_accumulate(
      'slice', 'down',
      ['FIRST(name) AS root_slice_name', 'GROUP_CONCAT(name) AS call_path', 'COUNT(*) AS path_nodes'],
      per := ['_trace_uuid']
    )
    WHERE id = 2
  """).fetchall()
  check('doc trees accumulate DOWN', got_acc_down, [
      ('t1', 2, 'Choreographer#doFrame',
       'Choreographer#doFrame,measure,binder transaction', 3),
      ('t2', 2, 'Choreographer#doFrame',
       'Choreographer#doFrame,Choreographer#doFrame,binder transaction', 3),
  ])

  # keep_if / drop_if / prune / flatten_recursion / key_siblings / merge_siblings
  got_anc = con.execute("""
    SELECT _trace_uuid::VARCHAR, id
    FROM pf_tree_keep_if(
      'slice', 'ancestor',
      '(SELECT _trace_uuid, id FROM slice WHERE name = ''binder transaction'')',
      per := ['_trace_uuid']
    )
  """).fetchall()
  check('doc trees keep_if ancestor', got_anc, [
      ('t1', 0),
      ('t1', 1),
      ('t2', 0),
      ('t2', 1),
  ])

  got_prune = con.execute("""
    SELECT _trace_uuid::VARCHAR, id
    FROM pf_tree_prune(
      'slice',
      '(SELECT _trace_uuid, id FROM slice WHERE name = ''DebugLog'')',
      per := ['_trace_uuid']
    )
  """).fetchall()
  check('doc trees prune', got_prune, [
      ('t1', 0),
      ('t1', 1),
      ('t1', 2),
      ('t2', 0),
      ('t2', 1),
      ('t2', 2),
  ])

  got_flat_rec = con.execute("""
    SELECT _trace_uuid::VARCHAR, id, parent_id
    FROM pf_tree_flatten_recursion('slice', ['name'], per := ['_trace_uuid'])
    WHERE _trace_uuid = 't2'
  """).fetchall()
  check('doc trees flatten_recursion', got_flat_rec, [
      ('t2', 0, None),
      ('t2', 1, None),
      ('t2', 2, 1),
  ])

  # --- docs/graphs.md examples ---
  con.execute("""
    CREATE OR REPLACE TABLE heap_graph_class (
      _trace_uuid _bt_trace_uuid, id BIGINT, name VARCHAR, deobfuscated_name VARCHAR
    );
    INSERT INTO heap_graph_class VALUES
      ('t1', 1, 'a.b', 'com.app.Cache'),
      ('t1', 2, 'a.c', 'com.app.Entry'),
      ('t1', 3, 'a.d', 'byte[]'),
      ('t2', 1, 'a.b', 'com.app.Cache');

    CREATE OR REPLACE TABLE heap_graph_object (
      _trace_uuid _bt_trace_uuid, upid BIGINT, graph_sample_ts BIGINT,
      id BIGINT, type_id BIGINT, self_size BIGINT, reachable BIGINT, root_type VARCHAR
    );
    INSERT INTO heap_graph_object VALUES
      ('t1', 10, 1000, 0, 1, 100, 1, 'ROOT_STICKY_CLASS'),
      ('t1', 10, 1000, 1, 2, 200, 1, NULL),
      ('t1', 10, 1000, 2, 2, 300, 1, NULL),
      ('t1', 10, 1000, 3, 3, 500, 1, NULL),
      ('t2', 20, 2000, 0, 1, 400, 1, 'ROOT_JNI_GLOBAL');

    CREATE OR REPLACE TABLE heap_graph_reference (
      _trace_uuid _bt_trace_uuid, owner_id BIGINT, owned_id BIGINT
    );
    INSERT INTO heap_graph_reference VALUES
      ('t1', 0, 1),
      ('t1', 1, 2),
      ('t1', 2, 3);

    CREATE OR REPLACE VIEW heap_nodes AS
    SELECT _trace_uuid, upid, graph_sample_ts, id, type_id, self_size
    FROM heap_graph_object WHERE reachable != 0;

    CREATE OR REPLACE VIEW heap_edges AS
    SELECT o._trace_uuid, o.upid, o.graph_sample_ts, r.owner_id AS src, r.owned_id AS dst
    FROM heap_graph_reference AS r
    JOIN heap_graph_object AS o ON r._trace_uuid = o._trace_uuid AND r.owner_id = o.id
    WHERE r.owned_id IS NOT NULL;

    CREATE OR REPLACE VIEW heap_roots AS
    SELECT _trace_uuid, upid, graph_sample_ts, id
    FROM heap_graph_object WHERE root_type IS NOT NULL;
  """)

  con.execute("""
    CREATE OR REPLACE TABLE dom_tree AS
    SELECT * FROM pf_graph_dominator_tree(
      'heap_nodes', 'heap_edges', 'heap_roots',
      per := ['_trace_uuid', 'upid', 'graph_sample_ts']
    )
  """)
  got_ret = con.execute("""
    SELECT d._trace_uuid::VARCHAR, d.id, r.total AS retained_size
    FROM dom_tree AS d
    JOIN pf_tree_accumulate_up('dom_tree', 'self_size', per := ['_trace_uuid', 'upid', 'graph_sample_ts']) AS r
      USING (_trace_uuid, upid, graph_sample_ts, id)
  """).fetchall()
  check('doc graphs object retained', got_ret, [
      ('t1', 0, 1100),
      ('t1', 1, 1000),
      ('t1', 2, 800),
      ('t1', 3, 500),
      ('t2', 0, 400),
  ])

  got_bfs_paths = con.execute("""
    WITH bfs_objects AS (
      SELECT b._trace_uuid, b.upid, b.graph_sample_ts, b.id, b.parent_id,
             COALESCE(c.deobfuscated_name, c.name) AS class_name, b.self_size
      FROM pf_graph_bfs_tree('heap_nodes', 'heap_edges', 'heap_roots',
                             per := ['_trace_uuid', 'upid', 'graph_sample_ts']) AS b
      JOIN heap_graph_class AS c ON b._trace_uuid = c._trace_uuid AND b.type_id = c.id
    ),
    unrolled AS (
      SELECT * FROM pf_tree_flatten_recursion('bfs_objects', ['class_name'],
                                              per := ['_trace_uuid', 'upid', 'graph_sample_ts'])
    ),
    keyed AS (
      SELECT * FROM pf_tree_key_siblings('unrolled', ['class_name'],
                                         key_col := 'path_id', parent_key_col := 'parent_path_id',
                                         per := ['_trace_uuid', 'upid', 'graph_sample_ts'])
    )
    SELECT _trace_uuid::VARCHAR, class_name, COUNT(*), SUM(self_size)
    FROM keyed
    GROUP BY _trace_uuid, upid, graph_sample_ts, path_id, parent_path_id, class_name
  """).fetchall()
  check('doc graphs bfs + flatten_recursion + key_siblings', got_bfs_paths, [
      ('t1', 'com.app.Cache', 1, 100),
      ('t1', 'com.app.Entry', 2, 500),
      ('t1', 'byte[]', 1, 500),
      ('t2', 'com.app.Cache', 1, 400),
  ])


def test_adversarial_edge_cases(con):
  # 1. Negative dangling parent_id on dense id=0 must fail (regression test for DenseParentFirst)
  con.execute("""
    CREATE OR REPLACE TABLE bad_neg_par (
      _trace_uuid VARCHAR, id BIGINT, parent_id BIGINT, val BIGINT
    );
    INSERT INTO bad_neg_par VALUES ('t1', 0, -5, 10);
  """)
  check_err(
      con, 'dense negative dangling parent_id (accumulate_up)',
      "SELECT * FROM pf_tree_accumulate_up('bad_neg_par', 'val', per := ['_trace_uuid'])",
      'parent')
  check_err(
      con, 'dense negative dangling parent_id (key_siblings)',
      "SELECT * FROM pf_tree_key_siblings('bad_neg_par', ['val'], per := ['_trace_uuid'])",
      'parent')

  # 2. Tree error conditions: NULL id, duplicate id, cycle
  con.execute("""
    CREATE OR REPLACE TABLE bad_null_id (_trace_uuid VARCHAR, id BIGINT, parent_id BIGINT, val BIGINT);
    INSERT INTO bad_null_id VALUES ('t1', NULL, NULL, 10);
    CREATE OR REPLACE TABLE bad_dup_id (_trace_uuid VARCHAR, id BIGINT, parent_id BIGINT, val BIGINT);
    INSERT INTO bad_dup_id VALUES ('t1', 1, NULL, 10), ('t1', 1, NULL, 20);
    CREATE OR REPLACE TABLE bad_cycle (_trace_uuid VARCHAR, id BIGINT, parent_id BIGINT, val BIGINT);
    INSERT INTO bad_cycle VALUES ('t1', 1, 2, 10), ('t1', 2, 1, 20);
  """)
  check_err(
      con, 'tree null id',
      "SELECT * FROM pf_tree_accumulate_up('bad_null_id', 'val', per := ['_trace_uuid'])",
      'no id')
  check_err(
      con, 'tree duplicate id',
      "SELECT * FROM pf_tree_accumulate_up('bad_dup_id', 'val', per := ['_trace_uuid'])",
      'same node')
  check_err(
      con, 'tree cycle',
      "SELECT * FROM pf_tree_accumulate_up('bad_cycle', 'val', per := ['_trace_uuid'])",
      'cycle')

  # 3. Interval negative dur/ts error vs zero-duration point intervals in 2/3/4-way intersection
  con.execute("""
    CREATE OR REPLACE TABLE bad_ivl (_trace_uuid VARCHAR, utid BIGINT, id BIGINT, ts BIGINT, dur BIGINT);
    INSERT INTO bad_ivl VALUES ('t1', 1, 0, 10, -1);
  """)
  check_err(
      con, 'interval negative dur',
      "SELECT * FROM pf_interval_flatten('bad_ivl', ['COUNT(*) AS c'], per := ['_trace_uuid', 'utid'])",
      'below zero')

  con.execute("""
    CREATE OR REPLACE TABLE pt_a (_trace_uuid VARCHAR, utid BIGINT, id BIGINT, ts BIGINT, dur BIGINT);
    CREATE OR REPLACE TABLE pt_b (_trace_uuid VARCHAR, utid BIGINT, id BIGINT, ts BIGINT, dur BIGINT);
    CREATE OR REPLACE TABLE pt_c (_trace_uuid VARCHAR, utid BIGINT, id BIGINT, ts BIGINT, dur BIGINT);
    CREATE OR REPLACE TABLE pt_d (_trace_uuid VARCHAR, utid BIGINT, id BIGINT, ts BIGINT, dur BIGINT);
    INSERT INTO pt_a VALUES ('t1', 1, 10, 50, 0), ('t1', 1, 11, 40, 20);
    INSERT INTO pt_b VALUES ('t1', 1, 20, 50, 0), ('t1', 1, 21, 45, 10);
    INSERT INTO pt_c VALUES ('t1', 1, 30, 50, 10);
    INSERT INTO pt_d VALUES ('t1', 1, 40, 50, 0);
  """)
  got_4way = con.execute("""
    SELECT ts, dur, a.id, b.id, c.id, d.id
    FROM pf_interval_intersection(
      ['pt_a', 'pt_b', 'pt_c', 'pt_d'],
      ['a', 'b', 'c', 'd'],
      per := ['_trace_uuid', 'utid']
    )
  """).fetchall()
  check('4-way point intersection', got_4way, [
      (50, 0, 10, 20, 30, 40),
      (50, 0, 10, 21, 30, 40),
      (50, 0, 11, 20, 30, 40),
      (50, 0, 11, 21, 30, 40),
  ])

  # 4. Graphs: negative node IDs, self-loops, disconnected components, multi-root dominator
  con.execute("""
    CREATE OR REPLACE TABLE adv_nodes (_trace_uuid VARCHAR, id BIGINT);
    CREATE OR REPLACE TABLE adv_edges (_trace_uuid VARCHAR, src BIGINT, dst BIGINT);
    CREATE OR REPLACE TABLE adv_roots (_trace_uuid VARCHAR, id BIGINT);
    INSERT INTO adv_nodes VALUES ('t1', -10), ('t1', -5), ('t1', 0), ('t1', 5), ('t1', 99);
    INSERT INTO adv_edges VALUES
      ('t1', -10, -10),  -- self-loop on root
      ('t1', -10, 0),
      ('t1', -5, 0),     -- node 0 reachable from two roots (-10 and -5), so dominated by super-root!
      ('t1', 0, 5),
      ('t1', 5, 0),      -- cycle 0 <-> 5
      ('t1', 99, 99);    -- disconnected self-loop
    INSERT INTO adv_roots VALUES ('t1', -10), ('t1', -5);
  """)
  got_adv_dom = con.execute("""
    SELECT id, parent_id, depth
    FROM pf_graph_dominator_tree('adv_nodes', 'adv_edges', 'adv_roots', per := ['_trace_uuid'])
  """).fetchall()
  check(
      'adversarial graph dominator (negative ids, self-loop, multi-root, cycle, disconnected)',
      got_adv_dom, [
          (-10, None, 0),
          (-5, None, 0),
          (0, None, 0),
          (5, 0, 1),
      ])


def test_pipe_syntax(con):
  # Re-run test_doc_examples setup so slice, sched, thread_state, heap_objects,
  # heap_refs, gc_roots, callstacks, inline_frames, etc. are populated.
  test_doc_examples(con)

  # 1. Relational stages: WHERE, EXTEND, SET, DROP, RENAME, SELECT * EXCEPT,
  #    ORDER BY, LIMIT, windows, duration literals.
  got_rel = con.execute("""
    FROM pf_pipe('
      FROM slice AS s
      |> WHERE s.dur >= 10ns AND _trace_uuid = ''t1''
      |> EXTEND s.ts + s.dur AS end_ts, 1ms AS one_ms
      |> SET end_ts = end_ts + 5
      |> RENAME category AS cat
      |> DROP track_id, one_ms
      |> SELECT * EXCEPT (parent_id, utid, depth)
      |> EXTEND ROW_NUMBER() OVER (ORDER BY ts) AS rn
      |> WHERE rn <= 3
      |> ORDER BY id ASC
      |> LIMIT 2 OFFSET 0
    ')
  """).fetchall()
  check('pipe relational stages', got_rel, [
      ('t1', 0, 100, 100, 'Choreographer#doFrame', 'ui', 205, 1),
      ('t1', 1, 110, 40, 'measure', 'ui', 155, 2),
  ])

  # 2. AGGREGATE ... GROUP BY (with key alias)
  got_agg = con.execute("""
    FROM pf_pipe('
      FROM slice
      |> WHERE _trace_uuid = ''t1''
      |> AGGREGATE COUNT(*) AS n, SUM(dur) AS total_dur
         GROUP BY category, depth + 1 AS level
      |> ORDER BY category, level
    ')
  """).fetchall()
  check('pipe aggregate group by with key alias', got_agg, [
      ('binder', 3, 1, 10),
      ('lock', 3, 1, 10),
      ('log', 2, 1, 30),
      ('ui', 1, 1, 100),
      ('ui', 2, 1, 40),
  ])

  # 3. FORK AS + INTERVAL MERGE CONSECUTIVE + INTERVAL JOIN
  got_fork = con.execute("""
    FROM pf_pipe('
      FROM thread_state
      |> WHERE _trace_uuid = ''t1''
      |> FORK AS raw_states
      |> INTERVAL MERGE CONSECUTIVE BY state PER (_trace_uuid, utid)
         AGGREGATE COUNT(*) AS fragment_count
      |> INTERVAL JOIN raw_states AS raw WITHIN BOUNDS PER (_trace_uuid, utid)
      |> SELECT ts, dur, state, fragment_count, raw.id AS raw_id
      |> ORDER BY ts, raw_id
    ')
  """).fetchall()
  check('pipe FORK AS + INTERVAL MERGE CONSECUTIVE + INTERVAL JOIN', got_fork, [
      (100, 50, 'Running', 2, 0),
      (100, 50, 'Running', 2, 1),
      (150, 10, 'S', 1, 2),
      (160, 40, 'Running', 1, 3),
  ])

  # 4. INTERVAL INTERSECTION OF & INTERVAL UNION OF
  got_ii = con.execute("""
    FROM pf_pipe('
      WITH thread_slices AS (
        FROM slice AS s
        |> JOIN thread_track AS tt USING (_trace_uuid, track_id)
        |> WHERE s.dur > 0
      )
      INTERVAL INTERSECTION OF (thread_slices AS sl, sched AS sc)
        PER (_trace_uuid, utid)
      |> AGGREGATE SUM(dur) AS on_cpu_dur, COUNT(*) AS segments
         GROUP BY _trace_uuid, sl.name
      |> ORDER BY _trace_uuid, on_cpu_dur DESC, name
    ')
  """).fetchall()
  check('pipe INTERVAL INTERSECTION OF', got_ii, [
      ('t1', 'Choreographer#doFrame', 90, 2),
      ('t1', 'measure', 40, 1),
      ('t1', 'DebugLog', 30, 1),
      ('t1', 'binder transaction', 10, 1),
      ('t1', 'LockContention', 10, 1),
      ('t2', 'Choreographer#doFrame', 80, 2),
      ('t2', 'binder transaction', 15, 1),
  ])

  # Test operand alias `AS ts` (colliding with interval column `ts`) so both
  # standalone `ts` (timestamp) and `ts.id` / `ts.name` (struct fields) resolve.
  got_ii_ts_alias = con.execute("""
    FROM pf_pipe('
      WITH thread_slices AS (
        FROM slice AS s
        |> JOIN thread_track AS tt USING (_trace_uuid, track_id)
        |> WHERE s.dur > 0
      )
      INTERVAL INTERSECTION OF (
        (FROM thread_slices |> WHERE dur >= 0) AS ts,
        (FROM sched |> WHERE dur >= 0) AS sc
      ) PER (_trace_uuid, utid)
      |> WHERE _trace_uuid = ''t1'' AND ts.depth = 0
      |> SELECT _trace_uuid, utid, ts, dur, ts.id AS slice_id, ts.name AS slice_name, sc.cpu AS cpu
      |> ORDER BY ts, slice_id
    ')
  """).fetchall()
  check('pipe INTERVAL INTERSECTION OF with AS ts alias collision',
        got_ii_ts_alias, [
            ('t1', 1, 100, 50, 0, 'Choreographer#doFrame', 0),
            ('t1', 1, 160, 40, 0, 'Choreographer#doFrame', 1),
        ])

  got_iu = con.execute("""
    FROM pf_pipe('
      INTERVAL UNION OF (
        (FROM sched |> WHERE _trace_uuid = ''t2'') AS sc,
        (FROM slice |> WHERE _trace_uuid = ''t2'' AND depth = 0) AS sl
      ) PER (_trace_uuid, utid)
      |> SELECT ts, dur, sc.ts IS NOT NULL AS has_sc, sl.ts IS NOT NULL AS has_sl
      |> ORDER BY ts
    ')
  """).fetchall()
  check('pipe INTERVAL UNION OF', got_iu, [
      (200, 5, False, True),
      (205, 50, True, True),
      (255, 25, False, True),
  ])

  # 5. INTERVALS FROM EVENTS with full payload preservation + window LAG
  con.execute("""
    CREATE OR REPLACE TABLE cpu_freq_events (
      _trace_uuid VARCHAR, cpu BIGINT, ts BIGINT, freq BIGINT
    );
    INSERT INTO cpu_freq_events VALUES
      ('t1', 0, 100, 1000),
      ('t1', 0, 150, 2000),
      ('t1', 0, 220, 1500);
  """)
  got_ev = con.execute("""
    FROM pf_pipe('
      INTERVALS FROM EVENTS cpu_freq_events PER (_trace_uuid, cpu)
        CLOSING LAST AT (300)
      |> EXTEND freq - LAG(freq) OVER (PARTITION BY _trace_uuid, cpu ORDER BY ts) AS delta
      |> SELECT ts, dur, cpu, freq, delta
      |> ORDER BY ts
    ')
  """).fetchall()
  check('pipe INTERVALS FROM EVENTS with payload', got_ev, [
      (100, 50, 0, 1000, None),
      (150, 70, 0, 2000, 1000),
      (220, 80, 0, 1500, -500),
  ])

  # 6. INTERVAL SPLIT, SUBTRACT, FILL GAPS WITH, FLATTEN (with PER ROLLUP over VARCHAR),
  #    SET BOUNDS, QUANTIZE
  got_split = con.execute("""
    FROM pf_pipe('
      FROM slice AS s
      |> WHERE _trace_uuid = ''t1'' AND depth = 0
      |> INTERVAL SPLIT thread_state AS st PER (_trace_uuid, utid)
      |> AGGREGATE SUM(dur) AS dur GROUP BY s.id, st.state
      |> ORDER BY state
    ')
  """).fetchall()
  check('pipe INTERVAL SPLIT', got_split, [
      (0, 'Running', 90),
      (0, 'S', 10),
  ])

  got_sub = con.execute("""
    FROM pf_pipe('
      FROM slice
      |> WHERE _trace_uuid = ''t1'' AND depth = 0
      |> INTERVAL SUBTRACT (FROM thread_state |> WHERE state = ''Running'')
         PER (_trace_uuid, utid)
      |> SELECT id, ts, dur
    ')
  """).fetchall()
  check('pipe INTERVAL SUBTRACT', got_sub, [(0, 150, 10)])

  got_fill = con.execute("""
    FROM pf_pipe('
      FROM sched
      |> WHERE _trace_uuid = ''t1''
      |> INTERVAL FILL GAPS WITH (
           FROM trace_bounds
           |> WHERE _trace_uuid = ''t1''
           |> SELECT _trace_uuid, start_ts AS ts, end_ts - start_ts AS dur, TRUE AS idle
         ) PER (_trace_uuid, utid)
      |> SELECT ts, dur, coalesce(idle, FALSE) AS is_idle
      |> ORDER BY ts
    ')
  """).fetchall()
  check('pipe INTERVAL FILL GAPS WITH', got_fill, [
      (100, 50, False),
      (150, 10, True),
      (160, 40, False),
      (200, 300, True),
  ])

  con.execute("""
    CREATE OR REPLACE TABLE binder_txns (
      ts BIGINT, dur BIGINT, server_process VARCHAR, interface VARCHAR
    );
    INSERT INTO binder_txns VALUES
      (10, 20, 'system_server', 'IActivityManager'),
      (15, 10, 'system_server', 'IPackageManager');
  """)
  got_fl_rollup = con.execute("""
    FROM pf_pipe('
      FROM binder_txns
      |> INTERVAL FLATTEN PER ROLLUP(server_process, interface)
         AGGREGATE COUNT(*) AS concurrency
      |> SELECT ts, dur, server_process, interface, grouping_server_process, grouping_interface, concurrency
      |> ORDER BY grouping_server_process, grouping_interface, ts, interface NULLS LAST
    ')
  """).fetchall()
  check('pipe INTERVAL FLATTEN PER ROLLUP over VARCHAR columns', got_fl_rollup,
        [
            (10, 20, 'system_server', 'IActivityManager', 0, 0, 1),
            (15, 10, 'system_server', 'IPackageManager', 0, 0, 1),
            (10, 5, 'system_server', None, 0, 1, 1),
            (15, 10, 'system_server', None, 0, 1, 2),
            (25, 5, 'system_server', None, 0, 1, 1),
            (10, 5, None, None, 1, 1, 1),
            (15, 10, None, None, 1, 1, 2),
            (25, 5, None, None, 1, 1, 1),
        ])

  got_bounds_quant = con.execute("""
    FROM pf_pipe('
      FROM sched
      |> WHERE _trace_uuid = ''t1''
      |> INTERVAL SET BOUNDS BEGIN AT ts FOR dur / 2 PER (_trace_uuid)
      |> INTERVAL QUANTIZE 25ns ALIGNED TO ZERO PER (_trace_uuid)
      |> SELECT ts, dur, bucket
      |> ORDER BY ts
    ')
  """).fetchall()
  check('pipe INTERVAL SET BOUNDS + QUANTIZE', got_bounds_quant, [
      (100, 25, 100),
      (160, 15, 150),
      (175, 5, 175),
  ])

  # 7. LEFT INTERVAL FIND with anchor `END OF f` + KEEP IF / DROP IF
  con.execute("""
    CREATE OR REPLACE TABLE inp (_trace_uuid VARCHAR, id BIGINT, ts BIGINT, dur BIGINT);
    CREATE OR REPLACE TABLE frm (_trace_uuid VARCHAR, id BIGINT, ts BIGINT, dur BIGINT);
    CREATE OR REPLACE TABLE prs (_trace_uuid VARCHAR, id BIGINT, ts BIGINT, dur BIGINT);
    INSERT INTO inp VALUES ('t1', 1, 10, 5), ('t1', 2, 100, 5);
    INSERT INTO frm VALUES ('t1', 10, 20, 10);
    INSERT INTO prs VALUES ('t1', 100, 35, 5);
  """)
  got_find = con.execute("""
    FROM pf_pipe('
      FROM inp AS i
      |> LEFT INTERVAL FIND frm AS f STARTING AFTER END WITHIN 20ns PER (_trace_uuid)
      |> LEFT INTERVAL FIND prs AS p STARTING AFTER END OF f PER (_trace_uuid)
      |> EXTEND p.ts - i.ts AS latency
      |> SELECT id, f.id AS fid, p.id AS pid, latency
      |> ORDER BY id
    ')
  """).fetchall()
  check('pipe LEFT INTERVAL FIND chained with END OF f', got_find, [
      (1, 10, 100, 25),
      (2, None, None, None),
  ])

  got_keep = con.execute("""
    FROM pf_pipe('
      FROM slice
      |> WHERE _trace_uuid = ''t1''
      |> INTERVAL KEEP IF OVERLAPPING BOUNDS (FROM slice |> WHERE name = ''LockContention'')
         PER (_trace_uuid, utid)
      |> SELECT id, name
      |> ORDER BY id
    ')
  """).fetchall()
  check('pipe INTERVAL KEEP IF', got_keep, [
      (0, 'Choreographer#doFrame'),
      (3, 'DebugLog'),
      (4, 'LockContention'),
  ])

  # 8. Tree stages: KEEP IF, DROP IF, PRUNE, CONTRACT, ABSORB, REPARENT,
  #    FLATTEN RECURSION, KEY SIBLINGS, MERGE SIBLINGS, MERGE INTO PARENT,
  #    INVERT, ACCUMULATE, EXPAND
  got_tree_chain = con.execute("""
    FROM pf_pipe('
      FROM slice
      |> TREE KEEP IF DESCENDANT OF (FROM slice |> WHERE name = ''Choreographer#doFrame'')
         PER (_trace_uuid)
      |> SELECT _trace_uuid::VARCHAR AS tr, id, name
      |> ORDER BY tr, id
    ')
  """).fetchall()
  check('pipe TREE KEEP IF DESCENDANT OF', got_tree_chain, [
      ('t1', 1, 'measure'),
      ('t1', 2, 'binder transaction'),
      ('t1', 3, 'DebugLog'),
      ('t1', 4, 'LockContention'),
      ('t2', 1, 'Choreographer#doFrame'),
      ('t2', 2, 'binder transaction'),
  ])

  con.execute("""
    CREATE OR REPLACE TABLE callstacks (
      _trace_uuid VARCHAR, id BIGINT, parent_id BIGINT, callsite_id BIGINT, name VARCHAR, self_dur BIGINT
    );
    INSERT INTO callstacks VALUES
      ('t1', 0, NULL, 100, 'main', 10),
      ('t1', 1, 0,    101, 'draw', 20),
      ('t1', 2, 1,    102, 'draw', 15),
      ('t1', 3, 2,    103, 'flush', 30),
      ('t1', 4, 0,    104, 'draw', 20),
      ('t2', 0, NULL, 200, 'main', 5),
      ('t2', 1, 0,    201, 'draw', 40);

    CREATE OR REPLACE TABLE inline_frames (
      _trace_uuid VARCHAR, callsite_id BIGINT, inline_depth BIGINT, inlined_name VARCHAR
    );
    INSERT INTO inline_frames VALUES
      ('t1', 101, 1, 'inlined_helper_1'),
      ('t1', 101, 2, 'inlined_leaf_2');

    CREATE OR REPLACE TABLE heap_objects (
      _trace_uuid VARCHAR, id BIGINT, class_name VARCHAR, self_size BIGINT
    );
    INSERT INTO heap_objects VALUES
      ('t1', 1, 'com.app.Activity', 100),
      ('t1', 2, 'byte[]', 200),
      ('t1', 3, 'java.lang.String', 300),
      ('t1', 4, 'android.graphics.Bitmap', 1000),
      ('t1', 5, 'java.util.HashMap', 0),
      ('t2', 1, 'com.app.Service', 50),
      ('t2', 2, 'com.app.Cache', 400);

    CREATE OR REPLACE TABLE heap_refs (
      _trace_uuid VARCHAR, src BIGINT, dst BIGINT
    );
    INSERT INTO heap_refs VALUES
      ('t1', 1, 2),
      ('t1', 1, 3),
      ('t1', 2, 4),
      ('t1', 3, 4),
      ('t1', 1, 5),
      ('t2', 1, 2);

    CREATE OR REPLACE TABLE gc_roots (
      _trace_uuid VARCHAR, id BIGINT
    );
    INSERT INTO gc_roots VALUES
      ('t1', 1),
      ('t2', 1);
  """)

  got_flame = con.execute("""
    FROM pf_pipe('
      FROM callstacks
      |> TREE FLATTEN RECURSION BY name PER (_trace_uuid)
      |> TREE MERGE SIBLINGS BY name PER (_trace_uuid)
         AGGREGATE SUM(self_dur) AS self_dur, COUNT(*) AS call_count
      |> TREE ACCUMULATE UP SUM(self_dur) AS cumulative_dur PER (_trace_uuid)
      |> SELECT _trace_uuid::VARCHAR AS tr, id, parent_id, name, self_dur, cumulative_dur, call_count
      |> ORDER BY tr, id
    ')
  """).fetchall()
  check('pipe TREE FLATTEN RECURSION + MERGE SIBLINGS + ACCUMULATE UP',
        got_flame, [
            ('t1', 0, None, 'main', 10, 95, 1),
            ('t1', 1, 0, 'draw', 55, 85, 3),
            ('t1', 2, 1, 'flush', 30, 30, 1),
            ('t2', 0, None, 'main', 5, 45, 1),
            ('t2', 1, 0, 'draw', 40, 40, 1),
        ])

  got_expand = con.execute("""
    FROM pf_pipe('
      FROM callstacks
      |> WHERE _trace_uuid = ''t1'' AND id <= 1
      |> TREE EXPAND DOWN inline_frames BY callsite_id
         ORDERED BY inline_depth
         CHARGE self_dur TO LEAF
         PER (_trace_uuid)
      |> SELECT id, parent_id, coalesce(inlined_name, name) AS frame_name, self_dur
      |> ORDER BY id
    ')
  """).fetchall()
  check('pipe TREE EXPAND DOWN ... CHARGE TO LEAF', got_expand, [
      (0, None, 'main', 10),
      (1, 0, 'draw', None),
      (2, 1, 'inlined_helper_1', None),
      (3, 2, 'inlined_leaf_2', 20),
  ])

  # 9. Graph sources + Tree stages, materialized.
  compiled = con.execute("""
    SELECT pf_compile_pipe('
      GRAPH DOMINATOR TREE NODES heap_objects EDGES heap_refs FROM gc_roots PER (_trace_uuid)
      |> TREE ACCUMULATE UP SUM(self_size) AS retained_size, COUNT(*) AS dominated_objs PER (_trace_uuid)
    ')
  """).fetchone()[0]
  con.execute('CREATE OR REPLACE TABLE test_heap_dom AS ' + compiled)
  got_dom = con.execute("""
    FROM pf_pipe('
      FROM test_heap_dom
      |> SELECT _trace_uuid::VARCHAR AS tr, id, parent_id, class_name, self_size, retained_size, dominated_objs
      |> ORDER BY tr, retained_size DESC, id
    ')
  """).fetchall()
  check('pipe GRAPH DOMINATOR TREE + TREE ACCUMULATE', got_dom, [
      ('t1', 1, None, 'com.app.Activity', 100, 1600, 5),
      ('t1', 4, 1, 'android.graphics.Bitmap', 1000, 1000, 1),
      ('t1', 2, 1, 'byte[]', 200, 200, 1),
      ('t1', 3, 1, 'java.lang.String', 300, 300, 1),
      ('t1', 5, 1, 'java.util.HashMap', 0, 0, 1),
      ('t2', 1, None, 'com.app.Service', 50, 450, 2),
      ('t2', 2, 1, 'com.app.Cache', 400, 400, 1),
  ])

  got_bfs_key = con.execute("""
    FROM pf_pipe('
      GRAPH BFS TREE NODES heap_objects EDGES heap_refs FROM gc_roots PER (_trace_uuid)
      |> TREE FLATTEN RECURSION BY class_name PER (_trace_uuid)
      |> TREE KEY SIBLINGS BY class_name AS (node_id, parent_node_id) PER (_trace_uuid)
      |> AGGREGATE SUM(self_size) AS self_size, COUNT(*) AS self_count
         GROUP BY _trace_uuid, node_id, parent_node_id, class_name
      |> ORDER BY _trace_uuid, self_size DESC
    ')
  """).fetchall()
  check('pipe GRAPH BFS TREE + FLATTEN RECURSION + KEY SIBLINGS + AGGREGATE',
        got_bfs_key, [
            ('t1', 0, None, 'com.app.Activity', 100, 1),
            ('t1', 1, 0, 'byte[]', 200, 1),
            ('t1', 2, 0, 'java.lang.String', 300, 1),
            ('t1', 3, 0, 'java.util.HashMap', 0, 1),
            ('t1', 4, 1, 'android.graphics.Bitmap', 1000, 1),
            ('t2', 0, None, 'com.app.Service', 50, 1),
            ('t2', 1, 0, 'com.app.Cache', 400, 1),
        ])


if __name__ == '__main__':
  main()
