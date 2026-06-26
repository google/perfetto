#!/usr/bin/env python3
"""tracing-v2 Task 02 (device stress) — build an SMB *replay file* from a real
boot trace.

The device-stress harness recreates REAL write conditions by replaying, per CPU,
the exact sequence of de-bundled ftrace events from a zero-data-loss boot trace
(e.g. aot_boot) — instead of a synthetic uniform firehose. This tool does the
offline extraction:

  for every de-bundled ftrace event -> (cpu, ts, wire_size)
  -> grouped per CPU, emitted as (delta_ns, size) streams

The harness then runs one writer thread per CPU, each emitting its events at the
recorded inter-arrival gaps (scaled by 1/multiplier) and writing `size` bytes,
through the v2 SharedRingBuffer. Same timing, bursts and sizes as the real device.

Sizes are EXACT de-bundled FtraceEvent payload bytes, reconstructed with the
shared Task-1 `ftrace_expand` primitives (CompactSched un-packed, cpu added).

INPUT MUST BE DECOMPRESSED. If the .pftrace has compressed_packets, this tool
auto-runs `traceconv decompress_packets` when --traceconv is given (or pass an
already-decompressed file).

OUTPUT:
  <out>.smbr   binary replay file (consumed by the harness; format below)
  <out>.json   human-readable summary (per-CPU counts, bytes, implied 1x rates)

USAGE:
  build_replay.py aot_boot_t1.dec --out aot_boot_t1 [--traceconv path/to/traceconv]
"""

import argparse
import gzip
import json
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ftrace_expand as fx  # vendored copy (md5 must match Task-1 master)

MAGIC = b'SMBR'
VERSION = 1
U32_MAX = 0xFFFFFFFF
U16_MAX = 0xFFFF


def first_ts(ev_bytes):
  """Absolute timestamp (F_FE_TIMESTAMP, field 1 varint) of an FtraceEvent."""
  for fnum, wt, val in fx.iter_fields(ev_bytes):
    if fnum == fx.F_FE_TIMESTAMP and wt == 0:
      return val
  return 0


def load_trace_bytes(path):
  """Read a .pftrace, transparently gunzipping if it's file-level gzip
  (the in-repo traces are gzip'd; trace_processor handles this automatically,
  the raw proto parser does not)."""
  data = open(path, 'rb').read()
  if data[:2] == b'\x1f\x8b':
    data = gzip.decompress(data)
  return data


def has_compressed(data):
  for fnum, wt, packet in fx.iter_fields(data):
    if fnum != fx.F_TRACE_PACKET or wt != 2:
      continue
    for pf, _pwt, _pv in fx.iter_fields(packet):
      if pf == fx.F_TP_COMPRESSED:
        return True
  return False


def extract_events(path):
  """Single pass over the trace; yield (cpu, ts_ns, wire_size) per de-bundled
  event, in trace order. Mirrors ftrace_expand.analyze() but emits per-event
  (cpu, ts, size) instead of aggregate stats."""
  data = load_trace_bytes(path)
  if has_compressed(data):
    sys.exit('ERROR: trace has in-proto compressed_packets. Decompress first:\n'
             '  traceconv decompress_packets <in> <out>   (or pass --traceconv)')
  prev_chain = {}
  st = fx.Stats()  # reconstruct_compact_sched needs one; we ignore its totals
  for fnum, wt, packet in fx.iter_fields(data):
    if fnum != fx.F_TRACE_PACKET or wt != 2:
      continue
    bundle = None
    for pf, pwt, pval in fx.iter_fields(packet):
      if pf == fx.F_TP_FTRACE_EVENTS and pwt == 2:
        bundle = pval
    if bundle is None:
      continue
    cpu, individual, compact = 0, [], None
    for bf, bwt, bval in fx.iter_fields(bundle):
      if bf == fx.F_FEB_CPU and bwt == 0:
        cpu = bval
      elif bf == fx.F_FEB_EVENT and bwt == 2:
        individual.append(bval)
      elif bf == fx.F_FEB_COMPACT_SCHED and bwt == 2:
        compact = bval
    for ev in individual:
      fe = ev + fx.field_varint(fx.F_FE_CPU, cpu)
      yield cpu, first_ts(ev), len(fe)
    if compact is not None:
      for fe in fx.reconstruct_compact_sched(compact, cpu, prev_chain, st):
        yield cpu, first_ts(fe), len(fe)


def main():
  ap = argparse.ArgumentParser(
      description=__doc__,
      formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input', help='decompressed .pftrace (or compressed + --traceconv)')
  ap.add_argument('--out', required=True, help='output prefix (writes .smbr + .json)')
  ap.add_argument('--traceconv', default=None,
                  help='path to traceconv; if given and input is compressed, '
                       'auto-decompress to a temp file first')
  args = ap.parse_args()

  path = args.input
  if args.traceconv and has_compressed(load_trace_bytes(path)):
    dec = args.out + '.dec'
    print(f'[build_replay] decompress_packets -> {dec}', file=sys.stderr)
    subprocess.run([args.traceconv, 'decompress_packets', path, dec], check=True)
    path = dec

  # Collect per-CPU (ts, size) lists.
  per_cpu = {}  # cpu -> list[(ts, size)]
  n = 0
  for cpu, ts, size in extract_events(path):
    per_cpu.setdefault(cpu, []).append((ts, min(size, U16_MAX)))
    n += 1
  if not per_cpu:
    sys.exit('no ftrace events found (wrong/empty trace?)')

  cpus = sorted(per_cpu)
  global_min = min(v[0][0] for v in per_cpu.values() if v)
  global_max = max(v[-1][0] for v in per_cpu.values() if v)
  # Per-CPU streams are already in trace order; sort defensively by ts.
  for c in cpus:
    per_cpu[c].sort(key=lambda x: x[0])

  # --- write binary replay file ---
  # Format (little-endian):
  #   'SMBR' u32:version u32:n_cpus u64:total_events u64:duration_ns
  #   then per cpu: u32:cpu_id u32:count, then count*(u32:delta_ns u16:size)
  out_bin = args.out + '.smbr'
  with open(out_bin, 'wb') as f:
    f.write(MAGIC)
    f.write(struct.pack('<III', VERSION, len(cpus), 0))  # n_cpus; pad
    f.write(struct.pack('<QQ', n, global_max - global_min))
    summary_cpu = []
    for c in cpus:
      evs = per_cpu[c]
      f.write(struct.pack('<II', c, len(evs)))
      prev = evs[0][0]
      total_bytes = 0
      buf = bytearray()
      for ts, size in evs:
        d = ts - prev
        if d < 0:
          d = 0
        if d > U32_MAX:
          d = U32_MAX
        prev = ts
        total_bytes += size
        buf += struct.pack('<IH', d, size)
      f.write(buf)
      summary_cpu.append({'cpu': c, 'events': len(evs), 'bytes': total_bytes})

  dur_s = (global_max - global_min) / 1e9
  total_bytes = sum(c['bytes'] for c in summary_cpu)
  summary = {
      'source': os.path.basename(args.input),
      'replay_bin': os.path.basename(out_bin),
      'n_cpus': len(cpus),
      'total_events': n,
      'total_bytes_debundled': total_bytes,
      'duration_s': round(dur_s, 3),
      'agg_event_rate_per_s': round(n / dur_s) if dur_s else 0,
      'agg_MB_per_s_1x': round(total_bytes / 1e6 / dur_s, 2) if dur_s else 0,
      'avg_bytes_per_event': round(total_bytes / n, 1) if n else 0,
      'per_cpu': [
          dict(c,
               MB_per_s_1x=round(c['bytes'] / 1e6 / dur_s, 2) if dur_s else 0)
          for c in summary_cpu
      ],
  }
  with open(args.out + '.json', 'w') as f:
    json.dump(summary, f, indent=2)

  print(json.dumps(summary, indent=2))
  print(f'\n[build_replay] wrote {out_bin} ({os.path.getsize(out_bin)} bytes) '
        f'+ {args.out}.json', file=sys.stderr)


if __name__ == '__main__':
  main()
