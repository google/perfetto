#!/usr/bin/env python3
"""tracing-v2 Task 03 — host compression-RATIO sweep (the size dimension).

Companion to cost_bench.py (which measures on-device CPU/latency). This measures
RATIO only — which is architecture-independent, so we do it once on the host across
ALL 12 traces. Covers the dimensions the `-b` latency bench can't express:

  * codec x level            : gzip / lz4 / zstd, whole-stream ratio curve
  * zstd block size          : 512K / 1M / 2M / 4M / whole  (compress each block
                               independently and sum — what traced does per flush)
  * zstd --long (big window) : ratio side of the keep-window knob (RSS is on device)
  * today baseline           : gzip-6 on the BUNDLED stream = what ships today, so
                               every v2 number can be read as "x today".

Inputs are the two de-bundled streams written by debundle_corpus.py:
  <exp>/<cat>_<tN>.today.pftrace  and  <exp>/<cat>_<tN>.v2.pftrace
Reads only; never modifies them. Emits one combined JSON.

USAGE: python3 compress_sweep.py EXP_DIR OUT_JSON [--jobs N]
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import subprocess
import sys
import time

MB = 1024 * 1024
KB = 1024
_T0 = time.monotonic()


def log(m):
  print(f"[+{int(time.monotonic()-_T0)}s] {m}", file=sys.stderr, flush=True)


def argv_for(codec, level, longwin=False):
  if codec == 'gzip':
    return ['gzip', f'-{level}', '-c']
  if codec == 'lz4':
    return ['lz4', f'-{level}', '-c']            # lz4 1-12 (>=3 = HC)
  a = ['zstd', f'-{level}', '-c', '-q']
  if longwin:
    a += ['--long=27']
  return a


def csize(data, argv):
  """Compressed length of one buffer with one compressor call."""
  return len(subprocess.run(argv, input=data, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL).stdout)


def csize_blocked(data, argv, block):
  """Compress in independent `block`-byte chunks and sum (what traced does)."""
  if block is None or block >= len(data):
    return csize(data, argv)
  return sum(csize(data[i:i + block], argv) for i in range(0, len(data), block))


# whole-stream codec x level ratio curve
LEVELS = {'gzip': [1, 6, 9], 'lz4': [1, 3, 6, 9, 12],
          'zstd': [1, 2, 3, 4, 6, 9, 12, 19]}
# zstd block-size lever (independent blocks)
BLOCKS = [('512K', 512 * KB), ('1M', MB), ('2M', 2 * MB), ('4M', 4 * MB),
          ('full', None)]
BLOCK_LEVELS = [3, 12]


def sweep_one(label, today_path, v2_path, pool):
  v2 = open(v2_path, 'rb').read()
  today = open(today_path, 'rb').read()
  n = len(v2)
  futs = {}

  # today baseline: gzip-6 on the bundled stream (what ships today)
  futs['__today_gzip6'] = pool.submit(csize, today, argv_for('gzip', 6))

  # codec x level on the de-bundled (v2) stream, whole-stream
  for codec, levels in LEVELS.items():
    for L in levels:
      futs[f'{codec}-{L}'] = pool.submit(csize, v2, argv_for(codec, L))
  # zstd block-size sweep
  for L in BLOCK_LEVELS:
    for bname, bsz in BLOCKS:
      futs[f'zstd-{L}@{bname}'] = pool.submit(csize_blocked, v2,
                                              argv_for('zstd', L), bsz)
  # zstd --long vs default (window), whole-stream
  for L in [6, 19]:
    futs[f'zstd-{L}-long'] = pool.submit(csize, v2, argv_for('zstd', L, True))

  res = {k: f.result() for k, f in futs.items()}
  today_b = res.pop('__today_gzip6')
  out = {
      'label': label,
      'raw': {'today_bundled_MB': round(len(today) / MB, 2),
              'v2_debundled_MB': round(n / MB, 2),
              'debundle_x': round(n / len(today), 3)},
      'today_gzip6_MB': round(today_b / MB, 3),
      'configs': {},
  }
  for k, b in sorted(res.items()):
    out['configs'][k] = {'MB': round(b / MB, 3),
                         'ratio': round(n / b, 2) if b else None,  # in/out
                         'vs_today': round(b / today_b, 3) if today_b else None}
  log(f"{label}: done ({len(res)} configs)")
  return out


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('exp_dir')
  ap.add_argument('out_json')
  ap.add_argument('--jobs', type=int, default=8)
  args = ap.parse_args()

  todays = sorted(glob.glob(os.path.join(args.exp_dir, '*.today.pftrace')))
  results = []
  with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for tp in todays:
      base = os.path.basename(tp)[:-len('.today.pftrace')]
      v2 = os.path.join(args.exp_dir, base + '.v2.pftrace')
      if not os.path.exists(v2):
        log(f"{base}: no v2 stream, skip"); continue
      label = '/'.join(base.rsplit('_', 1))   # <cat>_<tN> -> <cat>/<tN>
      log(f"{label}: sweeping…")
      results.append(sweep_one(label, tp, v2, pool))
  with open(args.out_json, 'w') as f:
    json.dump(results, f, indent=2)
  log(f"wrote {args.out_json} ({len(results)} traces)")
  print(f"OK {len(results)} traces -> {args.out_json}")


if __name__ == '__main__':
  main()
