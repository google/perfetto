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
"""Correctness tests for the tree operators (docs/trees.md).

Run: python tests/test_trees.py   (exits non-zero on failure)

Oracles:
  * TP main's PerfettoSQL Next `TREE ACCUMULATE UP|DOWN SUM(..)` (the only
    tree operator TP implements natively) on synthetic tables, including the
    error cases (duplicate id, cycle, self parent, dangling parent, NULL id),
    run through `trace_processor_shell query` on an empty trace. Skipped if
    the binary is missing (TREE_TP_ORACLE=0 to skip).
  * TP main on real traces: bench/bench_trees.py --check compares
    pf_tree_accumulate_up/down against TP for every trace (row-level sums).
  * An independent brute-force Python reference for every operator on random
    forests (dense / sparse / shuffled / negative ids, several partitions,
    NULL values and keys, missing parents, deep chains).
"""
import os
import random
import subprocess
import sys
import tempfile

import duckdb

from _ext import EXT, TP

FAILS = []
NCHECK = [0]


def check(cond, msg):
  NCHECK[0] += 1
  if not cond:
    FAILS.append(msg)
    print('FAIL:', msg)


def connect():
  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute(f"LOAD '{EXT}'")
  con.execute("SET memory_limit='8GB'")
  con.execute('SET threads=8')
  return con


# ---------------------------------------------------------------------------
# Random forests. rows: dict(trace_idx, id, parent_id, v, w, name, mk)
# ---------------------------------------------------------------------------
def gen_forest(rng, trace_idx, n, id_mode, dangling=0.0):
  if id_mode == 'dense':
    ids = list(range(n))
  elif id_mode == 'shuffled':
    ids = list(range(n))
    rng.shuffle(ids)
  elif id_mode == 'sparse':
    ids = rng.sample(range(-10**12, 10**12), n)
  else:
    ids = rng.sample(range(n * 5), n)
  rows = []
  for i, x in enumerate(ids):
    if i == 0 or rng.random() < 0.08:
      p = None
    elif rng.random() < 0.3:
      p = ids[i - 1]  # chains
    else:
      p = ids[rng.randrange(i)]
    if p is not None and rng.random() < dangling:
      p = 10**15 + i  # not a row
    rows.append(
        dict(
            trace_idx=trace_idx,
            id=x,
            parent_id=p,
            v=None if rng.random() < 0.1 else rng.randrange(-50, 1000),
            w=None if rng.random() < 0.1 else rng.random() * 100,
            name=None if rng.random() < 0.05 else rng.choice('abcd'),
            mk=rng.random() < 0.15))
  rng.shuffle(rows)
  return rows


def load(con, name, rows):
  con.execute(f'DROP TABLE IF EXISTS {name}')
  con.execute(
      f'CREATE TABLE {name} (trace_idx INTEGER, id BIGINT, parent_id BIGINT, '
      'v BIGINT, w DOUBLE, name VARCHAR)')
  if rows:
    con.executemany(
        f'INSERT INTO {name} VALUES (?, ?, ?, ?, ?, ?)',
        [(r['trace_idx'], r['id'], r['parent_id'], r['v'], r['w'], r['name'])
         for r in rows])
  con.execute(f'DROP TABLE IF EXISTS {name}_mk')
  con.execute(f'CREATE TABLE {name}_mk (trace_idx INTEGER, id BIGINT)')
  mk = [(r['trace_idx'], r['id']) for r in rows if r['mk']]
  if mk:
    con.executemany(f'INSERT INTO {name}_mk VALUES (?, ?)', mk)


class Forest:
  """Reference forest of one partition."""

  def __init__(self, rows, strict):
    self.rows = {r['id']: r for r in rows}
    self.par = {}
    for r in rows:
      p = r['parent_id']
      if p is not None and p not in self.rows:
        if strict:
          raise ValueError('dangling')
        p = None
      self.par[r['id']] = p
    self.kids = {x: [] for x in self.rows}
    for x, p in self.par.items():
      if p is not None:
        self.kids[p].append(x)
    for k in self.kids.values():
      k.sort()

  def anc(self, x):  # strict ancestors, nearest first
    out = []
    p = self.par[x]
    while p is not None:
      out.append(p)
      p = self.par[p]
    return out

  def desc(self, x):  # strict descendants
    out, st = [], list(self.kids[x])
    while st:
      y = st.pop()
      out.append(y)
      st.extend(self.kids[y])
    return out


def parts(rows):
  d = {}
  for r in rows:
    d.setdefault(r['trace_idx'], []).append(r)
  return d


def approx(a, b):
  if a is None or b is None:
    return a is None and b is None
  if isinstance(a, float) or isinstance(b, float):
    return abs(a - b) <= 1e-6 * max(1.0, abs(a), abs(b))
  return a == b


def rows_eq(got, want, msg):
  got = sorted(got, key=repr)
  want = sorted(want, key=repr)
  ok = len(got) == len(want) and all(
      len(g) == len(w) and all(approx(x, y)
                               for x, y in zip(g, w))
      for g, w in zip(got, want))
  if not ok:
    print('  got :', got[:6])
    print('  want:', want[:6])
  check(ok, msg)


# ---------------------------------------------------------------------------
# Reference operators
# ---------------------------------------------------------------------------
def ref_accumulate(rows, direction, fn, col):
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, True)
    for x, r in f.rows.items():
      members = [x] + (f.desc(x) if direction == 'up' else f.anc(x))
      vals = [f.rows[m][col] for m in members]
      nn = [v for v in vals if v is not None]
      if fn == 'sum':
        res = sum(nn) if nn else (0 if col == 'v' else 0.0)
      elif fn == 'min':
        res = min(nn) if nn else None
      elif fn == 'max':
        res = max(nn) if nn else None
      elif fn == 'count*':
        res = len(members)
      elif fn == 'count':
        res = len(nn)
      elif fn == 'avg':
        res = sum(nn) / len(nn) if nn else None
      elif fn in ('arg_max', 'arg_min'):
        cand = [
            (f.rows[m][col], m) for m in members if f.rows[m][col] is not None
        ]
        if not cand:
          res = None
        else:
          best = max(c[0] for c in cand) if fn == 'arg_max' else min(
              c[0] for c in cand)
          res = f.rows[min(m for v, m in cand if v == best)]['name']
      elif fn == 'first':
        res = f.rows[(f.anc(x) or [x])[-1]]['name']
      elif fn == 'last':
        res = r['name']
      elif fn == 'group_concat':
        ms = sorted(members) if direction == 'up' else list(reversed(members))
        names = [f.rows[m]['name'] for m in ms if f.rows[m]['name'] is not None]
        res = ','.join(names) if names else None
      out.append((t, x, res))
  return out


def ref_flags(rows, op, over_rows=None):
  out = []
  srows = over_rows if over_rows is not None else rows
  for t, rs in parts(srows).items():
    f = Forest(rs, False)
    mk = {r['id'] for r in rs if r['mk']}
    work = {r['id'] for r in rows if r['trace_idx'] == t}
    for x in f.rows:
      if x not in work:
        continue
      a, d = f.anc(x), f.desc(x)
      if op == 'keep_desc':
        keep = any(y in mk for y in a)
      elif op == 'keep_anc':
        keep = any(y in mk for y in d)
      elif op == 'drop_desc':
        keep = not any(y in mk for y in a)
      elif op == 'drop_anc':
        keep = not any(y in mk for y in d)
      elif op == 'prune':
        keep = x not in mk and not any(y in mk for y in a)
      if keep:
        out.append((t, x))
  return out


def ref_reparent(rows):
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, False)
    mk = {r['id'] for r in rs if r['mk']}
    for x in f.rows:
      out.append((t, x, next((y for y in f.anc(x) if y in mk), None)))
  return out


def ref_contract(rows):
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, False)
    mk = {r['id'] for r in rs if r['mk']}
    acc = {}
    for x in f.rows:
      owner = x if x not in mk else next(
          (y for y in f.anc(x) if y not in mk), None)
      if owner is not None:
        acc.setdefault(owner, []).append(f.rows[x]['v'])
    for x in f.rows:
      if x in mk:
        continue
      np = next((y for y in f.anc(x) if y not in mk), None)
      vs = [v for v in acc[x] if v is not None]
      out.append((t, x, np, sum(vs) if vs else None, len(acc[x])))
  return out


def ref_absorb(rows):
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, False)
    mk = {r['id'] for r in rs if r['mk']}
    acc = {}
    for x in f.rows:
      tops = [y for y in [x] + f.anc(x) if y in mk]
      owner = tops[-1] if tops else x
      acc.setdefault(owner, []).append(f.rows[x]['v'])
    for x, vals in acc.items():
      vs = [v for v in vals if v is not None]
      out.append(
          (t, x, f.rows[x]['parent_id'], sum(vs) if vs else None, len(vals)))
  return out


def ref_merge_siblings(rows, ordered):
  """Canonical: (t, tuple of key path [with run index if ordered]) -> sum, count."""
  out = {}
  for t, rs in parts(rows).items():
    f = Forest(rs, True)
    # Merged node identity = path of (key, run) from the root.
    ident = {}
    roots = sorted(x for x in f.rows if f.par[x] is None)

    def visit_children(parent_ident, children):
      last_key, run = object(), -1
      for c in children:  # children of the merged parent, in id order
        k = f.rows[c]['name']
        if ordered:
          if k != last_key:
            run += 1
            last_key = k
          ident[c] = parent_ident + ((k, run),)
        else:
          ident[c] = parent_ident + ((k,),)

    # BFS by merged parent: children of all members of a merged node,
    # members in BFS order, each member's children in id order.
    visit_children((), roots)
    level = roots
    while level:
      groups = {}
      for x in level:
        groups.setdefault(ident[x], []).append(x)
      nxt = []
      seen = set()
      for x in level:
        if ident[x] in seen:
          continue
        seen.add(ident[x])
        ch = [c for m in groups[ident[x]] for c in f.kids[m]]
        visit_children(ident[x], ch)
        nxt.extend(ch)
      level = nxt
    for x, idn in ident.items():
      s = out.setdefault((t, idn), [0, 0, False])
      if f.rows[x]['v'] is not None:
        s[0] += f.rows[x]['v']
        s[2] = True
      s[1] += 1
  return [(k[0], k[1], v[0] if v[2] else None, v[1]) for k, v in out.items()]


def ref_merge_into_parent(rows, indirect):
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, True)
    key = {x: f.rows[x]['name'] for x in f.rows}
    owner = {}

    def own(x):
      return x if owner[x] == x else own(owner[x])

    if not indirect:
      for x in f.rows:
        owner[x] = x
      # process parents first
      order = sorted(f.rows, key=lambda x: len(f.anc(x)))
      for x in order:
        p = f.par[x]
        if p is not None and key[p] == key[x]:
          owner[x] = owner[p]
    else:
      for x in f.rows:
        owner[x] = x
      order = sorted(f.rows, key=lambda x: len(f.anc(x)))
      for x in order:
        path = list(reversed(f.anc(x))) + [x]  # root .. x
        occ = next((i for i, y in enumerate(path[:-1]) if key[y] == key[x]),
                   None)
        if occ is not None:
          for y in path[occ + 1:]:
            owner[y] = path[occ]
      for x in order:
        owner[x] = own(x)
    acc = {}
    for x in f.rows:
      acc.setdefault(owner[x], []).append(f.rows[x]['v'])
    for x in f.rows:
      if owner[x] != x:
        continue
      p = f.par[x]
      vs = [v for v in acc[x] if v is not None]
      out.append(
          (t, x, None if p is None else owner[p], sum(vs) if vs else None,
           len(acc[x])))
  return out


def ref_flatten_recursion(rows):
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, False)
    for x in f.rows:
      k = f.rows[x]['name']
      p = next((y for y in f.anc(x) if f.rows[y]['name'] != k), None)
      out.append((t, x, p))
  return out


def ref_key_siblings(rows, ordered):
  """Canonical: (t, id, node key-path tuple, parent key-path tuple or None)."""
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, True)
    ident = {}
    roots = sorted(x for x in f.rows if f.par[x] is None)

    def visit_children(parent_ident, children):
      last_key, run = object(), -1
      for c in children:
        k = f.rows[c]['name']
        if ordered:
          if k != last_key:
            run += 1
            last_key = k
          ident[c] = parent_ident + ((k, run),)
        else:
          ident[c] = parent_ident + ((k,),)

    visit_children((), roots)
    level = roots
    while level:
      groups = {}
      for x in level:
        groups.setdefault(ident[x], []).append(x)
      nxt, seen = [], set()
      for x in level:
        if ident[x] in seen:
          continue
        seen.add(ident[x])
        ch = [c for m in groups[ident[x]] for c in f.kids[m]]
        visit_children(ident[x], ch)
        nxt.extend(ch)
      level = nxt
    for x in f.rows:
      p = f.par[x]
      out.append((t, x, ident[x], None if p is None else ident[p]))
  return out


def ref_invert(rows):
  """Canonical: (t, leaf, tuple(node path leaf->...->root position k))."""
  out = []
  for t, rs in parts(rows).items():
    f = Forest(rs, True)
    for x in f.rows:
      if f.kids[x]:
        continue
      path = [x] + f.anc(x)
      for k in range(len(path)):
        out.append((t, x, tuple(path[:k + 1])))
  return out


# ---------------------------------------------------------------------------
# DuckDB side
# ---------------------------------------------------------------------------
def run_random(con, rng, case, rows):
  load(con, 't', rows)
  per = "per := ['trace_idx']"
  try:
    # ACCUMULATE: every function, both directions.
    for direction in ('up', 'down'):
      fns = [('sum', 'sum(v)', 'v'), ('sum', 'sum(w)', 'w'),
             ('min', 'min(v)', 'v'), ('max', 'max(w)', 'w'),
             ('count*', 'count(*)', 'v'), ('count', 'count(v)', 'v'),
             ('avg', 'avg(v)', 'v'), ('arg_max', 'arg_max(name, v)', 'v'),
             ('arg_min', 'arg_min(name, w)', 'w'),
             ('group_concat', 'group_concat(name)', 'v')]
      if direction == 'down':
        fns += [('first', 'first(name)', 'v'), ('last', 'last(name)', 'v')]
      aggs = [f"{e} AS a{i}" for i, (_, e, _) in enumerate(fns)]
      got = con.execute(
          f"SELECT trace_idx, id, {', '.join(f'a{i}' for i in range(len(fns)))} "
          f"FROM pf_tree_accumulate('t', '{direction}', {aggs!r}, {per})"
      ).fetchall()
      for i, (fn, e, col) in enumerate(fns):
        rows_eq([(g[0], g[1], g[2 + i]) for g in got],
                ref_accumulate(rows, direction, fn, col),
                f'{case}: accumulate {direction} {e}')
    got = con.execute(
        f"SELECT trace_idx, id, total FROM pf_tree_accumulate_up('t', 'v', {per})"
    ).fetchall()
    rows_eq(got, ref_accumulate(rows, 'up', 'sum', 'v'),
            f'{case}: accumulate_up')
    got = con.execute(
        f"SELECT trace_idx, id, total FROM pf_tree_accumulate_down('t', 'v', {per})"
    ).fetchall()
    rows_eq(got, ref_accumulate(rows, 'down', 'sum', 'v'),
            f'{case}: accumulate_down')
    # Filters.
    for op, sql in [
        ('keep_desc', "pf_tree_keep_if('t', 'descendant', 't_mk', {per})"),
        ('keep_anc', "pf_tree_keep_if('t', 'ancestor', 't_mk', {per})"),
        ('drop_desc', "pf_tree_drop_if('t', 'descendant', 't_mk', {per})"),
        ('drop_anc', "pf_tree_drop_if('t', 'ancestor', 't_mk', {per})"),
        ('prune', "pf_tree_prune('t', 't_mk', {per})")
    ]:
      got = con.execute(
          f"SELECT trace_idx, id FROM {sql.format(per=per)}").fetchall()
      rows_eq(got, ref_flags(rows, op), f'{case}: {op}')
    got = con.execute(
        f"SELECT trace_idx, id, parent_id FROM pf_tree_reparent('t', 't_mk', {per})"
    ).fetchall()
    rows_eq(got, ref_reparent(rows), f'{case}: reparent')
    got = con.execute(
        f"SELECT trace_idx, id, parent_id, s, n FROM pf_tree_contract('t', 't_mk', "
        f"['sum(v) AS s', 'count(*) AS n'], {per})").fetchall()
    rows_eq(got, ref_contract(rows), f'{case}: contract')
    got = con.execute(
        f"SELECT trace_idx, id, parent_id, s, n FROM pf_tree_absorb('t', 't_mk', "
        f"['sum(v) AS s', 'count(*) AS n'], {per})").fetchall()
    rows_eq(got, ref_absorb(rows), f'{case}: absorb')
    # Merges (canonicalised by key path).
    for ordered in (False, True):
      got = con.execute(
          f"SELECT trace_idx, id, parent_id, name, s, n FROM pf_tree_merge_siblings('t', ['name'], "
          f"['sum(v) AS s', 'count(*) AS n'], {per}, ordered := {ordered})"
      ).fetchall()
      by = {(g[0], g[1]): g for g in got}
      canon = []
      runs = {}

      # Rebuild key paths; for ORDERED, run index = rank among same-key
      # siblings under the same merged parent in id (= creation) order.
      def path(g):
        out = []
        while g is not None:
          out.append(g)
          g = by.get((g[0], g[2])) if g[2] is not None else None
        return list(reversed(out))

      for g in got:
        p = path(g)
        if ordered:
          idn = []
          for node in p:
            sib = sorted(
                x[1] for x in got if x[0] == node[0] and x[2] == node[2])
            # run index = number of key changes before this node among siblings
            seq = [by[(node[0], s)][3] for s in sib]
            pos = sib.index(node[1])
            run = sum(1 for k in range(1, pos + 1) if seq[k] != seq[k - 1])
            idn.append((node[3], run))
          canon.append((g[0], tuple(idn), g[4], g[5]))
        else:
          canon.append((g[0], tuple((node[3],) for node in p), g[4], g[5]))
      rows_eq(canon, ref_merge_siblings(rows, ordered),
              f'{case}: merge_siblings ordered={ordered}')
    for indirect in (False, True):
      got = con.execute(
          f"SELECT trace_idx, id, parent_id, s, n FROM pf_tree_merge_into_parent('t', ['name'], "
          f"['sum(v) AS s', 'count(*) AS n'], {per}, indirect := {indirect})"
      ).fetchall()
      rows_eq(got, ref_merge_into_parent(rows, indirect),
              f'{case}: merge_into_parent indirect={indirect}')
    got = con.execute(
        f"SELECT trace_idx, id, parent_id FROM pf_tree_flatten_recursion('t', ['name'], {per})"
    ).fetchall()
    rows_eq(got, ref_flatten_recursion(rows), f'{case}: flatten_recursion')
    for ordered in (False, True):
      got = con.execute(
          f"SELECT trace_idx, id, parent_id, name, node_id, parent_node_id "
          f"FROM pf_tree_key_siblings('t', ['name'], {per}, ordered := {ordered})"
      ).fetchall()
      # Map (trace_idx, node_id) -> representative key; verify key_siblings gives
      # the exact same equivalence classes and parent_node_id edges as ref_key_siblings.
      ref_ks = {
          (r[0], r[1]): (r[2], r[3]) for r in ref_key_siblings(rows, ordered)
      }
      nid_to_ident = {}
      ok_ks = len(got) == len(rows)
      for t_idx, nid, pid, nm, k_id, pk_id in got:
        w_ident, w_pident = ref_ks[(t_idx, nid)]
        prev = nid_to_ident.setdefault((t_idx, k_id), w_ident)
        if prev != w_ident:
          ok_ks = False
      # Also check distinct key count per trace matches distinct ref identities, and parent_node_id matches.
      ident_to_nid = {(k[0], v): k[1] for k, v in nid_to_ident.items()}
      if len(ident_to_nid) != len(nid_to_ident):
        ok_ks = False
      for t_idx, nid, pid, nm, k_id, pk_id in got:
        w_ident, w_pident = ref_ks[(t_idx, nid)]
        if (w_pident is None) != (pk_id is None):
          ok_ks = False
        elif w_pident is not None and nid_to_ident.get(
            (t_idx, pk_id)) != w_pident:
          ok_ks = False
      check(ok_ks, f'{case}: key_siblings ordered={ordered}')

    got = con.execute(
        f"SELECT trace_idx, id, parent_id, node_id, leaf_id FROM pf_tree_invert('t', {per})"
    ).fetchall()
    by = {(g[0], g[1]): g for g in got}
    canon = []
    for g in got:
      p, x = [], g
      while x is not None:
        p.append(x[3])
        x = by.get((x[0], x[2])) if x[2] is not None else None
      canon.append((g[0], g[4], tuple(reversed(p))))
    rows_eq(canon, ref_invert(rows), f'{case}: invert')
  except duckdb.Error as e:
    check(False, f'{case}: unexpected error {e}')


def test_random(con):
  rng = random.Random(7)
  for it in range(int(os.environ.get('TREE_TEST_ITERS', 12))):
    mode = ['dense', 'shuffled', 'sparse', 'gaps'][it % 4]
    rows = []
    for t in range(rng.randrange(1, 4)):
      rows += gen_forest(rng, t, rng.randrange(1, 60), mode)
    run_random(con, rng, f'random#{it}({mode})', rows)


def test_over_and_dangling(con):
  rng = random.Random(11)
  full = gen_forest(rng, 0, 80, 'shuffled') + gen_forest(rng, 1, 50, 'sparse')
  load(con, 'fullt', full)
  work = [r for r in full if rng.random() < 0.6]
  load(con, 'work', work)
  con.execute('CREATE OR REPLACE TABLE work_mk AS SELECT * FROM fullt_mk')
  per = "per := ['trace_idx']"
  for op, sql in [
      ('keep_desc',
       "pf_tree_keep_if('work', 'descendant', 'fullt_mk', {per}, over := 'fullt')"
      ),
      ('keep_anc',
       "pf_tree_keep_if('work', 'ancestor', 'fullt_mk', {per}, over := 'fullt')"
      ),
      ('drop_desc',
       "pf_tree_drop_if('work', 'descendant', 'fullt_mk', {per}, over := 'fullt')"
      ), ('prune', "pf_tree_prune('work', 'fullt_mk', {per}, over := 'fullt')")
  ]:
    got = con.execute(
        f"SELECT trace_idx, id FROM {sql.format(per=per)}").fetchall()
    rows_eq(got, ref_flags(work, op, over_rows=full), f'over: {op}')
  # Without OVER, a filtered working set: missing parents become roots.
  for op, sql in [('keep_desc',
                   "pf_tree_keep_if('work', 'descendant', 'work_mk', {per})"),
                  ('prune', "pf_tree_prune('work', 'work_mk', {per})")]:
    got = con.execute(
        f"SELECT trace_idx, id FROM {sql.format(per=per)}").fetchall()
    rows_eq(got, ref_flags(work, op), f'working set (dangling as root): {op}')
  got = con.execute(
      f"SELECT trace_idx, id, parent_id FROM pf_tree_reparent('work', 'work_mk', {per})"
  ).fetchall()
  rows_eq(got, ref_reparent(work), 'working set: reparent')
  # REPARENT OVER: nearest marker ancestor in the full tree.
  got = con.execute(
      f"SELECT trace_idx, id, parent_id FROM pf_tree_reparent('work', 'fullt_mk', {per}, over := 'fullt')"
  ).fetchall()
  want = [
      w for w in ref_reparent(full)
      if (w[0], w[1]) in {(r['trace_idx'], r['id']) for r in work}
  ]
  rows_eq(got, want, 'over: reparent')
  # Dangling parents are an error for ACCUMULATE (as in TP).
  try:
    con.execute(
        f"SELECT count(*) FROM pf_tree_accumulate_up('work', 'v', {per})"
    ).fetchall()
    dangling = any(r['parent_id'] is not None and r['parent_id'] not in
                   {w['id']
                    for w in work
                    if w['trace_idx'] == r['trace_idx']}
                   for r in work)
    check(not dangling,
          'accumulate on a working set with missing parents must error')
  except duckdb.Error as e:
    check('not itself a row' in str(e), f'dangling error message: {e}')


def test_edge_cases(con):
  per = "per := ['trace_idx']"
  load(con, 'e', [])
  for sql in [
      f"pf_tree_accumulate_up('e', 'v', {per})",
      f"pf_tree_keep_if('e', 'descendant', 'e_mk', {per})",
      f"pf_tree_merge_siblings('e', ['name'], ['count(*) AS n'], {per})",
      f"pf_tree_invert('e', {per})", "pf_tree_accumulate_up('e', 'v')"
  ]:
    check(
        con.execute(f'SELECT count(*) FROM {sql}').fetchone()[0] == 0,
        f'empty input: {sql}')
  # Error cases (TP semantics).
  cases = {
      'dup': [(0, 1, None), (0, 1, None)],
      'self': [(0, 1, 1)],
      'cycle': [(0, 1, 2), (0, 2, 1), (0, 3, None)],
      'dangling': [(0, 1, 5)],
      'nullid': [(0, None, None)],
  }
  msgs = {
      'dup': 'more than one row has the same node',
      'self': 'its own parent',
      'cycle': 'cycle',
      'dangling': 'not itself a row',
      'nullid': 'has no id'
  }
  for name, rs in cases.items():
    con.execute(
        'CREATE OR REPLACE TABLE x (trace_idx INTEGER, id BIGINT, parent_id BIGINT, v BIGINT)'
    )
    con.executemany('INSERT INTO x VALUES (?, ?, ?, 1)', rs)
    try:
      con.execute(
          f"SELECT * FROM pf_tree_accumulate_up('x', 'v', {per})").fetchall()
      check(False, f'error case {name}: no error')
    except duckdb.Error as e:
      check(msgs[name] in str(e), f'error case {name}: {e}')
  # Overflow.
  con.execute(
      'CREATE OR REPLACE TABLE x AS SELECT * FROM (VALUES (0, 1, NULL, 9223372036854775807), '
      '(0, 2, 1, 1)) v(trace_idx, id, parent_id, v)')
  try:
    con.execute(
        f"SELECT * FROM pf_tree_accumulate_up('x', 'v', {per})").fetchall()
    check(False, 'overflow: no error')
  except duckdb.Error as e:
    check('overflow' in str(e), f'overflow: {e}')
  # FIRST/LAST only DOWN.
  try:
    con.execute(
        f"SELECT * FROM pf_tree_accumulate('x', 'up', ['first(v) AS f'], {per})"
    ).fetchall()
    check(False, 'FIRST with UP: no error')
  except duckdb.Error as e:
    check('DOWN' in str(e), f'FIRST with UP: {e}')
  # Deep chain (100k) and a wide star.
  con.execute(
      'CREATE OR REPLACE TABLE x AS SELECT 0 AS trace_idx, i AS id, '
      'CASE WHEN i = 0 THEN NULL ELSE i - 1 END AS parent_id, 1 AS v FROM range(100000) r(i)'
  )
  r = con.execute(
      f"SELECT max(total), min(total) FROM pf_tree_accumulate_up('x', 'v', {per})"
  ).fetchone()
  check(r == (100000, 1), f'deep chain up {r}')
  r = con.execute(
      f"SELECT max(total) FROM pf_tree_accumulate_down('x', 'v', {per})"
  ).fetchone()
  check(r == (100000,), f'deep chain down {r}')
  # Generic node table without per (one forest), string-free.
  con.execute(
      'CREATE OR REPLACE TABLE g AS SELECT * FROM (VALUES (10, NULL, 1), (20, 10, 2), (5, 20, 3)) v(id, parent_id, v)'
  )
  r = con.execute(
      "SELECT id, total FROM pf_tree_accumulate_up('g', 'v') ORDER BY id"
  ).fetchall()
  check(r == [(5, 3), (10, 6), (20, 5)], f'no-per accumulate {r}')


def test_expand(con):
  # Nodes: 1 -> 2 -> 3 (names a, b, c); operand chains keyed by name.
  con.execute(
      "CREATE OR REPLACE TABLE n AS SELECT * FROM (VALUES (0, 1, NULL, 'a', 10), "
      "(0, 2, 1, 'b', 20), (0, 3, 2, 'c', 30)) v(trace_idx, id, parent_id, name, dur)"
  )
  con.execute(
      "CREATE OR REPLACE TABLE op AS SELECT * FROM (VALUES ('b', 1, 'x'), ('b', 2, 'y')) "
      "v(name, seq, label)")
  r = con.execute(
      "SELECT id, parent_id, name, dur, label, expanded_from FROM pf_tree_expand('n', 'down', 'op', "
      "['name'], ordered_by := ['seq'], charge := ['dur'], charge_to := 'leaf', "
      "per := ['trace_idx']) ORDER BY id").fetchall()
  want = [(1, None, 'a', 10, None, None), (2, 1, 'b', None, None, None),
          (3, 5, 'c', 30, None, None), (4, 2, 'b', None, 'x', 2),
          (5, 4, 'b', 20, 'y', 2)]
  check(r == want, f'expand down chain + charge: {r}')
  r = con.execute(
      "SELECT id, parent_id, dur, label FROM pf_tree_expand('n', 'up', 'op', ['name'], "
      "ordered_by := ['seq'], charge := ['dur'], charge_to := 'root', per := ['trace_idx']) "
      "ORDER BY id").fetchall()
  want = [(1, None, 10, None), (2, 5, None, None), (3, 2, 30, None),
          (4, 1, 20, 'x'), (5, 4, None, 'y')]
  check(r == want, f'expand up chain + charge: {r}')
  # Fan-out sub-structure with explicit local ids: children stay on the node.
  con.execute(
      "CREATE OR REPLACE TABLE op2 AS SELECT * FROM (VALUES ('b', 1, NULL, 'p'), ('b', 2, 1, 'q'), "
      "('b', 3, 1, 'r')) v(name, id, parent_id, label)")
  r = con.execute(
      "SELECT id, parent_id, label FROM pf_tree_expand('n', 'down', 'op2', ['name'], "
      "per := ['trace_idx']) ORDER BY id").fetchall()
  want = [(1, None, None), (2, 1, None), (3, 2, None), (4, 2, 'p'), (5, 4, 'q'),
          (6, 4, 'r')]
  check(r == want, f'expand down fan-out: {r}')
  for sql, msg in [
      ("pf_tree_expand('n', 'down', 'op2', ['name'], charge := ['dur'], charge_to := 'leaf', per := ['trace_idx'])",
       'chain'),
      ("pf_tree_expand('n', 'down', 'op', ['name'], ordered_by := ['seq'], charge := ['dur'], charge_to := 'root')",
       'TO LEAF'),
      ("pf_tree_expand('n', 'up', 'op', ['name'], ordered_by := ['seq'], charge := ['dur'], charge_to := 'leaf')",
       'TO ROOT'),
      ("pf_tree_expand('n', 'up', 'op2', ['name'], per := ['trace_idx'])",
       'single bottom')
  ]:
    try:
      con.execute(f'SELECT * FROM {sql}').fetchall()
      check(False, f'expand error ({msg}): no error')
    except duckdb.Error as e:
      check(msg in str(e), f'expand error ({msg}): {e}')


# ---------------------------------------------------------------------------
# TP oracle (synthetic tables through TP main's pipe syntax)
# ---------------------------------------------------------------------------
def tp_accumulate(rows, direction):
  """rows: (id, parent_id, v). Returns sorted [(id, total)] or ('error', msg)."""
  vals = ', '.join(
      f"({i}, {'NULL' if p is None else p}, {'NULL' if v is None else v})"
      for i, p, v in rows)
  sql = (
      'PERFETTO PRAGMA pipelines = 1;\n'
      f'CREATE PERFETTO TABLE t AS SELECT column1 AS id, column2 AS parent_id, column3 AS v '
      f'FROM (VALUES {vals});\n'
      f'FROM t |> TREE ACCUMULATE {direction.upper()} SUM(v) AS total |> SELECT id, total;\n'
  )
  with tempfile.NamedTemporaryFile(suffix='.pftrace') as tr:
    p = subprocess.run([TP, 'query', '--quiet', '-f', '-', tr.name],
                       input=sql,
                       capture_output=True,
                       text=True)
  if p.returncode != 0:
    return ('error',
            p.stderr.strip().splitlines()[-1] if p.stderr.strip() else '')
  lines = [l for l in p.stdout.strip().splitlines() if l]
  return sorted(tuple(int(x) for x in l.split(',')) for l in lines[1:])


def test_tp_oracle(con):
  if os.environ.get('TREE_TP_ORACLE', '1') == '0' or not os.path.exists(TP):
    print('skipping TP oracle')
    return
  if tp_accumulate([(1, None, 1)], 'up') != [(1, 1)]:
    print('skipping TP oracle: trace processor has no TREE ACCUMULATE')
    return
  rng = random.Random(3)
  cases = []
  for it in range(8):
    n = rng.randrange(1, 40)
    rows = gen_forest(rng, 0, n, ['dense', 'shuffled', 'gaps', 'gaps'][it % 4])
    cases.append([(r['id'], r['parent_id'], r['v']) for r in rows])
  cases += [
      [(1, None, None)],
      [(1, None, 5), (2, 1, None), (3, 2, 7)],
      [(1, None, 1), (1, None, 2)],  # duplicate id
      [(1, 2, 1), (2, 1, 1)],  # cycle
      [(1, 1, 1)],  # self parent
      [(1, None, 1), (2, 9, 1)],  # dangling parent
  ]
  for k, rows in enumerate(cases):
    con.execute(
        'CREATE OR REPLACE TABLE o (id BIGINT, parent_id BIGINT, v BIGINT)')
    con.executemany('INSERT INTO o VALUES (?, ?, ?)', rows)
    for direction in ('up', 'down'):
      want = tp_accumulate(rows, direction)
      try:
        got = sorted(
            con.execute(
                f"SELECT id, total FROM pf_tree_accumulate_{direction}('o', 'v')"
            ).fetchall())
      except duckdb.Error as e:
        got = ('error', str(e))
      if isinstance(want, tuple) or isinstance(got, tuple):
        check(
            isinstance(want, tuple) and isinstance(got, tuple),
            f'TP oracle case {k} {direction}: TP={str(want)[:120]} duck={str(got)[:120]}'
        )
      else:
        check(got == want, f'TP oracle case {k} {direction}')


def main():
  con = connect()
  test_edge_cases(con)
  test_random(con)
  test_over_and_dangling(con)
  test_expand(con)
  test_tp_oracle(con)
  print(f'{NCHECK[0] - len(FAILS)}/{NCHECK[0]} checks passed')
  sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
  main()
