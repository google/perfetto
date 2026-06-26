#!/usr/bin/env python3
"""tracing-v2 Task 04: columnar pre-pass for sched.

Task 3 showed sched-dominated ftrace is BIGGER than today after plain zstd, because
de-bundling discards CompactSched (columnar + delta-timestamps + interned comm).
This measures how much a columnar pass in `traced` recovers — by comparing, for the
SCHED events only, the two encodings, each compressed:

  columnar = the original `compact_sched` blobs from the trace. This is what a
             re-columnarizing `traced` would produce (conservative: a real traced
             could intern over a larger window than per-bundle CompactSched).
  row      = the same sched events de-bundled to full FtraceEvents (Task 1/2).

Both are the sched PAYLOAD only (no TracePacket framing — framing is the Task 3
axis). Answers: "Option 2 (columnar+zstd) vs Option 1 (row+zstd) vs today (gzip on
columnar)" for sched.

INPUT MUST BE DECOMPRESSED (traceconv decompress_packets) first.
USAGE: python3 columnar_bench.py decomp.pftrace --label "config/name" [--quick]
"""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ftrace_expand as fx  # noqa: E402

MB = 1024 * 1024


def compress(data, argv, block=1 * MB):
  total = 0
  for i in range(0, len(data), block):
    p = subprocess.run(argv, input=data[i:i + block], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    total += len(p.stdout)
  return total


def collect_sched(path):
  """Return (columnar_bytes, row_bytes, n_sched_events)."""
  data = open(path, 'rb').read()
  columnar, row = bytearray(), bytearray()
  prev_chain = {}
  st = fx.Stats()
  for fnum, wt, packet in fx.iter_fields(data):
    if fnum != fx.F_TRACE_PACKET or wt != 2:
      continue
    bundle = None
    for pf, pwt, pv in fx.iter_fields(packet):
      if pf == fx.F_TP_FTRACE_EVENTS and pwt == 2:
        bundle = pv
    if bundle is None:
      continue
    cpu, cs = 0, None
    for bf, bwt, bv in fx.iter_fields(bundle):
      if bf == fx.F_FEB_CPU and bwt == 0:
        cpu = bv
      elif bf == fx.F_FEB_COMPACT_SCHED and bwt == 2:
        cs = bv
    if cs is None:
      continue
    columnar += cs
    for fe in fx.reconstruct_compact_sched(cs, cpu, prev_chain, st):
      row += fe
  return bytes(columnar), bytes(row), st.compact_switch + st.compact_waking


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input')
  ap.add_argument('--label', default=None)
  ap.add_argument('--quick', action='store_true', help='cap zstd level at 12')
  ap.add_argument('--json', action='store_true')
  args = ap.parse_args()
  label = args.label or os.path.basename(args.input)

  col, row, n = collect_sched(args.input)
  if n == 0:
    sys.exit('no compact_sched in this trace (sched events not compact-encoded?)')

  levels = [3, 9, 12] if args.quick else [3, 9, 12, 19]
  comps = [('gzip-6', ['gzip', '-6', '-c'])]
  for L in levels:
    comps.append((f'zstd-{L}', ['zstd', f'-{L}', '-c']))

  rows = []
  for cl, argv in comps:
    cb, rb = compress(col, argv), compress(row, argv)
    rows.append({'cfg': cl, 'columnar_b': cb, 'row_b': rb,
                 'row_over_columnar': round(rb / cb, 3) if cb else None})

  def f(cfg, key):
    return next(r[key] for r in rows if r['cfg'] == cfg)

  today = f('gzip-6', 'columnar_b')          # today: gzip on columnar
  opt1 = f('zstd-12', 'row_b')               # Option 1 (Task 3): zstd on row
  opt2 = f('zstd-12', 'columnar_b')          # Option 2 (Task 4): zstd on columnar
  result = {
      'label': label, 'sched_events': n,
      'raw_MB': {'columnar': round(len(col) / MB, 2), 'row': round(len(row) / MB, 2),
                 'row_over_columnar': round(len(row) / len(col), 3)},
      'sched_only': {
          'today_gzip_columnar_MB': round(today / MB, 3),
          'opt1_zstd_row_MB': round(opt1 / MB, 3),
          'opt2_zstd_columnar_MB': round(opt2 / MB, 3),
          'opt1_vs_today': round(opt1 / today, 3),     # Task 3: row+zstd vs today
          'opt2_vs_today': round(opt2 / today, 3),     # Task 4: columnar+zstd vs today
          'opt2_vs_opt1': round(opt2 / opt1, 3),       # how much columnar recovers
      },
      'rows': rows,
  }

  if not args.json:
    print('=' * 72)
    print(f"Task 04 columnar (sched only) — [{label}]   {n:,} sched events")
    print('=' * 72)
    print(f"RAW sched: columnar {len(col)/MB:.1f} MB   row(de-bundled) "
          f"{len(row)/MB:.1f} MB   ({len(row)/len(col):.2f}x bigger)")
    print('-' * 72)
    print(f"{'compressor':<10}{'columnar MB':>13}{'row MB':>10}{'row/columnar':>14}")
    for r in rows:
      print(f"{r['cfg']:<10}{r['columnar_b']/MB:>13.2f}{r['row_b']/MB:>10.2f}"
            f"{r['row_over_columnar']:>13}x")
    print('-' * 72)
    s = result['sched_only']
    print("SCHED bytes, three ways (zstd-12 / gzip):")
    print(f"  today  (gzip on columnar)  : {s['today_gzip_columnar_MB']:.2f} MB")
    print(f"  Option 1 (zstd on row)     : {s['opt1_zstd_row_MB']:.2f} MB  "
          f"({s['opt1_vs_today']}x today)   <- Task 3, plain zstd")
    print(f"  Option 2 (zstd on columnar): {s['opt2_zstd_columnar_MB']:.2f} MB  "
          f"({s['opt2_vs_today']}x today)   <- Task 4, columnar pass")
    print(f"  => columnar recovers: Option 2 is {s['opt2_vs_opt1']}x of Option 1 "
          f"(smaller is better)")
    print('\n----- JSON -----')
  print('===JSON BEGIN===')
  print(json.dumps(result, indent=2))
  print('===JSON END===')


if __name__ == '__main__':
  main()
