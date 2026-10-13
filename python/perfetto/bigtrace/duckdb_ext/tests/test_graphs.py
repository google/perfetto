"""Correctness tests for the graph operators (spec §7).

Run: python tests/test_graphs.py   (exits non-zero on failure)

Oracles:
  * an independent brute-force Python reference on synthetic graphs (chains,
    diamonds, cycles, self loops, duplicate edges, unreachable nodes, multiple
    roots, dangling edges, NULLs, sparse / huge / negative ids, many
    partitions);
  * TP's legacy stdlib (graph_reachable_dfs / graph_reachable_bfs /
    android.memory.heap_graph.dominator_tree) on real Java heap graphs, if the
    oracle Parquet files produced by bench/bench_graphs.py --extract exist
    (see docs/graphs.md). Set GRAPH_TP_ORACLE=0 to skip.
"""
import os
import random
import sys
from collections import deque

import duckdb

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT = os.path.join(ROOT, 'perfetto.duckdb_extension')
ORACLE_DIR = os.environ.get('GRAPH_ORACLE_DIR',
                            os.path.expanduser('~/bt_bench/graphs_data'))

FAILS = []


def check(cond, msg):
  if not cond:
    FAILS.append(msg)
    print('FAIL:', msg)


def connect():
  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute(f"LOAD '{EXT}'")
  con.execute("SET memory_limit='20GB'")
  con.execute('SET threads=16')
  return con


# ---------------------------------------------------------------------------
# Brute-force reference (deliberately written differently from the kernel).
# ---------------------------------------------------------------------------
def graph_of(nodes, edges, seeds):
  ns = set(x for x in nodes if x is not None)
  adj = {v: set() for v in ns}
  for s, d in edges:
    if s in ns and d in ns:
      adj[s].add(d)
  sd = sorted(set(x for x in seeds if x in ns))
  return ns, {v: sorted(a) for v, a in adj.items()}, sd


def ref_dfs(nodes, edges, seeds):
  """TP-style: one stack of (node, parent), visit on pop, children pushed in
  reverse so the smallest is popped first, seeds pushed largest first."""
  ns, adj, sd = graph_of(nodes, edges, seeds)
  out = {}
  stack = [(s, None) for s in reversed(sd)]
  while stack:
    v, p = stack.pop()
    if v in out:
      continue
    out[v] = (p, 0 if p is None else out[p][1] + 1)
    for w in reversed(adj[v]):
      stack.append((w, v))
  return out


def ref_bfs(nodes, edges, seeds):
  ns, adj, sd = graph_of(nodes, edges, seeds)
  out = {s: (None, 0) for s in sd}
  q = deque(sd)
  while q:
    v = q.popleft()
    for w in adj[v]:
      if w not in out:
        out[w] = (v, out[v][1] + 1)
        q.append(w)
  return out


def ref_dom(nodes, edges, seeds):
  """Definition-based: d dominates v iff v is unreachable from the root when d
  is removed. Multiple seeds: virtual root VR -> every seed, stripped."""
  ns, adj, sd = graph_of(nodes, edges, seeds)
  if not sd:
    return {}
  VR = object()
  if len(sd) == 1:
    root = sd[0]
    succ = dict(adj)
  else:
    root = VR
    succ = dict(adj)
    succ[VR] = sd

  def reach(removed):
    if root is removed:
      return set()
    seen = {root}
    st = [root]
    while st:
      v = st.pop()
      for w in succ[v]:
        if w is not removed and w != removed and w not in seen:
          seen.add(w)
          st.append(w)
    return seen

  R = reach(None)
  doms = {v: {v} for v in R}
  for d in R:
    if d is root:
      continue
    r2 = reach(d)
    for v in R:
      if v not in r2:
        doms[v].add(d)
  for v in R:
    doms[v].add(root)
  out = {}
  for v in R:
    if v is VR:
      continue
    strict = doms[v] - {v}
    if not strict:
      out[v] = (None, 0)
      continue
    idom = max(strict, key=lambda d: len(doms[d]))
    depth = len(doms[v]) - 1 - (1 if root is VR else 0)
    out[v] = (None if idom is VR else idom, depth)
  return out


REFS = {'dfs': ref_dfs, 'bfs': ref_bfs, 'dominator': ref_dom}


# ---------------------------------------------------------------------------
# Synthetic cases
# ---------------------------------------------------------------------------
def load_case(con, parts):
  """parts: list of (key, nodes, edges, seeds); key = (trace_idx, upid)."""
  nrows, erows, srows = [], [], []
  for (t, u), nodes, edges, seeds in parts:
    for x in nodes:
      nrows.append(
          (t, u, x, None if x is None else f'n{x}', None if x is None else
           (x * 7) % 13))
    for s, d in edges:
      erows.append((t, u, s, d))
    for s in seeds:
      srows.append((t, u, s))
  con.execute('CREATE OR REPLACE TABLE gn(trace_idx INT, upid INT, id BIGINT, '
              'name VARCHAR, self_size BIGINT)')
  con.execute('CREATE OR REPLACE TABLE ge(trace_idx INT, upid INT, src BIGINT, '
              'dst BIGINT)')
  con.execute('CREATE OR REPLACE TABLE gs(trace_idx INT, upid INT, id BIGINT)')
  if nrows:
    con.executemany('INSERT INTO gn VALUES (?, ?, ?, ?, ?)', nrows)
  if erows:
    con.executemany('INSERT INTO ge VALUES (?, ?, ?, ?)', erows)
  if srows:
    con.executemany('INSERT INTO gs VALUES (?, ?, ?)', srows)


def run_case(con, name, parts):
  load_case(con, parts)
  for op, ref in REFS.items():
    got = con.execute(
        f"SELECT trace_idx, upid, id, parent_id, depth, name, self_size "
        f"FROM pf_graph_{op}_tree('gn', 'ge', 'gs', per := ['trace_idx', 'upid'])"
    ).fetchall()
    gd = {}
    for t, u, i, p, d, nm, ss in got:
      key = (t, u, i)
      check(key not in gd, f'{name}/{op}: duplicate output row {key}')
      gd[key] = (p, d)
      check(nm == f'n{i}' and ss == (i * 7) % 13,
            f'{name}/{op}: payload mismatch for {key}: {nm} {ss}')
    exp = {}
    for (t, u), nodes, edges, seeds in parts:
      for v, pd in ref(nodes, edges, seeds).items():
        exp[(t, u, v)] = pd
    if gd != exp:
      diff = sorted(set(gd.items()) ^ set(exp.items()), key=str)[:10]
      check(
          False, f'{name}/{op}: mismatch ({len(gd)} vs {len(exp)} rows), '
          f'diff sample {diff}')


def random_graph(rng, n, m, nseeds, id_mode):
  if id_mode == 'dense':
    ids = list(range(n))
  elif id_mode == 'offset':
    ids, x = [], rng.randint(-10**6, 10**6)
    for _ in range(n):
      x += rng.choice([1, 1, 2, 3])
      ids.append(x)
    rng.shuffle(ids)
  else:  # sparse / huge
    ids = list({rng.randint(-2**62, 2**62) for _ in range(n)})
    n = len(ids)
  edges = []
  for _ in range(m):
    edges.append((rng.choice(ids), rng.choice(ids)))
  # dangling edges, NULLs, dup edges, self loops
  if n:
    edges += [(ids[0], 10**15 + 1), (10**15 + 2, ids[-1])]
    edges += edges[:3]
    v = rng.choice(ids)
    edges.append((v, v))
  edges.append((None, ids[0] if n else 1))
  seeds = rng.sample(ids, min(nseeds, n)) if n else []
  seeds += seeds[:1] + [10**15 + 3]  # duplicate seed, seed not in nodes
  nodes = ids + [None]  # NULL id (ignored)
  return nodes, edges, seeds


def synthetic_tests(con):
  run_case(con, 'empty', [])
  run_case(con, 'no_seeds', [((0, 0), [1, 2, 3], [(1, 2), (2, 3)], [])])
  run_case(con, 'no_edges', [((0, 0), [1, 2, 3], [], [2, 3])])
  run_case(con, 'single', [((0, 0), [5], [], [5])])
  run_case(con, 'chain',
           [((0, 0), list(range(10)), [(i, i + 1) for i in range(9)], [0])])
  run_case(con, 'chain_mid_seed',
           [((0, 0), list(range(10)), [(i, i + 1) for i in range(9)], [4])])
  run_case(con, 'diamond', [((0, 0), [1, 2, 3, 4], [(1, 2), (1, 3), (2, 4),
                                                    (3, 4)], [1])])
  run_case(con, 'cycle', [((0, 0), [1, 2, 3, 4], [(1, 2), (2, 3), (3, 1),
                                                  (3, 4)], [2])])
  run_case(con, 'self_loops_dups', [((0, 0), [1, 2, 3], [(1, 1), (1, 2), (1, 2),
                                                         (2, 2), (2, 3),
                                                         (2, 3)], [1])])
  run_case(con, 'unreachable', [((0, 0), [1, 2, 3, 4, 5], [(1, 2), (3, 4),
                                                           (4, 5)], [1])])
  run_case(con, 'multi_roots', [((0, 0), [1, 2, 3, 4, 5, 6], [(1, 3), (2, 3),
                                                              (3, 4), (2, 5),
                                                              (5, 4),
                                                              (4, 6)], [1, 2])])
  run_case(con, 'seed_reaches_seed', [((0, 0), [1, 2, 3, 4], [(1, 3), (3, 2),
                                                              (2, 4),
                                                              (1, 4)], [1, 2])])
  run_case(con, 'seed_reaches_seed_rev',
           [((0, 0), [1, 2, 3, 4], [(2, 1), (1, 3), (3, 4)], [1, 2])])
  # Classic Lengauer-Tarjan example (Appel / LT paper).
  lt = {
      'R': 'ABC',
      'A': 'D',
      'B': 'ADE',
      'C': 'FG',
      'D': 'L',
      'E': 'H',
      'F': 'I',
      'G': 'IJ',
      'H': 'EK',
      'I': 'K',
      'J': 'I',
      'K': 'IR',
      'L': 'H'
  }
  ix = {c: i for i, c in enumerate('RABCDEFGHIJKL')}
  run_case(con, 'lengauer_tarjan_paper', [((0, 0), list(
      range(13)), [(ix[a], ix[b]) for a in lt for b in lt[a]], [ix['R']])])
  # Dangling/NULL edges and seeds, partitions with different id modes.
  rng = random.Random(42)
  parts = []
  for t in range(6):
    for u in range(5):
      mode = ['dense', 'offset', 'sparse'][(t + u) % 3]
      n = rng.choice([0, 1, 2, 3, 5, 8, 13, 30, 60])
      m = rng.choice([0, n, 2 * n, 4 * n])
      parts.append(
          ((t, u), *random_graph(rng, n, m, rng.choice([0, 1, 1, 2, 3]), mode)))
  run_case(con, 'random_partitions', parts)
  # Many small random graphs, each a separate case for clearer diagnostics.
  for it in range(150):
    n = rng.randint(1, 25)
    m = rng.randint(0, 3 * n)
    parts = [((0, it), *random_graph(rng, n, m, rng.randint(1, 3),
                                     ['dense', 'offset', 'sparse'][it % 3]))]
    run_case(con, f'random_{it}', parts)
    if len(FAILS) > 20:
      return


def kernel_and_surface_tests(con):
  # per := [] (whole input is one graph) and partition column absent.
  con.execute(
      "CREATE OR REPLACE TABLE an AS SELECT * FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) t(id, name)"
  )
  con.execute(
      "CREATE OR REPLACE TABLE ae AS SELECT * FROM (VALUES (1, 2), (2, 3), (1, 3)) t(src, dst)"
  )
  con.execute("CREATE OR REPLACE TABLE as_ AS SELECT 1 AS id")
  for op, exp in [('dfs', {(1, None, 0), (2, 1, 1), (3, 2, 2)}),
                  ('bfs', {(1, None, 0), (2, 1, 1), (3, 1, 1)}),
                  ('dominator', {(1, None, 0), (2, 1, 1), (3, 1, 1)})]:
    got = set(
        con.execute(
            f"SELECT id, parent_id, depth FROM pf_graph_{op}_tree('an', 'ae', 'as_')"
        ).fetchall())
    check(got == exp, f'no-per {op}: {got}')
  cols = [
      d[0] for d in con.execute(
          "SELECT * FROM pf_graph_dfs_tree('an', 'ae', 'as_') LIMIT 0")
      .description
  ]
  check(cols == ['id', 'parent_id', 'depth', 'name'], f'column order {cols}')
  # A payload column named parent_id / depth is replaced by the tree's.
  con.execute(
      "CREATE OR REPLACE TABLE an2 AS SELECT id, name, 99 AS parent_id, 98 AS depth FROM an"
  )
  got = set(
      con.execute(
          "SELECT id, parent_id, depth FROM pf_graph_bfs_tree('an2', 'ae', 'as_')"
      ).fetchall())
  check(got == {(1, None, 0), (2, 1, 1), (3, 1, 1)},
        f'payload parent_id: {got}')
  # Direct kernel use.
  got = con.execute("""
      SELECT pf_graph_tree_agg(2, role, a, b) FROM (VALUES
        (0, 10, NULL), (0, 20, NULL), (0, 30, NULL),
        (1, 10, 20), (1, 20, 30), (1, 10, 30), (2, 10, NULL)) t(role, a, b)
  """).fetchone()[0]
  check(
      sorted(
          (r['id'], r['parent_id'], r['depth']) for r in got) == [(10, None, 0),
                                                                  (20, 10, 1),
                                                                  (30, 10, 1)],
      f'kernel direct {got}')
  # The kernel dedups node rows (the macro's payload join does not: node ids
  # must be unique per partition, like any join key).
  got = con.execute("""
      SELECT pf_graph_tree_agg(0, role, a, b) FROM (VALUES
        (0, 10, NULL), (0, 10, NULL), (0, 20, NULL), (0, 20, NULL),
        (1, 10, 20), (1, 10, 20), (2, 10, NULL), (2, 10, NULL)) t(role, a, b)
  """).fetchone()[0]
  check(
      sorted(
          (r['id'], r['parent_id'], r['depth']) for r in got) == [(10, None, 0),
                                                                  (20, 10, 1)],
      f'kernel dedup {got}')
  # Composes with a TREE ACCUMULATE UP-style rollup (retained size).
  got = dict(
      con.execute("""
      WITH RECURSIVE t AS (SELECT * FROM pf_graph_dominator_tree('an', 'ae', 'as_')),
      anc(id, aid) AS (SELECT id, id FROM t UNION ALL
                       SELECT anc.id, t.parent_id FROM anc JOIN t ON t.id = anc.aid
                       WHERE t.parent_id IS NOT NULL)
      SELECT aid, count(*) FROM anc GROUP BY aid
  """).fetchall())
  check(got == {1: 3, 2: 1, 3: 1}, f'retained count {got}')
  # Composes with TREE FLATTEN RECURSION + TREE KEY SIBLINGS (RFC-0041 §6.5 heap class tree).
  con.execute("""
      CREATE OR REPLACE TABLE hn AS SELECT * FROM (VALUES
        (1, 'List', 'app'), (2, 'Node', 'app'), (3, 'Node', 'app'),
        (4, 'String', 'app'), (5, 'String', 'app')) t(id, cls, heap);
      CREATE OR REPLACE TABLE he_cls AS SELECT * FROM (VALUES
        (1, 2), (2, 3), (2, 4), (3, 5)) t(src, dst);
      CREATE OR REPLACE TABLE hs_cls AS SELECT 1 AS id;
      CREATE OR REPLACE TEMP VIEW h_bfs AS
        SELECT * FROM pf_graph_bfs_tree('hn', 'he_cls', 'hs_cls');
      CREATE OR REPLACE TEMP VIEW h_flat AS
        SELECT * FROM pf_tree_flatten_recursion('h_bfs', ['cls']);
  """)
  got_cls = con.execute("""
      SELECT cls, count(*) AS cnt
      FROM pf_tree_key_siblings('h_flat', ['cls', 'heap'])
      GROUP BY node_id, parent_node_id, cls
      ORDER BY cls
  """).fetchall()
  # Node 3 ('Node' under 'Node' 2) is flattened to a sibling of 2 under 1 ('List'),
  # so both 'Node' objects share a node_id, and their 'String' children (4 and 5) share a node_id under it.
  check(got_cls == [('List', 1), ('Node', 2), ('String', 2)],
        f'heap class tree composition {got_cls}')


# ---------------------------------------------------------------------------
# TP oracle on real heap graphs (bench/bench_graphs.py extract + oracle).
# ---------------------------------------------------------------------------
def tp_oracle_tests(con):
  if os.environ.get('GRAPH_TP_ORACLE', '1') == '0':
    print('skipping TP oracle (GRAPH_TP_ORACLE=0)')
    return
  need = [
      os.path.join(ORACLE_DIR, f'tp_{x}.parquet')
      for x in ['dfs', 'bfs', 'dom']
  ]
  if not all(os.path.exists(p) for p in need) or not os.path.isdir(
      os.path.join(ORACLE_DIR, 'raw', 'object')):
    print(f'skipping TP oracle: data not found in {ORACLE_DIR}')
    return
  sys.path.insert(0, os.path.join(ROOT, 'bench'))
  os.environ['GRAPH_DATA'] = ORACLE_DIR
  import bench_graph_ops  # noqa: E402
  bench_graph_ops.DATA = ORACLE_DIR
  # File-backed scratch DB: 100M+ row tables must be evictable to fit 20 GB.
  scratch = os.path.join(ORACLE_DIR, 'test_oracle.duckdb')
  for suffix in ['', '.wal']:
    if os.path.exists(scratch + suffix):
      os.remove(scratch + suffix)
  con = duckdb.connect(scratch, config={'allow_unsigned_extensions': 'true'})
  con.execute(f"LOAD '{EXT}'")
  con.execute("SET memory_limit='16GB'")
  con.execute('SET threads=32')
  con.execute('SET preserve_insertion_order=false')
  con.execute(f"SET temp_directory='{ORACLE_DIR}/duck_tmp'")
  bench_graph_ops.duck_load(con)
  con.execute(
      f"CREATE OR REPLACE TABLE ho AS SELECT trace_idx, upid, id, self_size FROM hg_object WHERE trace_idx IN (SELECT DISTINCT trace_idx FROM '{need[2]}') ORDER BY trace_idx, id"
  )
  con.execute(
      "CREATE OR REPLACE TABLE he AS SELECT * FROM heap_edge WHERE trace_idx IN (SELECT DISTINCT trace_idx FROM ho)"
  )
  con.execute(
      "CREATE OR REPLACE TABLE hr AS SELECT * FROM heap_root WHERE trace_idx IN (SELECT DISTINCT trace_idx FROM ho)"
  )
  ntr, nobj, nedge = con.execute(
      'SELECT count(DISTINCT trace_idx), count(*), (SELECT count(*) FROM he) FROM ho'
  ).fetchone()
  print(f'TP oracle: {ntr} heap dumps, {nobj} objects, {nedge} edges')
  for op, f, cols in [('dfs', 'dfs', 'trace_idx, id, parent_id'),
                      ('bfs', 'bfs', 'trace_idx, id, parent_id'),
                      ('dominator', 'dom', 'trace_idx, id, parent_id, depth')]:
    path = os.path.join(ORACLE_DIR, f'tp_{f}.parquet')
    con.execute(
        f"CREATE OR REPLACE TABLE got AS SELECT * FROM pf_graph_{op}_tree('ho', 'he', 'hr', per := ['trace_idx', 'upid'])"
    )
    con.execute(f"CREATE OR REPLACE VIEW exp AS SELECT * FROM '{path}'")
    n_got, n_exp = con.execute(
        'SELECT (SELECT count(*) FROM got), (SELECT count(*) FROM exp)'
    ).fetchone()
    diff = con.execute(f"""
        SELECT count(*) FROM (
          (SELECT {cols} FROM got EXCEPT ALL SELECT {cols} FROM exp)
          UNION ALL
          (SELECT {cols} FROM exp EXCEPT ALL SELECT {cols} FROM got))
    """).fetchone()[0]
    chk = con.execute(f'SELECT sum(hash({cols})) FROM got').fetchone()[0]
    check(diff == 0 and n_got == n_exp,
          f'TP oracle {op}: {diff} differing rows (got {n_got}, tp {n_exp})')
    print(
        f'TP oracle {op}: {n_got} rows (tp {n_exp}), {diff} diffs, checksum {chk}'
    )
    if op == 'dominator':
      con.execute('ALTER TABLE got RENAME TO dom')
    else:
      con.execute('DROP TABLE got')
  # Retained sizes: dominator tree |> TREE ACCUMULATE UP SUM(self_size) vs
  # heap_graph_dominator_tree.dominated_size_bytes (needs the trees family).
  has_acc = con.execute(
      "SELECT count(*) FROM duckdb_functions() WHERE function_name = "
      "'pf_tree_accumulate_up'").fetchone()[0]
  if has_acc:
    con.execute(
        "CREATE OR REPLACE TABLE ret AS SELECT trace_idx, id, total FROM pf_tree_accumulate_up('dom', 'self_size', per := ['trace_idx', 'upid'])"
    )
    n, bad, s = con.execute(f"""
        SELECT count(*), count(*) FILTER (WHERE r.total IS DISTINCT FROM e.dominated_size_bytes),
               sum(r.total)
        FROM ret r FULL JOIN (SELECT trace_idx, id, dominated_size_bytes FROM '{need[2]}') e
        USING (trace_idx, id)""").fetchone()
    check(bad == 0, f'retained size: {bad} of {n} objects differ from TP')
    print(f'TP oracle retained size: {n} objects, {bad} diffs, sum {s}')
  else:
    print(
        'skipping retained-size check: pf_tree_accumulate_up not available yet')
  con.close()
  for suffix in ['', '.wal']:
    if os.path.exists(scratch + suffix):
      os.remove(scratch + suffix)


def main():
  con = connect()
  kernel_and_surface_tests(con)
  synthetic_tests(con)
  tp_oracle_tests(con)
  if FAILS:
    print(f'{len(FAILS)} FAILURES')
    sys.exit(1)
  print('all graph tests passed')


if __name__ == '__main__':
  main()
