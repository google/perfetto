#!/usr/bin/env python3
"""tracing-v2 Task 04 / E8+E11: int-column codec bake-off.

The generic encoder stores every int column as raw varints. E3 showed that for
TIMESTAMPS an explicit codec (delta) barely beats raw once zstd runs. This asks
the same for ALL the other int columns (pid, cpu, common_flags, and every sched
sub-field: prev_pid/next_pid/prio/state/…): does a dedicated int codec beat
raw-varint AFTER zstd, and on which columns?

Candidates per column (each lossless — round-trip gated before any size):
  raw      raw varints                         (current baseline)
  delta    delta + zigzag + varint             (monotonic / slow-moving)
  for      frame-of-reference bit-pack         (subtract min, fixed width)
  pfor     patched FOR (bit-pack + exceptions) (mostly-small with rare spikes)
  rle      run-length (value,count)            (long constant runs)
  dict     distinct-value table + bit-packed ids (low cardinality, non-contiguous)

Reports, per column: raw vs zstd bytes for each codec, and the winner. Then the
realistic AGGREGATE: all int columns concatenated and zstd'd once (1 MB blocks,
matching Phase A), baseline (all-raw) vs best-codec-per-column vs each single
codec. Tells us how much int codecs add ON TOP of columnar layout + zstd.

INPUT MUST BE DECOMPRESSED. USAGE:
  python3 int_codec_bench.py decomp.pftrace --label cfg [--topn 25] [--json]
"""

import argparse
import bisect
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import columnar_generic as cg  # noqa: E402

MB = 1024 * 1024
_T0 = time.monotonic()


def log(msg):
  print(f"[+{int(time.monotonic() - _T0)}s] {msg}", file=sys.stderr, flush=True)


# --- bit packing ------------------------------------------------------------
def bitpack(vals, width):
  if width == 0:
    return b''
  out = bytearray()
  acc = nbits = 0
  m = (1 << width) - 1
  for v in vals:
    acc |= (v & m) << nbits
    nbits += width
    while nbits >= 8:
      out.append(acc & 0xFF); acc >>= 8; nbits -= 8
  if nbits:
    out.append(acc & 0xFF)
  return bytes(out)


def bitunpack(blob, width, n):
  if width == 0:
    return [0] * n
  out = []
  acc = nbits = p = 0
  m = (1 << width) - 1
  for _ in range(n):
    while nbits < width:
      acc |= blob[p] << nbits; p += 1; nbits += 8
    out.append(acc & m); acc >>= width; nbits -= width
  return out


# --- codecs: (encode(vals)->bytes, decode(blob,n)->vals) --------------------
def enc_raw(vals):
  return cg.pack_uvarints(vals)


def dec_raw(b, n):
  return cg.read_uvarints(b, n)


def _zzg(d):                       # magnitude-general zigzag (any Python int)
  return (d << 1) if d >= 0 else (((-d) << 1) - 1)


def _unzzg(z):
  return (z >> 1) if (z & 1) == 0 else -((z + 1) >> 1)


def enc_delta(vals):
  out = bytearray()
  prev = 0
  for v in vals:
    out += cg.uvarint(_zzg(v - prev)); prev = v
  return bytes(out)


def dec_delta(b, n):
  out = []; p = acc = 0
  for _ in range(n):
    x, p = cg.read_uvarint(b, p); acc += _unzzg(x); out.append(acc)
  return out


def enc_for(vals):
  mn = min(vals); rng = max(vals) - mn
  w = rng.bit_length()
  return cg.uvarint(mn) + bytes([w]) + bitpack([v - mn for v in vals], w)


def dec_for(b, n):
  mn, p = cg.read_uvarint(b, 0)
  w = b[p]; p += 1
  return [mn + x for x in bitunpack(b[p:], w, n)]


def _pfor_pick_w(vals):
  """Pick bit width minimizing est size = n*w/8 + exceptions cost."""
  mn = min(vals); n = len(vals)
  sv = sorted(v - mn for v in vals)        # offsets, ascending
  maxw = sv[-1].bit_length()
  # suffix varint-byte cost of exception VALUES (largest offsets)
  best_w, best_est = maxw, float('inf')
  for w in range(0, maxw + 1):
    cap = (1 << w) - 1
    k = len(sv) - bisect.bisect_right(sv, cap)   # how many exceed cap
    est = n * w / 8.0 + k * 4                     # ~4 B/exception (idx+val)
    if est < best_est:
      best_est, best_w = est, w
  return mn, best_w


def enc_pfor(vals):
  mn, w = _pfor_pick_w(vals)
  cap = (1 << w) - 1
  lows, exc = [], []
  for i, v in enumerate(vals):
    o = v - mn
    if o > cap:
      lows.append(0); exc.append((i, v))
    else:
      lows.append(o)
  body = bytearray(cg.uvarint(mn) + bytes([w]) + cg.uvarint(len(exc)))
  body += bitpack(lows, w)
  prev = 0
  for i, v in exc:
    body += cg.uvarint(i - prev) + cg.uvarint(v); prev = i
  return bytes(body)


def dec_pfor(b, n):
  mn, p = cg.read_uvarint(b, 0)
  w = b[p]; p += 1
  ne, p = cg.read_uvarint(b, p)
  # bit-packed lows occupy ceil(n*w/8) bytes
  nbytes = (n * w + 7) // 8
  lows = bitunpack(b[p:p + nbytes], w, n); p += nbytes
  out = [mn + x for x in lows]
  idx = 0
  for _ in range(ne):
    d, p = cg.read_uvarint(b, p); v, p = cg.read_uvarint(b, p)
    idx += d; out[idx] = v
  return out


def enc_rle(vals):
  out = bytearray(); i = 0; n = len(vals)
  runs = 0; body = bytearray()
  while i < n:
    j = i
    while j < n and vals[j] == vals[i]:
      j += 1
    body += cg.uvarint(vals[i]) + cg.uvarint(j - i); runs += 1; i = j
  return bytes(cg.uvarint(runs) + body)


def dec_rle(b, n):
  runs, p = cg.read_uvarint(b, 0)
  out = []
  for _ in range(runs):
    v, p = cg.read_uvarint(b, p); c, p = cg.read_uvarint(b, p)
    out.extend([v] * c)
  return out


def enc_dict(vals):
  distinct = sorted(set(vals))
  idmap = {v: i for i, v in enumerate(distinct)}
  w = (len(distinct) - 1).bit_length() if len(distinct) > 1 else 0
  hdr = bytearray(cg.uvarint(len(distinct)))
  for v in distinct:
    hdr += cg.uvarint(v)
  hdr += bytes([w])
  return bytes(hdr) + bitpack([idmap[v] for v in vals], w)


def dec_dict(b, n):
  k, p = cg.read_uvarint(b, 0)
  distinct = []
  for _ in range(k):
    v, p = cg.read_uvarint(b, p); distinct.append(v)
  w = b[p]; p += 1
  return [distinct[i] for i in bitunpack(b[p:], w, n)]


CODECS = {'raw': (enc_raw, dec_raw), 'delta': (enc_delta, dec_delta),
          'for': (enc_for, dec_for), 'pfor': (enc_pfor, dec_pfor),
          'rle': (enc_rle, dec_rle), 'dict': (enc_dict, dec_dict)}


# --- gather int columns -----------------------------------------------------
KEYNAME = {'pid': 'pid', 'cpu': 'cpu', 'flags': 'common_flags'}


def gather_int_columns(events):
  """Return [(label, vals)] for every int (wt==0) column, mirroring encode()."""
  by_type = {}
  for i, e in enumerate(events):
    by_type.setdefault(e['type'], []).append(i)
  cols = []
  for T in sorted(by_type):
    idxs = by_type[T]
    keys = set()
    for i in idxs:
      keys.update(events[i]['fields'].keys())
    for key in sorted(keys, key=lambda k: str(k)):
      vals, wt = [], None
      for i in idxs:
        f = events[i]['fields'].get(key)
        if f is not None:
          wt = f[0]
          vals.append(f[1])
      if wt == 0 and vals:                       # int column
        kn = KEYNAME.get(key, key if isinstance(key, int) else str(key))
        cols.append((f"T{T}:{kn}", vals))
  return cols


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input')
  ap.add_argument('--label', default=None)
  ap.add_argument('--topn', type=int, default=25)
  ap.add_argument('--json', action='store_true')
  args = ap.parse_args()
  label = args.label or os.path.basename(args.input)

  log(f"[{label}] parsing + de-bundling…")
  events, _row, _today = cg.collect(args.input)
  log(f"{len(events):,} events; gathering int columns…")
  columns = gather_int_columns(events)
  log(f"{len(columns)} int columns; running {len(CODECS)} codecs each "
      f"(round-trip gated)…")

  per_col = []
  enc_blobs = {c: [] for c in CODECS}        # for the aggregate
  raw_total = best_total = 0
  for ci, (lbl, vals) in enumerate(columns):
    n = len(vals)
    row = {'col': lbl, 'n': n, 'raw_B': {}, 'zstd_B': {}}
    for c, (enc, dec) in CODECS.items():
      blob = enc(vals)
      if dec(blob, n) != vals:                 # correctness gate
        sys.exit(f'ROUND-TRIP FAILED: codec {c} on column {lbl}')
      row['raw_B'][c] = len(blob)
      row['zstd_B'][c] = cg.zstd(blob)
      enc_blobs[c].append(blob)
    win = min(row['zstd_B'], key=row['zstd_B'].get)
    row['winner'] = win
    row['raw_zstd_B'] = row['zstd_B']['raw']
    row['win_zstd_B'] = row['zstd_B'][win]
    row['gain_vs_raw_pct'] = round(
        (row['zstd_B']['raw'] - row['zstd_B'][win]) / row['zstd_B']['raw'] * 100, 1) \
        if row['zstd_B']['raw'] else 0.0
    raw_total += row['raw_zstd_B']
    best_total += row['win_zstd_B']
    per_col.append(row)
    if (ci + 1) % 10 == 0:
      log(f"  {ci + 1}/{len(columns)} columns done")

  # realistic aggregate: concat all columns per codec, zstd once (1 MB blocks)
  log("aggregating (concat + single zstd per codec)…")
  agg_single = {c: cg.zstd(b''.join(enc_blobs[c])) for c in CODECS}
  # per_col is still in original column order here (sort happens below)
  best_concat = b''.join(enc_blobs[per_col[i]['winner']][i]
                         for i in range(len(columns)))
  agg_best = cg.zstd(best_concat)

  per_col.sort(key=lambda r: r['raw_zstd_B'], reverse=True)
  base = agg_single['raw']
  res = {
      'label': label, 'events': len(events), 'n_int_columns': len(columns),
      'aggregate_zstd_MB': {c: round(agg_single[c] / MB, 3) for c in CODECS},
      'aggregate_best_per_col_MB': round(agg_best / MB, 3),
      'aggregate_vs_raw': {c: round(agg_single[c] / base, 3) for c in CODECS},
      'best_per_col_vs_raw': round(agg_best / base, 3),
      'best_per_col_gain_pct': round((base - agg_best) / base * 100, 1),
      'per_column_isolated_sum_zstd_MB': {
          'raw': round(raw_total / MB, 3), 'best': round(best_total / MB, 3)},
      'top_columns': [
          {'col': r['col'], 'n': r['n'],
           'raw_zstd_KB': round(r['raw_zstd_B'] / 1024, 1),
           'winner': r['winner'],
           'win_zstd_KB': round(r['win_zstd_B'] / 1024, 1),
           'gain_vs_raw_pct': r['gain_vs_raw_pct']}
          for r in per_col[:args.topn]],
  }

  if not args.json:
    print('=' * 78)
    print(f"E8/E11 int-codec bake-off — [{label}]   {len(events):,} events   "
          f"{len(columns)} int columns")
    print('=' * 78)
    a = res['aggregate_zstd_MB']
    print("Aggregate int-column bytes (concat + zstd-12 @1M):")
    for c in CODECS:
      print(f"  all-{c:<5}: {a[c]:.3f} MB   ({res['aggregate_vs_raw'][c]}x raw)")
    print(f"  BEST-per-column: {res['aggregate_best_per_col_MB']:.3f} MB   "
          f"({res['best_per_col_vs_raw']}x raw)  -> int-codec gain "
          f"{res['best_per_col_gain_pct']}% over raw-varint")
    print('-' * 78)
    print(f"Top {args.topn} int columns by size (raw+zstd), winner & gain:")
    print(f"  {'column':<22}{'rows':>9}  {'raw KB':>8}  {'winner':>6}  "
          f"{'win KB':>8}  gain%")
    for t in res['top_columns']:
      print(f"  {t['col']:<22}{t['n']:>9}  {t['raw_zstd_KB']:>8.1f}  "
            f"{t['winner']:>6}  {t['win_zstd_KB']:>8.1f}  {t['gain_vs_raw_pct']:>5}")
    print('\n----- JSON -----')
  print('===JSON BEGIN==='); print(json.dumps(res, indent=2)); print('===JSON END===')


if __name__ == '__main__':
  main()
