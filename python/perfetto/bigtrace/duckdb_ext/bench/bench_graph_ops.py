"""Graph operator benchmark: PerfettoSQL Next / TP stdlib vs DuckDB extension.

usage: bench_graph_ops.py psn  [PAR=3]          TP times -> json
       bench_graph_ops.py duck [THREADS=1,16,0] DuckDB times -> json
       bench_graph_ops.py report

Data: the Java heap dumps listed in GRAPH_DATA/traces.txt, with their raw
heap_graph_object / reference / class tables as Parquet (bench_graphs.py
extract). Both engines use TP's edge set (android.memory.heap_graph
excluded_refs: weak/soft/phantom/finalizer referents dropped, reachable
owners only) and roots (root_type IS NOT NULL); tie order is node id
(TP: edges ordered by (src, dst), DFS starts by id DESC, BFS by id ASC).

PSN side: TP has no GRAPH ... TREE pipe operator; the native C++ table
functions behind graph_reachable_dfs / graph_reachable_bfs /
graph_dominator_tree are timed. Inputs (edges, roots, a virtual super root
for the dominator tree) are materialized untimed; each op runs as
`CREATE OR REPLACE PERFETTO TABLE r AS <op>`, min TP-process CPU time over
REPS. DuckDB side: inputs preloaded in tables; each op timed as
SELECT count(*), max(id), max(parent_id) over pf_graph_*_tree(...,
full_row := false). Checksum per trace: (count(*), sum(parent_id)).
"""
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_ops as b  # noqa: E402

DATA = os.environ.get('GRAPH_DATA',
                      os.path.expanduser('~/bt_bench/graphs_data'))
OUT = b.OUT
REPS = b.REPS
WEAK = ("('KIND_FINALIZER_REFERENCE', 'KIND_PHANTOM_REFERENCE', "
        "'KIND_WEAK_REFERENCE')")

PSN_SETUP = """
INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;
INCLUDE PERFETTO MODULE graphs.search;
INCLUDE PERFETTO MODULE graphs.dominator_tree;
CREATE PERFETTO TABLE _pf_e AS
SELECT ref.owner_id AS source_node_id, ref.owned_id AS dest_node_id
FROM heap_graph_reference AS ref
JOIN heap_graph_object AS o ON ref.owner_id = o.id
WHERE o.reachable AND ref.id NOT IN _excluded_refs AND ref.owned_id IS NOT NULL
ORDER BY 1, 2;
CREATE PERFETTO TABLE _pf_roots_asc AS
SELECT id AS node_id FROM heap_graph_object WHERE root_type IS NOT NULL ORDER BY id;
CREATE PERFETTO TABLE _pf_roots_desc AS
SELECT node_id FROM _pf_roots_asc ORDER BY node_id DESC;
CREATE PERFETTO TABLE _pf_super AS SELECT max(id) + 1 AS id FROM heap_graph_object;
CREATE PERFETTO TABLE _pf_e_dom AS
SELECT * FROM _pf_e
UNION ALL SELECT (SELECT id FROM _pf_super), node_id FROM _pf_roots_asc;
"""

OPS = {
    'dfs': ('SELECT node_id AS id, parent_node_id AS parent_id FROM '
            'graph_reachable_dfs!(_pf_e, _pf_roots_desc)',
            "pf_graph_dfs_tree('heap_object', 'heap_edge', 'heap_root', "
            "per := ['trace_idx', 'upid'], full_row := false)"),
    'bfs': ('SELECT node_id AS id, parent_node_id AS parent_id FROM '
            'graph_reachable_bfs!(_pf_e, _pf_roots_asc)',
            "pf_graph_bfs_tree('heap_object', 'heap_edge', 'heap_root', "
            "per := ['trace_idx', 'upid'], full_row := false)"),
    'dominator': (
        'SELECT node_id AS id, nullif(dominator_node_id, (SELECT id FROM _pf_super)) AS parent_id '
        'FROM graph_dominator_tree!(_pf_e_dom, (SELECT id FROM _pf_super)) '
        'WHERE node_id != (SELECT id FROM _pf_super)',
        "pf_graph_dominator_tree('heap_object', 'heap_edge', 'heap_root', "
        "per := ['trace_idx', 'upid'], full_row := false)"),
}


def traces():
  out = []
  for line in open(os.path.join(DATA, 'traces.txt')):
    idx, path = line.split()
    out.append((int(idx), path))
  return out


def psn_trace(idx, path):
  sys.path.insert(0, f'{b.B}/tp_arrow/python')
  from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig
  tp = TraceProcessor(
      trace=path,
      config=TraceProcessorConfig(
          bin_path=b.PSN_BIN, extra_flags=['--dev'], load_timeout=1800))
  pid = tp.subprocess.pid
  res = {}
  try:
    for stmt in PSN_SETUP.split(';'):
      if stmt.strip():
        tp.query(stmt)
    for o, (q, _) in OPS.items():
      r = {'t': None, 'ck': None, 'err': None}
      try:
        best = None
        for _ in range(REPS):
          c = b.cpu_ns(pid)
          tp.query(f'CREATE OR REPLACE PERFETTO TABLE r AS {q}')
          dc = (b.cpu_ns(pid) - c) / 1e9
          best = dc if best is None else min(best, dc)
        row = next(
            iter(
                tp.query(
                    'SELECT count(*) AS n, coalesce(sum(parent_id), 0) AS s FROM r'
                )))
        r['t'], r['ck'] = best, (int(row.n), int(row.s))
      except Exception as e:  # pylint: disable=broad-except
        r['err'] = str(e)[:300]
      res[o] = r
  finally:
    tp.close()
  return idx, res


def run_psn(par):
  results = {}
  with ThreadPoolExecutor(par) as ex:
    futs = [ex.submit(psn_trace, i, p) for i, p in traces()]
    for f in futs:
      idx, res = f.result()
      results[idx] = res
      print(
          idx, {
              o: (r['t'], r['ck'], r['err']) for o, r in res.items()
          },
          flush=True)
  summary = {
      o: {
          'total_s': sum(r[o]['t'] or 0 for r in results.values()),
          'n_err': sum(1 for r in results.values() if r[o]['t'] is None),
          'rows': sum((r[o]['ck'] or (0, 0))[0] for r in results.values())
      } for o in OPS
  }
  os.makedirs(OUT, exist_ok=True)
  json.dump(
      {
          'summary': summary,
          'per_trace': {
              str(k): v for k, v in results.items()
          }
      }, open(f'{OUT}/graph_psn.json', 'w'))
  print(json.dumps(summary, indent=1))


def duck_load(con):
  raw = os.path.join(DATA, 'raw')
  for t in ['object', 'reference', 'class']:
    con.execute(
        f"CREATE OR REPLACE VIEW hg_{t} AS SELECT * FROM '{raw}/{t}/*.parquet'")
  con.execute(f"""
    CREATE OR REPLACE TABLE heap_excluded AS
    WITH wc AS (SELECT trace_idx, id AS type_id FROM hg_class WHERE kind IN {WEAK}),
    wo AS (SELECT DISTINCT o.trace_idx, o.reference_set_id FROM hg_object o
           JOIN wc USING (trace_idx, type_id) WHERE o.reference_set_id IS NOT NULL),
    rr AS (SELECT trace_idx, id, reference_set_id FROM hg_reference
           WHERE field_name = 'java.lang.ref.Reference.referent')
    SELECT rr.trace_idx, rr.id FROM rr JOIN wo USING (trace_idx, reference_set_id)"""
             )
  con.execute("""CREATE OR REPLACE TABLE heap_object AS
    SELECT trace_idx, upid, id FROM hg_object ORDER BY trace_idx, id""")
  con.execute("""CREATE OR REPLACE TABLE heap_edge AS
    SELECT o.trace_idx, o.upid, ref.owner_id AS src, ref.owned_id AS dst
    FROM hg_reference ref
    JOIN hg_object o ON o.trace_idx = ref.trace_idx AND o.id = ref.owner_id
    WHERE o.reachable != 0 AND ref.owned_id IS NOT NULL
      AND NOT EXISTS (SELECT 1 FROM heap_excluded x
                      WHERE x.trace_idx = ref.trace_idx AND x.id = ref.id)
    ORDER BY 1, 3, 4""")
  con.execute("""CREATE OR REPLACE TABLE heap_root AS
    SELECT trace_idx, upid, id FROM hg_object WHERE root_type IS NOT NULL ORDER BY 1, 3"""
             )


def run_duck(threads_list):
  import duckdb
  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute("SET memory_limit = '30GB'")
  con.execute(f"LOAD '{b.EXT}'")
  t0 = time.time()
  duck_load(con)
  out = {'load_s': time.time() - t0, 'threads': {}}
  for th in threads_list:
    con.execute(f'SET threads = {th if th else os.cpu_count()}')
    res = {}
    for o, (_, fn) in OPS.items():
      best = best_cpu = None
      for _ in range(REPS):
        t, c = time.time(), time.process_time()
        con.execute(
            f'SELECT count(*), max(id), max(parent_id) FROM {fn}').fetchall()
        dt, dc = time.time() - t, time.process_time() - c
        best = dt if best is None else min(best, dt)
        best_cpu = dc if best_cpu is None else min(best_cpu, dc)
      r = {'t': best, 'cpu': best_cpu}
      if th == threads_list[0]:
        rows = con.execute(
            f'SELECT trace_idx, count(*), coalesce(sum(parent_id), 0)::HUGEINT FROM {fn} GROUP BY 1'
        ).fetchall()
        r['ck'] = {str(x[0]): (x[1], int(x[2])) for x in rows}
        r['rows'] = sum(x[1] for x in rows)
      res[o] = r
      print(
          th,
          o,
          f'{best:.3f}s wall {best_cpu:.3f}s cpu',
          r.get('rows', ''),
          flush=True)
    out['threads'][str(th)] = res
  json.dump(out, open(f'{OUT}/graph_duck.json', 'w'))


def report():
  psn = json.load(open(f'{OUT}/graph_psn.json'))
  duck = json.load(open(f'{OUT}/graph_duck.json'))
  ths = list(duck['threads'].keys())
  label = lambda t: '1T CPU' if t == '1' else ('all-T wall'
                                               if t == '0' else f'{t}T wall')
  hdr = ('| op | rows | TP CPU (s) | ' +
         ' | '.join(f'Duck {label(t)} (s) | vs TP' for t in ths) +
         ' | checksums |')
  print(hdr)
  print('|' + '---|' * (hdr.count('|') - 1))
  ck_src = duck['threads'][ths[0]]
  for o in OPS:
    ps = psn['summary'][o]
    cells, ok = [], ps['n_err'] == 0
    for t in ths:
      d = duck['threads'][t][o]
      v = d['cpu'] if t == '1' else d['t']
      cells.append(f'{v:.3f} | {ps["total_s"] / v:.1f}x')
    for i, per in psn['per_trace'].items():
      ok &= list(per[o]['ck'] or [0, 0]) == list(ck_src[o]['ck'].get(i, [0, 0]))
    print(f"| {o} | {ps['rows']:,} | {ps['total_s']:.3f} | " +
          ' | '.join(cells) + f" | {'OK' if ok else 'MISMATCH'} |")


def main():
  mode = sys.argv[1]
  if mode == 'psn':
    run_psn(int(sys.argv[2]) if len(sys.argv) > 2 else 3)
  elif mode == 'duck':
    run_duck([
        int(x)
        for x in (sys.argv[2] if len(sys.argv) > 2 else '1,16,0').split(',')
    ])
  else:
    report()


if __name__ == '__main__':
  main()
