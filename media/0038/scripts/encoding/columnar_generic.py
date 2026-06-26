#!/usr/bin/env python3
"""tracing-v2 Task 04: GENERIC field-agnostic columnar encoder (prototype).

Takes de-bundled ftrace events and re-encodes them columnar, WITHOUT any
per-event knowledge — purely by proto field number + wire type. See DESIGN.md.

Encoding (Phase A + key levers):
  - partition events by type (= payload sub-message field number)
  - global columns: timestamp (delta+zigzag), type-id, in event order
  - per-(type,field) columns; optional fields carry a presence bitmap
  - string/bytes columns -> a GLOBAL dictionary (interned across the whole window,
    the traced-side win) + an index column
  - int columns -> raw packed varints (codec bake-off is the next phase)
  - then zstd the whole blob
  - round-trips (value-exact) as the correctness gate.

Reports columnar+zstd vs plain-zstd-row (Task 3 Option 1) vs today (bundled+compact).
INPUT MUST BE DECOMPRESSED.
USAGE: python3 columnar_generic.py decomp.pftrace --label "config/name"
       --ts {delta,raw,shuffle}   (timestamp codec to try; default delta)
"""

import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ftrace_expand as fx  # noqa: E402

MB = 1024 * 1024
_T0 = time.monotonic()


def log(msg):
  """Progress to stderr (flushed) — shows live in the terminal / .log file."""
  print(f"[+{int(time.monotonic() - _T0)}s] {msg}", file=sys.stderr, flush=True)


# --- varint / bit helpers ---------------------------------------------------
def uvarint(v):
  out = bytearray()
  while v > 0x7F:
    out.append(0x80 | (v & 0x7F)); v >>= 7
  out.append(v & 0x7F)
  return bytes(out)


def pack_uvarints(vals):
  return b''.join(uvarint(v) for v in vals)


def read_uvarint(b, p):
  r = s = 0
  while True:
    x = b[p]; p += 1; r |= (x & 0x7F) << s
    if not (x & 0x80):
      return r, p
    s += 7


def read_uvarints(blob, n):
  out, p = [], 0
  for _ in range(n):
    v, p = read_uvarint(blob, p); out.append(v)
  return out


def zz(v):
  return (v << 1) ^ (v >> 63)


def unzz(v):
  return (v >> 1) ^ -(v & 1)


def pack_bits(bits):
  out = bytearray((len(bits) + 7) // 8)
  for i, b in enumerate(bits):
    if b:
      out[i >> 3] |= 1 << (i & 7)
  return bytes(out)


def read_bit(blob, i):
  return (blob[i >> 3] >> (i & 7)) & 1


def shuffle8(vals):
  """byte-plane split of 8-byte values (E12)."""
  planes = [bytearray(len(vals)) for _ in range(8)]
  for i, v in enumerate(vals):
    for k in range(8):
      planes[k][i] = (v >> (8 * k)) & 0xFF
  return b''.join(bytes(p) for p in planes)


def unshuffle8(blob, n):
  planes = [blob[k * n:(k + 1) * n] for k in range(8)]
  out = []
  for i in range(n):
    v = 0
    for k in range(8):
      v |= planes[k][i] << (8 * k)
    out.append(v)
  return out


# --- parse de-bundled events ------------------------------------------------
def collect(path):
  """Return (events, row_payload_bytes, today_payload_bytes).
  event = {'ts':int,'type':int,'fields':{key:(wt,val)}}, key in {'pid','cpu',
  'flags'} or int sub-field number."""
  data = open(path, 'rb').read()
  events, row, today = [], bytearray(), bytearray()
  prev_chain = {}
  st = fx.Stats()

  def parse_fe(fe):
    ev = {'ts': 0, 'type': 0, 'fields': {}}
    for fnum, wt, val in fx.iter_fields(fe):
      if fnum == fx.F_FE_TIMESTAMP and wt == 0:
        ev['ts'] = val
      elif fnum == fx.F_FE_PID and wt == 0:
        ev['fields']['pid'] = (0, val)
      elif fnum == fx.F_FE_COMMON_FLAGS and wt == 0:
        ev['fields']['flags'] = (0, val)
      elif fnum == fx.F_FE_CPU and wt == 0:
        ev['fields']['cpu'] = (0, val)
      elif wt == 2:                          # the event payload sub-message
        ev['type'] = fnum
        for sf, swt, sval in fx.iter_fields(val):
          ev['fields'][sf] = (swt, sval)
      else:
        ev['fields'][('top', fnum)] = (wt, val)
    return ev

  for fnum, wt, packet in fx.iter_fields(data):
    if fnum != fx.F_TRACE_PACKET or wt != 2:
      continue
    bundle = None
    for pf, pwt, pv in fx.iter_fields(packet):
      if pf == fx.F_TP_FTRACE_EVENTS and pwt == 2:
        bundle = pv
    if bundle is None:
      continue
    today += bundle
    cpu, indiv, cs = 0, [], None
    for bf, bwt, bv in fx.iter_fields(bundle):
      if bf == fx.F_FEB_CPU and bwt == 0:
        cpu = bv
      elif bf == fx.F_FEB_EVENT and bwt == 2:
        indiv.append(bv)
      elif bf == fx.F_FEB_COMPACT_SCHED and bwt == 2:
        cs = bv
    for ev in indiv:
      fe = ev + fx.field_varint(fx.F_FE_CPU, cpu)
      row += fe
      events.append(parse_fe(fe))
    if cs is not None:
      for fe in fx.reconstruct_compact_sched(cs, cpu, prev_chain, st):
        row += fe
        events.append(parse_fe(fe))
  return events, bytes(row), bytes(today)


# --- columnar encode / decode ----------------------------------------------
def keyrepr(k):
  return ['c', k] if isinstance(k, str) else (['t', k[1]] if isinstance(k, tuple)
                                              else ['f', k])


def keyparse(k):
  return k[1] if k[0] == 'c' else (('top', k[1]) if k[0] == 't' else k[1])


def encode(events, ts_codec='delta', dict_mode='global'):
  n = len(events)
  ts = [e['ts'] for e in events]
  types = [e['type'] for e in events]

  dict_list, dict_idx = [], {}

  def intern(b):
    i = dict_idx.get(b)
    if i is None:
      i = len(dict_list); dict_idx[b] = i; dict_list.append(b)
    return i

  by_type = {}
  for i, e in enumerate(events):
    by_type.setdefault(e['type'], []).append(i)

  col_blobs = []
  type_meta = []
  for T in sorted(by_type):
    idxs = by_type[T]
    keys = set()
    for i in idxs:
      keys.update(events[i]['fields'].keys())
    cols = []
    for key in sorted(keys, key=lambda k: str(k)):
      present, vals, wt = [], [], None
      for i in idxs:
        f = events[i]['fields'].get(key)
        if f is None:
          present.append(0)
        else:
          present.append(1); wt = f[0]; vals.append(f[1])
      allp = all(present)
      if wt == 0:                                  # int column -> raw varints
        blob = pack_uvarints(vals); codec = 'raw'
      elif dict_mode == 'global':                  # bytes -> global dictionary
        idxv = [intern(v if isinstance(v, bytes) else bytes(v)) for v in vals]
        blob = pack_uvarints(idxv); codec = 'dict'
      else:                                        # bytes -> raw len-prefixed (no dict)
        blob = b''.join(uvarint(len(v)) + v for v in
                        (vv if isinstance(vv, bytes) else bytes(vv) for vv in vals))
        codec = 'rawb'
      pres = b'' if allp else pack_bits(present)
      cols.append({'key': keyrepr(key), 'wt': wt, 'codec': codec,
                   'np': len(vals), 'allp': allp,
                   'blen': len(blob), 'plen': len(pres)})
      col_blobs.append(blob + pres)
    type_meta.append({'T': T, 'n': len(idxs), 'cols': cols})

  if ts_codec == 'delta':
    deltas = [ts[i] - (ts[i - 1] if i else 0) for i in range(n)]
    ts_blob = pack_uvarints([zz(d) for d in deltas])
  elif ts_codec == 'shuffle':
    ts_blob = shuffle8(ts)
  else:
    ts_blob = pack_uvarints(ts)
  type_blob = pack_uvarints(types)
  dict_blob = b''.join(uvarint(len(s)) + s for s in dict_list)

  header = {'n': n, 'ts_codec': ts_codec, 'ts_len': len(ts_blob),
            'type_len': len(type_blob), 'dict_len': len(dict_blob),
            'dict_n': len(dict_list), 'types': type_meta}
  hb = json.dumps(header, separators=(',', ':')).encode()
  return uvarint(len(hb)) + hb + ts_blob + type_blob + dict_blob + b''.join(col_blobs)


def decode(blob):
  hlen, p = read_uvarint(blob, 0)
  header = json.loads(blob[p:p + hlen]); p += hlen
  n = header['n']
  ts_blob = blob[p:p + header['ts_len']]; p += header['ts_len']
  type_blob = blob[p:p + header['type_len']]; p += header['type_len']
  dict_blob = blob[p:p + header['dict_len']]; p += header['dict_len']

  if header['ts_codec'] == 'delta':
    d = read_uvarints(ts_blob, n); ts, acc = [], 0
    for x in d:
      acc += unzz(x); ts.append(acc)
  elif header['ts_codec'] == 'shuffle':
    ts = unshuffle8(ts_blob, n)
  else:
    ts = read_uvarints(ts_blob, n)
  types = read_uvarints(type_blob, n)
  dl, dp = [], 0
  for _ in range(header['dict_n']):
    ln, dp = read_uvarint(dict_blob, dp); dl.append(dict_blob[dp:dp + ln]); dp += ln

  # rebuild per-type column value iterators
  type_cols = {}
  for tm in header['types']:
    cols = {}
    for c in tm['cols']:
      cb = blob[p:p + c['blen']]; p += c['blen']
      pb = blob[p:p + c['plen']]; p += c['plen']
      if c['codec'] == 'rawb':
        vals, p2 = [], 0
        for _ in range(c['np']):
          ln, p2 = read_uvarint(cb, p2); vals.append(cb[p2:p2 + ln]); p2 += ln
      else:
        raw = read_uvarints(cb, c['np'])
        vals = raw if c['codec'] == 'raw' else [dl[i] for i in raw]
      cols[tuple(c['key'])] = {'vals': vals, 'allp': c['allp'], 'pres': pb,
                               'wt': c['wt'], 'pos': 0, 'row': 0}
    type_cols[tm['T']] = cols

  out = []
  for i in range(n):
    T = types[i]; ev = {'ts': ts[i], 'type': T, 'fields': {}}
    for keyb, col in type_cols[T].items():
      r = col['row']; col['row'] += 1
      has = col['allp'] or read_bit(col['pres'], r)
      if has:
        v = col['vals'][col['pos']]; col['pos'] += 1
        ev['fields'][keyparse(list(keyb))] = (col['wt'], v)
    out.append(ev)
  return out


# --- harness ----------------------------------------------------------------
def zstd(data, level=12, block=1 * MB):
  argv = ['zstd', f'-{level}', '-c']
  total = 0
  for i in range(0, len(data), block):
    total += len(subprocess.run(argv, input=data[i:i + block],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL).stdout)
  return total


def gzip6(data, block=1 * MB):
  total = 0
  for i in range(0, len(data), block):
    total += len(subprocess.run(['gzip', '-6', '-c'], input=data[i:i + block],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL).stdout)
  return total


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input')
  ap.add_argument('--label', default=None)
  ap.add_argument('--ts', default='delta', choices=['delta', 'raw', 'shuffle'])
  ap.add_argument('--dict', default='global', choices=['global', 'none'])
  ap.add_argument('--json', action='store_true')
  args = ap.parse_args()
  label = args.label or os.path.basename(args.input)

  log(f"[{label}] parsing + de-bundling ftrace (slow first step)…")
  events, row, today = collect(args.input)
  log(f"{len(events):,} events parsed; encoding columnar "
      f"(ts={args.ts}, dict={args.dict})…")
  t_enc = time.monotonic()
  blob = encode(events, ts_codec=args.ts, dict_mode=args.dict)
  enc_s = time.monotonic() - t_enc
  log(f"encoded {len(blob)//MB} MB columnar in {enc_s:.1f}s; verifying round-trip…")

  # correctness gate: value-exact round-trip
  t_dec = time.monotonic()
  dec = decode(blob)
  dec_s = time.monotonic() - t_dec
  ok = (len(dec) == len(events))
  if ok:
    for a, b in zip(events, dec):
      if a['ts'] != b['ts'] or a['type'] != b['type'] or a['fields'] != b['fields']:
        ok = False
        break
  if not ok:
    sys.exit('ROUND-TRIP FAILED — encoding is lossy, aborting (no size claim)')

  log("round-trip OK; compressing (columnar / row / today)…")
  col_z = zstd(blob)
  row_z = zstd(row)
  today_z = zstd(today)
  today_g = gzip6(today)
  log("done")
  result = {
      'label': label, 'events': len(events), 'roundtrip_ok': ok,
      'raw_MB': {'today_bundled': round(len(today) / MB, 2),
                 'row_debundled': round(len(row) / MB, 2),
                 'columnar': round(len(blob) / MB, 2)},
      'compressed_MB': {
          'today_gzip': round(today_g / MB, 3),
          'today_zstd12': round(today_z / MB, 3),
          'row_zstd12': round(row_z / MB, 3),
          'columnar_zstd12': round(col_z / MB, 3),
      },
      'vs_today_gzip': {
          'row(Opt1)': round(row_z / today_g, 3),
          'columnar(Opt2generic)': round(col_z / today_g, 3),
      },
      'ts_codec': args.ts, 'dict_mode': args.dict,
      'encode_MBps': round((len(row) / MB) / enc_s, 1) if enc_s else None,
      'decode_MBps': round((len(row) / MB) / dec_s, 1) if dec_s else None,
  }

  if not args.json:
    c = result['compressed_MB']
    print('=' * 72)
    print(f"Task 04 generic columnar — [{label}]   {len(events):,} events   "
          f"round-trip {'OK' if ok else 'FAIL'}")
    print('=' * 72)
    r = result['raw_MB']
    print(f"RAW: today(bundled) {r['today_bundled']} MB   row(de-bundled) "
          f"{r['row_debundled']} MB   columnar {r['columnar']} MB")
    print('-' * 72)
    print(f"compressed (zstd-12 @1M, ftrace payload only):")
    print(f"  today    (bundled+compact)  : {c['today_gzip']:.2f} MB gzip / "
          f"{c['today_zstd12']:.2f} MB zstd")
    print(f"  Option 1 (de-bundled row)   : {c['row_zstd12']:.2f} MB zstd   "
          f"({result['vs_today_gzip']['row(Opt1)']}x today-gzip)")
    print(f"  Option 2 (generic columnar) : {c['columnar_zstd12']:.2f} MB zstd   "
          f"({result['vs_today_gzip']['columnar(Opt2generic)']}x today-gzip)   "
          f"[ts={args.ts}]")
    print('\n----- JSON -----')
  print('===JSON BEGIN==='); print(json.dumps(result, indent=2)); print('===JSON END===')


if __name__ == '__main__':
  main()
