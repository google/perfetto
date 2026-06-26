#!/usr/bin/env python3
"""De-bundle every trace in the corpus into the two ftrace-only streams the
recompression study compresses:

  <out>/<cat>_<tN>.today.pftrace  = today's form (FtraceEventBundle + CompactSched)
  <out>/<cat>_<tN>.v2.pftrace     = de-bundled firehose (one FtraceEvent/packet)

Field traces ship DEFLATE-compressed, so each is `traceconv decompress_packets`'d
first. Inputs are only read, never modified. Logs every step.

USAGE: python3 debundle_corpus.py TRACES_DIR OUT_DIR TRACECONV [cat/tN ...]
       (no trace list => all <cat>/t{1,2}.pftrace under TRACES_DIR)
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ftrace_expand as fx  # noqa: E402

MB = 1024 * 1024


def main():
  traces_dir, out_dir, traceconv = sys.argv[1], sys.argv[2], sys.argv[3]
  os.makedirs(out_dir, exist_ok=True)
  if len(sys.argv) > 4:
    items = sys.argv[4:]
  else:
    items = []
    for cat in sorted(os.listdir(traces_dir)):
      for tn in ('t1', 't2'):
        if os.path.isfile(os.path.join(traces_dir, cat, f'{tn}.pftrace')):
          items.append(f'{cat}/{tn}')
  t0 = time.monotonic()
  for it in items:
    cat, tn = it.split('/')
    safe = f'{cat}_{tn}'
    src = os.path.join(traces_dir, cat, f'{tn}.pftrace')
    today = os.path.join(out_dir, f'{safe}.today.pftrace')
    v2 = os.path.join(out_dir, f'{safe}.v2.pftrace')
    if os.path.exists(today) and os.path.exists(v2):
      print(f"[+{int(time.monotonic()-t0)}s] {it}: already done, skip", flush=True)
      continue
    dec = os.path.join(out_dir, f'{safe}.dec.tmp')
    print(f"[+{int(time.monotonic()-t0)}s] {it}: decompress_packets…", flush=True)
    # traceconv returns nonzero on these field traces but still writes a valid
    # decompressed stream (empty stderr) — verify by output, not exit code.
    subprocess.run([traceconv, 'decompress_packets', src, dec],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not (os.path.exists(dec) and os.path.getsize(dec) > 0):
      print(f"[+{int(time.monotonic()-t0)}s] {it}: decompress produced no output, SKIP",
            flush=True)
      continue
    print(f"[+{int(time.monotonic()-t0)}s] {it}: de-bundling…", flush=True)
    d = os.path.join(out_dir, f'{safe}.d')
    os.makedirs(d, exist_ok=True)
    st = fx.analyze(dec, write_traces=True, out_dir=d)
    os.replace(os.path.join(d, 'ftrace_only.pftrace'), today)
    os.replace(os.path.join(d, 'ftrace_expanded.pftrace'), v2)
    shutil.rmtree(d, ignore_errors=True)
    os.remove(dec)
    print(f"[+{int(time.monotonic()-t0)}s] {it}: {st.total_events:,} events  "
          f"today {os.path.getsize(today)/MB:.1f} MB  v2 {os.path.getsize(v2)/MB:.1f} MB",
          flush=True)
  print(f"[+{int(time.monotonic()-t0)}s] ALL DONE -> {out_dir}", flush=True)


if __name__ == '__main__':
  main()
