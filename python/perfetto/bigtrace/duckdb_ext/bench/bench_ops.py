"""Per-trace operator benchmark: PerfettoSQL Next (PSN) vs DuckDB extension.

usage: bench_ops.py psn  N [PAR=12] [OPS]   PSN pure-operator times -> json
       bench_ops.py duck N [THREADS=1,16,0] [OPS]  DuckDB times -> json
       bench_ops.py report N                 markdown table from the jsons

Data: DATA/<table>/<idx:06d>.parquet written by extract.py with the same PSN
trace processor binary, so ids/utids match. The N-trace set is the first N
traces (in ops/traces.txt order) whose slice table is non-empty.

PSN side (same method as the PSN optimizer's harness): one trace processor
per trace (heap graphs skipped); inputs are first materialized as PERFETTO
TABLEs (untimed), then each op runs as `CREATE OR REPLACE PERFETTO TABLE r AS
<pipeline>` REPS times and the min TP-process CPU time is kept. A trace whose
op inputs are all empty counts 0 s. PSN total = sum over traces.

DuckDB side: one in-memory DuckDB with all N traces' inputs preloaded into
tables (rows clustered by _trace_uuid, an ENUM), each op timed as
`SELECT count(*), max(c1), .. FROM (<op>)` over every output column PSN
materializes (best of REPS), threads as given (0 = all cores). Checksums
(untimed): per trace (count(*), sum(<ck>)) must equal PSN's.
"""
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

B = os.environ.get('BT_BENCH_DIR', os.path.expanduser('~/bt_bench'))
DATA = os.environ.get('OPS_DATA', f'{B}/ops/data')
OUT = os.environ.get('OPS_OUT', f'{B}/ops/results')
PSN_BIN = os.environ.get(
    'PSN_BIN', '/tmp/perfetto_next_opt/out/rel/trace_processor_shell')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT = os.environ.get('PF_EXT', os.path.join(ROOT, 'perfetto.duckdb_extension'))
REPS = int(os.environ.get('REPS', '3'))
TABLES = ('slice', 'thread_state', 'sched')

# Inputs, identical on both engines (DuckDB adds _trace_uuid).
INPUTS = {
    'in_tree': ('slice', 'SELECT id, parent_id, dur FROM {t}'),
    'in_flat':
        ('slice', 'SELECT ts, dur, track_id, depth FROM {t} WHERE dur > 0'),
    'in_sl': ('slice', 'SELECT id, ts, dur, track_id FROM {t} WHERE dur >= 0'),
    'in_d0':
        ('slice',
         'SELECT id, ts, dur, track_id FROM {t} WHERE depth = 0 AND dur >= 0'),
    'in_d1':
        ('slice',
         'SELECT id, ts, dur, track_id FROM {t} WHERE depth = 1 AND dur >= 0'),
    'in_d2':
        ('slice',
         'SELECT id, ts, dur, track_id FROM {t} WHERE depth = 2 AND dur >= 0'),
    'in_ts':
        ('thread_state',
         'SELECT id, ts, dur, utid FROM {t} WHERE dur >= 0 AND utid IS NOT NULL'
        ),
    'in_sched':
        ('sched',
         'SELECT id, ts, dur, utid FROM {t} WHERE dur >= 0 AND utid IS NOT NULL'
        ),
    'in_mark':
        ('slice', 'SELECT id FROM {t} WHERE depth IN (1, 2) AND dur > 5000000'),
}

# op -> (inputs, PSN body, DuckDB body, checksum expr over r).
# The PSN body is a pipeline / query; DuckDB body is a query over the
# preloaded inputs. Checksum: (count(*), sum(<expr>)) per trace.
OPS = {
    # The first four mirror the PSN optimizer's harness (same SELECT lists).
    'tree_up': (
        ['in_tree'],
        'FROM in_tree |> TREE ACCUMULATE UP SUM(dur) AS total |> SELECT total',
        "SELECT _trace_uuid, total FROM pf_tree_accumulate_up('in_tree', 'dur', per := ['_trace_uuid'])",
        'total'),
    'tree_down': (
        ['in_tree'],
        'FROM in_tree |> TREE ACCUMULATE DOWN SUM(dur) AS total |> SELECT total',
        "SELECT _trace_uuid, total FROM pf_tree_accumulate_down('in_tree', 'dur', per := ['_trace_uuid'])",
        'total'),
    'flatten_count': (
        ['in_flat'],
        'FROM in_flat |> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS cnt |> SELECT dur, cnt',
        "SELECT _trace_uuid, dur, cnt FROM pf_interval_flatten('in_flat', ['count(*) AS cnt'], per := ['_trace_uuid', 'track_id'])",
        'dur + cnt'),
    'ii2_slice': (
        ['in_d0', 'in_d1'],
        'INTERVAL INTERSECTION OF (in_d0 AS a, in_d1 AS b) PER track_id |> SELECT dur',
        "SELECT _trace_uuid, dur FROM pf_interval_intersection(['in_d0', 'in_d1'], ['a', 'b'], per := ['_trace_uuid', 'track_id'])",
        'dur'),
    'flatten_aggs': (
        ['in_flat'],
        'FROM in_flat |> INTERVAL FLATTEN PER track_id AGGREGATE COUNT(*) AS cnt, SUM(depth) AS sd, SUM(dur) AS su '
        '|> SELECT ts, dur, track_id, cnt, sd, su',
        "SELECT _trace_uuid, ts, dur, track_id, cnt, sd, su FROM pf_interval_flatten('in_flat', "
        "['count(*) AS cnt', 'sum(depth) AS sd', 'sum(dur) AS su'], per := ['_trace_uuid', 'track_id'])",
        'ts % 1000003 + dur + track_id + cnt + sd + su % 1000003'),
    'ii2_sched_ts': (
        ['in_ts', 'in_sched'],
        'INTERVAL INTERSECTION OF (in_ts AS a, in_sched AS b) PER utid |> SELECT ts, dur, utid',
        "SELECT _trace_uuid, ts, dur, utid FROM pf_interval_intersection(['in_ts', 'in_sched'], ['a', 'b'], per := ['_trace_uuid', 'utid'])",
        'ts % 1000003 + dur + utid'),
    'ii3_slice': (
        ['in_d0', 'in_d1', 'in_d2'],
        'INTERVAL INTERSECTION OF (in_d0 AS a, in_d1 AS b, in_d2 AS c) PER track_id |> SELECT ts, dur, track_id',
        "SELECT _trace_uuid, ts, dur, track_id FROM pf_interval_intersection(['in_d0', 'in_d1', 'in_d2'], ['a', 'b', 'c'], per := ['_trace_uuid', 'track_id'])",
        'ts % 1000003 + dur + track_id'),
    'ii_self': (
        ['in_sl'],
        'INTERVAL INTERSECTION OF (in_sl AS a, in_sl AS b) PER track_id |> SELECT ts, dur, track_id',
        "SELECT _trace_uuid, ts, dur, track_id FROM pf_interval_intersection(['in_sl', 'in_sl'], ['a', 'b'], per := ['_trace_uuid', 'track_id'])",
        'ts % 1000003 + dur + track_id'),
    # No native PSN operator: PSN runs its best SQL formulation.
    'self_dur': (
        ['in_tree'],
        'SELECT t.id, t.dur - coalesce(c.cd, 0) AS self_dur FROM in_tree t LEFT JOIN '
        '(SELECT parent_id AS id, sum(dur) AS cd FROM in_tree WHERE parent_id IS NOT NULL GROUP BY parent_id) c USING (id)',
        'SELECT _trace_uuid, id, self_dur FROM (SELECT t._trace_uuid, t.id, t.dur - coalesce(c.cd, 0) AS self_dur FROM in_tree t LEFT JOIN '
        '(SELECT _trace_uuid, parent_id AS id, sum(dur) AS cd FROM in_tree WHERE parent_id IS NOT NULL GROUP BY ALL) c USING (_trace_uuid, id))',
        'id + self_dur'),
    'keep_desc': (
        ['in_tree', 'in_mark'],
        'SELECT s.id FROM in_tree s WHERE EXISTS (SELECT 1 FROM ancestor_slice(s.id) a WHERE a.id IN (SELECT id FROM in_mark))',
        "SELECT _trace_uuid, id FROM pf_tree_keep_if('in_tree', 'descendant', 'in_mark', per := ['_trace_uuid'], full_row := false)",
        'id'),
    'keep_anc': (
        ['in_tree', 'in_mark'],
        'SELECT id FROM in_tree WHERE id IN (SELECT a.id FROM in_mark m, ancestor_slice(m.id) a)',
        "SELECT _trace_uuid, id FROM pf_tree_keep_if('in_tree', 'ancestor', 'in_mark', per := ['_trace_uuid'], full_row := false)",
        'id'),
}

# Ops skipped at all threads: ii_self's per-thread partition outputs (tens of
# millions of rows) exceed the benchmark's 40 GB memory scope at 48 threads.
NO_ALL_THREADS = {'ii_self'}


def consumer(o):
  """Timed DuckDB consumer of op o: reads every output column PSN
  materializes (the PSN pipeline's final SELECT list) with a cheap max().
  With CONSUMER=ctas it instead materializes the output into a table, like
  PSN's CREATE PERFETTO TABLE."""
  body = OPS[o][2]
  if os.environ.get('CONSUMER') == 'ctas':
    return f'CREATE OR REPLACE TEMP TABLE __pf_out AS {body}'
  cols = [
      c for c in body[len('SELECT '):body.index(' FROM ')].split(', ')
      if c != '_trace_uuid'
  ]
  return f"SELECT count(*), {', '.join(f'max({c})' for c in cols)} FROM ({body})"


def trace_set(n):
  paths = [l.strip() for l in open(f'{B}/ops/traces.txt')]
  import pyarrow.parquet as pq
  out = []
  for idx in range(len(paths)):
    f = f'{DATA}/slice/{idx:06d}.parquet'
    if not all(os.path.exists(f'{DATA}/{t}/{idx:06d}.parquet') for t in TABLES):
      continue
    if pq.ParquetFile(f).metadata.num_rows > 0:
      out.append((idx, paths[idx]))
    if len(out) == n:
      break
  return out


def selected_ops():
  sel = os.environ.get('OPS')
  return [o for o in OPS if not sel or o in sel.split(',')]


# ---------------------------------------------------------------------------
# PSN
# ---------------------------------------------------------------------------
def cpu_ns(pid):
  total = 0
  for t in os.listdir(f'/proc/{pid}/task'):
    try:
      with open(f'/proc/{pid}/task/{t}/schedstat') as f:
        total += int(f.read().split()[0])
    except OSError:
      pass
  return total


def psn_trace(idx, path, ops):
  """Times ops on one trace: TP process CPU time (all its threads, from
  schedstat, so preemption on a loaded machine is excluded) of each
  `CREATE OR REPLACE PERFETTO TABLE r AS <op>` RPC, min of REPS."""
  sys.path.insert(0, f'{B}/tp_arrow/python')
  from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig
  tp = TraceProcessor(
      trace=path,
      config=TraceProcessorConfig(
          bin_path=PSN_BIN,
          extra_flags=['--dev', '--dev-flag', 'skip-heap-graph=true'],
          load_timeout=600))
  pid = tp.subprocess.pid
  res = {}
  try:
    tp.query('PERFETTO PRAGMA pipelines = 1')
    need = sorted({i for o in ops for i in OPS[o][0]})
    rows = {}
    for name in need:
      tbl, q = INPUTS[name]
      tp.query(f'CREATE PERFETTO TABLE {name} AS {q.format(t=tbl)}')
      rows[name] = next(iter(tp.query(f'SELECT count(*) AS n FROM {name}'))).n
    for o in ops:
      ins, body, _, ck = OPS[o]
      r = {
          'empty': all(rows[i] == 0 for i in ins),
          't': None,
          'ck': None,
          'err': None
      }
      if r['empty']:
        r['t'], r['ck'] = 0.0, (0, 0)
        res[o] = r
        continue
      try:
        best = None
        for _ in range(REPS):
          c = cpu_ns(pid)
          tp.query(f'CREATE OR REPLACE PERFETTO TABLE r AS {body}')
          dc = (cpu_ns(pid) - c) / 1e9
          best = dc if best is None else min(best, dc)
        row = next(
            iter(
                tp.query(
                    f'SELECT count(*) AS n, coalesce(sum({ck}), 0) AS s FROM r')
            ))
        r['t'], r['ck'] = best, (int(row.n), int(row.s))
      except Exception as e:  # pylint: disable=broad-except
        r['err'] = str(e)[:300]
      res[o] = r
  finally:
    tp.close()
  return idx, res


def run_psn(n, par, ops):
  traces = trace_set(n)
  t0 = time.time()
  results = {}
  with ThreadPoolExecutor(par) as ex:
    futs = [ex.submit(psn_trace, i, p, ops) for i, p in traces]
    for k, f in enumerate(futs):
      idx, res = f.result()
      results[idx] = res
      if k % 25 == 0:
        print(f'psn {k}/{len(traces)} {time.time() - t0:.0f}s', flush=True)
  summary = {}
  for o in ops:
    ts = [results[i][o]['t'] for i, _ in traces]
    errs = [(i, results[i][o]['err'])
            for i, _ in traces
            if results[i][o]['t'] is None]
    summary[o] = {
        'total_s': sum(t for t in ts if t is not None),
        'n_err': len(errs),
        'errs': errs[:3],
        'rows': sum((results[i][o]['ck'] or (0, 0))[0] for i, _ in traces),
    }
  os.makedirs(OUT, exist_ok=True)
  with open(f'{OUT}/psn_{n}.json', 'w') as f:
    json.dump(
        {
            'summary': summary,
            'per_trace': {
                str(i): {
                    o: results[i][o] for o in ops
                } for i, _ in traces
            }
        }, f)
  print(json.dumps(summary, indent=1))


# ---------------------------------------------------------------------------
# DuckDB
# ---------------------------------------------------------------------------
def duck_load(con, traces, inputs=None):
  """Creates the _bt_trace_uuid ENUM and the given input tables (default
  all), each read straight from the per-trace Parquet files in trace order so
  rows are clustered by _trace_uuid."""
  idxs = [i for i, _ in traces]
  if not con.execute(
      "SELECT 1 FROM duckdb_types() WHERE type_name = '_bt_trace_uuid'"
  ).fetchall():
    labels = ', '.join(f"'{i}'" for i in idxs)
    con.execute(f'CREATE TYPE _bt_trace_uuid AS ENUM ({labels})')
  for name in inputs if inputs is not None else INPUTS:
    tbl, q = INPUTS[name]
    files = [f'{DATA}/{tbl}/{i:06d}.parquet' for i in idxs]
    src = (f"(SELECT trace_idx::VARCHAR::_bt_trace_uuid AS _trace_uuid, * "
           f"FROM read_parquet({files!r}, union_by_name = true))")
    sel = q.format(t=src).replace('SELECT ', 'SELECT _trace_uuid, ', 1)
    con.execute(f'CREATE OR REPLACE TABLE {name} AS {sel}')


def run_duck(n, threads_list, ops):
  import duckdb
  traces = trace_set(n)
  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute("SET memory_limit = '30GB'")
  con.execute(f"LOAD '{EXT}'")
  out = {'threads': {str(th): {} for th in threads_list}, 'load_s': {}}
  loaded = set()
  for k, o in enumerate(ops):
    ins, _, body, ck = OPS[o]
    later = {i for o2 in ops[k + 1:] for i in OPS[o2][0]}
    t0 = time.time()
    duck_load(con, traces, [i for i in ins if i not in loaded])
    out['load_s'][o] = time.time() - t0
    loaded |= set(ins)
    for th in threads_list:
      if th == 0 and o in NO_ALL_THREADS:
        continue
      con.execute(f'SET threads = {th if th else os.cpu_count()}')
      best = best_cpu = None
      err = None
      for _ in range(REPS):
        t, c = time.time(), time.process_time()
        try:
          con.execute(consumer(o)).fetchall()
        except Exception as e:  # pylint: disable=broad-except
          err = str(e)[:300]
          break
        dt, dc = time.time() - t, time.process_time() - c
        best = dt if best is None else min(best, dt)
        best_cpu = dc if best_cpu is None else min(best_cpu, dc)
      if err:
        out['threads'][str(th)][o] = {'err': err}
        print(th, o, 'ERR', err, flush=True)
        continue
      res = {'t': best, 'cpu': best_cpu}
      if th == threads_list[0]:
        rows = con.execute(
            f'SELECT _trace_uuid::VARCHAR, count(*), coalesce(sum({ck}), 0)::HUGEINT '
            f'FROM ({body}) GROUP BY 1').fetchall()
        res['ck'] = {r[0]: (r[1], int(r[2])) for r in rows}
        res['rows'] = sum(r[1] for r in rows)
      out['threads'][str(th)][o] = res
      print(
          th,
          o,
          f'{best:.3f}s wall {best_cpu:.3f}s cpu',
          res.get('rows', ''),
          flush=True)
    for i in loaded - later:
      con.execute(f'DROP TABLE {i}')
    loaded &= later
    # Saved after every op so a killed run keeps what it measured.
    os.makedirs(OUT, exist_ok=True)
    tag = os.environ.get('TAG', 'duck')
    with open(f'{OUT}/{tag}_{n}.json', 'w') as f:
      json.dump(out, f)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
def report(n):
  """Markdown table: PSN CPU time vs DuckDB (1T: CPU time, more threads:
  wall time), with per-trace checksum agreement."""
  psn = json.load(open(f'{OUT}/psn_{n}.json'))
  tag = os.environ.get('TAG', 'duck')
  duck = json.load(open(f'{OUT}/{tag}_{n}.json'))
  ths = list(duck['threads'].keys())
  ck_src = duck['threads'][ths[0]]
  label = lambda t: '1T CPU' if t == '1' else ('all-T wall'
                                               if t == '0' else f'{t}T wall')
  hdr = ('| op | rows | PSN CPU (s) | ' +
         ' | '.join(f'Duck {label(t)} (s) | vs PSN' for t in ths) +
         ' | checksums |')
  print(hdr)
  print('|' + '---|' * (hdr.count('|') - 1))
  for o in psn['summary']:
    ps = psn['summary'][o]
    cells, ok = [], ps['n_err'] == 0
    for t in ths:
      d = duck['threads'][t].get(o)
      if not d and t == '0' and o in NO_ALL_THREADS:
        cells.append('- | -')
        continue
      if not d or 'err' in d:
        cells.append('err | -')
        ok = False
        continue
      v = d['cpu'] if t == '1' else d['t']
      cells.append(f'{v:.3f} | {ps["total_s"] / v:.1f}x')
    for i, per in psn['per_trace'].items():
      want = per[o]['ck'] or [0, 0]
      got = ck_src.get(o, {}).get('ck', {}).get(i, [0, 0])
      ok &= list(want) == list(got)
    print(f"| {o} | {ps['rows']:,} | {ps['total_s']:.3f} | " +
          ' | '.join(cells) + f" | {'OK' if ok else 'MISMATCH'} |")


def main():
  mode, n = sys.argv[1], int(sys.argv[2])
  ops = selected_ops()
  if mode == 'psn':
    run_psn(n, int(sys.argv[3]) if len(sys.argv) > 3 else 12, ops)
  elif mode == 'duck':
    ths = [
        int(x)
        for x in (sys.argv[3] if len(sys.argv) > 3 else '1,16,0').split(',')
    ]
    run_duck(n, ths, ops)
  elif mode == 'report':
    report(n)


if __name__ == '__main__':
  main()
