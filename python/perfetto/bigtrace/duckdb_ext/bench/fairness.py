"""Fairness checks for bench_ops.py.

1. Single-trace comparison: for the median, a large (90th percentile) and
   the largest trace (by slice count) of the first N traces, each op on ONE
   trace in PSN (pre-materialized inputs, CREATE PERFETTO TABLE of the op,
   CPU, min of REPS) vs DuckDB 1T on a table holding only that trace (CPU,
   min of REPS), both with the max() consumer and with CREATE TABLE AS.
2. PSN fixed per-query overhead: the same op over 1-row inputs, per trace.

usage: fairness.py N  -> prints markdown, writes OUT/fairness_N.json
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import bench_ops as b  # noqa: E402
import pyarrow.parquet as pq  # noqa: E402

OPS = [o for o in b.selected_ops()]


def duck_one(trace, ctas):
  os.environ['CONSUMER'] = 'ctas' if ctas else 'max'
  b.trace_set = lambda n: [trace]
  b.OUT = '/tmp/pf_fair'
  os.environ['TAG'] = 'one'
  b.run_duck(1, [1], OPS)
  return {
      o: v['cpu'] for o, v in json.load(open('/tmp/pf_fair/one_1.json'))
      ['threads']['1'].items()
  }


def main():
  n = int(sys.argv[1])
  traces = b.trace_set(n)
  sizes = sorted(
      (pq.ParquetFile(f'{b.DATA}/slice/{i:06d}.parquet').metadata.num_rows, i,
       p) for i, p in traces)
  picks = {
      'median': sizes[len(sizes) // 2],
      'p90': sizes[len(sizes) * 9 // 10],
      'largest': sizes[-1]
  }
  out = {}
  for name, (rows, i, p) in picks.items():
    _, psn = b.psn_trace(i, p, OPS)
    saved = dict(b.INPUTS)
    for k, (t, q) in saved.items():
      b.INPUTS[k] = (t, q + ' LIMIT 1')
    _, psn1 = b.psn_trace(i, p, OPS)
    b.INPUTS.update(saved)
    out[name] = {
        'slices': rows,
        'idx': i,
        'psn': {
            o: psn[o]['t'] for o in OPS
        },
        'psn_1row': {
            o: psn1[o]['t'] for o in OPS
        },
        'duck_max': duck_one((i, p), False),
        'duck_ctas': duck_one((i, p), True)
    }
    print(name, json.dumps(out[name]), flush=True)
  with open(f'{b.OUT}/fairness_{n}.json', 'w') as f:
    json.dump(out, f)
  print(
      '| trace (slices) | op | PSN CPU | PSN 1-row (fixed) | PSN - fixed | '
      'Duck 1T max() | Duck 1T CTAS | raw speedup (CTAS) | adjusted speedup (CTAS) |'
  )
  print('|---|---|---|---|---|---|---|---|---|')
  for name, r in out.items():
    for o in OPS:
      ps, p1 = r['psn'][o], r['psn_1row'][o]
      dm, dc = r['duck_max'].get(o), r['duck_ctas'].get(o)
      if ps is None or p1 is None or not dm or not dc:
        continue
      adj = max(ps - p1, 0.0)
      print(
          f"| {name} ({r['slices']:,}) | {o} | {ps:.4f} | {p1:.4f} | {adj:.4f} | "
          f'{dm:.4f} | {dc:.4f} | {ps / dc:.1f}x | {adj / dc:.1f}x |')


if __name__ == '__main__':
  main()
