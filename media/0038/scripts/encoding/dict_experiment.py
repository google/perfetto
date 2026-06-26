#!/usr/bin/env python3
"""tracing-v2 Task 04 / E16: does a DICTIONARY actually help?

Two different "dictionaries" are easy to conflate — this measures both:

  1. PER-TRACE in-blob dict  (already measured: ~3-5% after zstd).
     A string dictionary built per trace, shipped inside the blob. zstd's own
     LZ window largely subsumes it, so it adds little.

  2. DYNAMIC / on-the-fly dict  (this script — the lever that matters).
     A dictionary that GROWS as events stream in: the compression window (or an
     intern table) kept alive across flushes, scenario (5). Its value shows up
     only in the REGIME PRODUCTION ACTUALLY USES: block compression, where each
     flushed block is zstd'd from a COLD start. A single giant zstd call (Phase A)
     hides this because one window already sees the whole trace. (A *static*
     shipped dict, scenario (3), is impractical — stale across kernels/apps — and
     is kept only as a floor.)

So we model production faithfully: cut the event stream into independent
per-flush BATCHES, columnar-encode each as a self-contained block, and compress
each block on its own. Then compare:

  (1) single-blob  : whole trace, one zstd window         (Phase A baseline)
  (2) block,nodict : per-block zstd, cold each time        (production today)
  (3) block,static : per-block zstd -D dict, dict trained on ANOTHER trace
                     (a shipped static dict — held-out; impractical, a floor)
  (4) block,self   : per-block zstd -D dict, dict trained on THIS trace's batches
                     (oracle upper bound for a per-session-adapted dict)
  (5) block,concat : all blocks through ONE zstd stream    (rolling window =
                     the dictionary grows on the fly — the realistic lever)

(2)-(1) is the cold-block penalty; (2)->(5) is the on-the-fly-dict win; (2)->(3)
is the static-dict floor; (4) bounds a perfectly-matched session dict. Round-trip
is gated per block (same encode() as columnar_generic).

USAGE:
  python3 dict_experiment.py unlock.dec --label unlock [--train-input boot.dec]
          [--batch 8192] [--level 12] [--maxdict 112640] [--json]
INPUT MUST BE DECOMPRESSED (traceconv decompress_packets).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import columnar_generic as cg  # noqa: E402  (reuse collect/encode/decode)

MB = 1024 * 1024
_T0 = time.monotonic()


def log(msg):
  print(f"[+{int(time.monotonic() - _T0)}s] {msg}", file=sys.stderr, flush=True)


def zc(data, level, dict_path=None):
  """zstd-compress one blob, return compressed length (single call, cold)."""
  argv = ['zstd', f'-{level}', '-c']
  if dict_path:
    argv += ['-D', dict_path]
  return len(subprocess.run(argv, input=data, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL).stdout)


def make_batches(events, batch):
  """Self-contained columnar block per `batch` events (in-blob per-batch dict).
  Uses cg.encode()'s defaults (delta ts + global dict) so it works regardless of
  the columnar_generic.py version on PATH."""
  return [cg.encode(events[i:i + batch]) for i in range(0, len(events), batch)]


def train_dict(blobs, out_path, maxdict):
  """Train a zstd dictionary from a list of block blobs. Returns out_path or None."""
  d = out_path + '.samples'
  if os.path.isdir(d):
    shutil.rmtree(d)
  os.makedirs(d)
  for i, b in enumerate(blobs):
    with open(os.path.join(d, f's{i:05d}'), 'wb') as f:
      f.write(b)
  r = subprocess.run(['zstd', '--train'] + [os.path.join(d, x) for x in os.listdir(d)]
                     + ['-o', out_path, f'--maxdict={maxdict}'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
  shutil.rmtree(d)
  if r.returncode != 0 or not os.path.exists(out_path):
    log(f"  zstd --train failed: {r.stderr.decode()[:200]}")
    return None
  return out_path


def sum_blocks(blobs, level, dict_path=None):
  return sum(zc(b, level, dict_path) for b in blobs)


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input')
  ap.add_argument('--label', default=None)
  ap.add_argument('--train-input', default=None,
                  help='separate trace to train the STATIC dict on (held-out). '
                       'If absent, uses this trace\'s first-half batches.')
  ap.add_argument('--batch', type=int, default=8192, help='events per flush block')
  ap.add_argument('--level', type=int, default=12)
  ap.add_argument('--maxdict', type=int, default=112640, help='dict size cap (bytes)')
  ap.add_argument('--tmp', default='/tmp/t3dict')
  ap.add_argument('--json', action='store_true')
  args = ap.parse_args()
  label = args.label or os.path.basename(args.input)
  os.makedirs(args.tmp, exist_ok=True)

  log(f"[{label}] parsing + de-bundling…")
  events, _row, _today = cg.collect(args.input)
  log(f"{len(events):,} events; building {args.batch}-event blocks…")
  blocks = make_batches(events, args.batch)
  # correctness gate: every block must round-trip value-exact before any size claim
  for bi in (0, len(blocks) // 2, len(blocks) - 1):
    dec = cg.decode(blocks[bi])
    src = events[bi * args.batch: bi * args.batch + args.batch]
    if len(dec) != len(src) or any(
        a['ts'] != b['ts'] or a['type'] != b['type'] or a['fields'] != b['fields']
        for a, b in zip(src, dec)):
      sys.exit(f'ROUND-TRIP FAILED on block {bi} — aborting (no size claim)')
  blob_single = cg.encode(events)
  avg_block_kb = (sum(len(b) for b in blocks) / len(blocks)) / 1024
  log(f"{len(blocks)} blocks, avg {avg_block_kb:.0f} KB raw; "
      f"single blob {len(blob_single)//MB} MB")

  # (1) single-blob baseline (Phase A regime)
  log("compressing (1) single blob…")
  c1 = zc(blob_single, args.level)
  # (2) block, no dict
  log("compressing (2) blocks, no dict (production today)…")
  c2 = sum_blocks(blocks, args.level)

  # (5) blocks concatenated through ONE zstd stream (rolling window, no dict).
  #     Isolates "keep the window across flushes" from "ship an explicit dict".
  log("compressing (5) blocks concatenated, one rolling window…")
  c5 = zc(b''.join(blocks), args.level)

  # (3) static dict trained on a HELD-OUT source
  if args.train_input:
    log(f"training STATIC dict on held-out trace {args.train_input}…")
    tev, _, _ = cg.collect(args.train_input)
    train_blocks = make_batches(tev, args.batch)
    static_src = os.path.basename(args.train_input)
  else:
    half = max(1, len(blocks) // 2)
    train_blocks = blocks[:half]
    static_src = f"{label}:first-half"
  dict_static = train_dict(train_blocks, os.path.join(args.tmp, 'static.dict'),
                           args.maxdict)
  c3 = sum_blocks(blocks, args.level, dict_static) if dict_static else None
  if dict_static:
    log(f"static dict {os.path.getsize(dict_static)//1024} KB (src={static_src})")

  # (4) self/oracle dict trained on THIS trace's own batches
  log("training SELF/oracle dict on this trace's blocks…")
  dict_self = train_dict(blocks, os.path.join(args.tmp, 'self.dict'), args.maxdict)
  c4 = sum_blocks(blocks, args.level, dict_self) if dict_self else None

  def mb(x):
    return round(x / MB, 3) if x is not None else None

  def ratio(x):
    return round(x / c2, 3) if x is not None else None  # vs block-nodict

  res = {
      'label': label, 'events': len(events), 'batch_events': args.batch,
      'n_blocks': len(blocks), 'avg_block_KB': round(avg_block_kb, 1),
      'level': args.level, 'maxdict': args.maxdict, 'static_dict_src': static_src,
      'MB': {
          '1_single_blob': mb(c1),
          '2_block_nodict': mb(c2),
          '3_block_static_dict': mb(c3),
          '4_block_self_dict': mb(c4),
          '5_block_concat_window': mb(c5),
      },
      'vs_block_nodict': {           # <1.0 == dict/window helps
          '1_single_blob': ratio(c1),
          '3_block_static_dict': ratio(c3),
          '4_block_self_dict': ratio(c4),
          '5_block_concat_window': ratio(c5),
      },
      'cold_block_penalty_pct': round((c2 - c1) / c1 * 100, 1),   # (2) vs (1)
      'rolling_window_win_pct': round((c2 - c5) / c2 * 100, 1),   # (2)->(5)
      'static_dict_win_pct':
          round((c2 - c3) / c2 * 100, 1) if c3 is not None else None,  # (2)->(3)
      'self_dict_win_pct':
          round((c2 - c4) / c2 * 100, 1) if c4 is not None else None,  # (2)->(4)
  }

  if not args.json:
    m = res['MB']
    print('=' * 74)
    print(f"E16 dictionary experiment — [{label}]   {len(events):,} events   "
          f"{len(blocks)} x {args.batch}-event blocks (~{avg_block_kb:.0f} KB)")
    print('=' * 74)
    print(f"  (1) single blob, one window     : {m['1_single_blob']:.3f} MB   "
          f"(Phase A regime)")
    print(f"  (2) blocks, NO dict (cold each)  : {m['2_block_nodict']:.3f} MB   "
          f"= production today  -> cold-block penalty +{res['cold_block_penalty_pct']}%")
    print(f"  (5) blocks, ON-THE-FLY window (no dict): {m['5_block_concat_window']:.3f} MB"
          f"   ({res['vs_block_nodict']['5_block_concat_window']}x nodict)  "
          f"keep-window win {res['rolling_window_win_pct']}%")
    if c4 is not None:
      print(f"  (4) blocks, SELF dict (oracle)   : {m['4_block_self_dict']:.3f} MB"
            f"   ({res['vs_block_nodict']['4_block_self_dict']}x nodict)  "
            f"upper-bound win {res['self_dict_win_pct']}%")
    if c3 is not None:
      print(f"  (3) blocks, STATIC dict (floor)  : {m['3_block_static_dict']:.3f} MB"
            f"   ({res['vs_block_nodict']['3_block_static_dict']}x nodict)  "
            f"floor {res['static_dict_win_pct']}%   src={static_src}")
    print('\n----- JSON -----')
  print('===JSON BEGIN==='); print(json.dumps(res, indent=2)); print('===JSON END===')


if __name__ == '__main__':
  main()
