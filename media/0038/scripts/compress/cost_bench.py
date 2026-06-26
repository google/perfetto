#!/usr/bin/env python3
"""tracing-v2 Task 02c — compression COST bench (CPU / memory), host or Pixel.

Prices a FIXED known-good shortlist (we are NOT re-sweeping size — that's settled
by primiano's doc + Task 3). Headline per config: compression MB/s, decompression
MB/s, peak RSS, ratio. The size knee is already chosen; here we ask "what does it
cost on real ARM, and does zstd@chosen-level beat today's gzip?".

Portable by construction: zstd/lz4 are driven via their built-in `-b` benchmark
mode (in-process timing loop → clean MB/s, self-verifies round-trip), so the same
tool runs on host (x86) and on the Pixel (arm64). The orchestrator + parsing ALWAYS
run host-side; `--target adb` just runs the tool on the device over adb (file must
already be pushed) and parses the output here.

USAGE
  # host (x86), file is a local de-bundled .pftrace stream:
  python3 cost_bench.py /tmp/ftrace_expanded.pftrace --label aot/t1

  # device (arm64), file + binaries already pushed to the device scratch dir:
  python3 cost_bench.py /data/local/tmp/tv2/aot.pftrace --target adb \
      --bin-dir /data/local/tmp/tv2 --label aot/t1

Input should be the DE-BUNDLED ftrace stream (Task 1/2 `ftrace_expanded.pftrace`)
so the cost reflects what traced would actually compress. Decompress field traces
first (`traceconv decompress_packets`). gzip is the today-baseline anchor; it has
no `-b` mode, so it is measured host-side only (on `--target adb` it is skipped
with a logged note — the gzip baseline lives on host / in Task 3).
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

MB = 1024 * 1024

# (display name, tool, level, extra args). FULL knee-finding sweep across all three
# codecs — we want to MAP the ratio-vs-latency curve, not just price known points.
SHORTLIST = [
    ('gzip-1',      'gzip', 1, []),
    ('gzip-6',      'gzip', 6, []),              # today's baseline anchor
    ('gzip-9',      'gzip', 9, []),
    # lz4: levels 1-9 are the fast band, higher are HC. Sweep the whole range.
    ('lz4-1',       'lz4',  1, []),
    ('lz4-3',       'lz4',  3, []),
    ('lz4-6',       'lz4',  6, []),
    ('lz4-9',       'lz4',  9, []),
    ('lz4-12',      'lz4',  12, []),             # lz4hc max
    # zstd: the codec we expect to win — dense sampling around the 3-6 knee.
    ('zstd-1',      'zstd', 1, []),
    ('zstd-2',      'zstd', 2, []),
    ('zstd-3',      'zstd', 3, []),
    ('zstd-4',      'zstd', 4, []),
    ('zstd-6',      'zstd', 6, []),
    ('zstd-9',      'zstd', 9, []),
    ('zstd-12',     'zstd', 12, []),
    ('zstd-19',     'zstd', 19, []),             # slow/best end of the curve
    ('zstd-6-long', 'zstd', 6, ['--long=27']),  # keep-window point (RSS ceiling)
]


def _add_devicebench_to_path():
  """Find tracing_v2/device/devicebench.py (repo layout) or a sibling copy (flat
  manual layout), walking up from this file. Works regardless of where scripts live."""
  d = os.path.dirname(os.path.abspath(__file__))
  for _ in range(7):
    for cand in (d, os.path.join(d, 'tracing_v2', 'device'),
                 os.path.join(d, 'device')):
      if os.path.exists(os.path.join(cand, 'devicebench.py')):
        sys.path.insert(0, cand)
        return
    d = os.path.dirname(d)

_SIZES = re.compile(r'(\d+)\s*->\s*(\d+)')
_SPEEDS = re.compile(r'([\d.]+)\s*MB/s')


def log(msg):
  print(msg, file=sys.stderr, flush=True)


# ---- runners: same tiny interface for host and device --------------------------

class HostRunner:
  name = 'host'

  def argv(self, argv):
    cp = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
    return cp.returncode, cp.stdout

  def have(self, tool):
    from shutil import which
    return which(tool) is not None

  def rss_kb(self, argv):
    # Poll /proc/<pid>/status:VmHWM (same method as the device runner — no
    # dependency on /usr/bin/time, identical semantics host vs Pixel).
    with open(os.devnull, 'wb') as dn:
      p = subprocess.Popen(argv, stdout=dn, stderr=dn)
      peak = 0
      while p.poll() is None:
        try:
          with open(f'/proc/{p.pid}/status') as f:
            for line in f:
              if line.startswith('VmHWM'):
                peak = max(peak, int(line.split()[1]))
        except (FileNotFoundError, ProcessLookupError):
          break
        time.sleep(0.02)
    return peak or None

  def tmp(self):
    return tempfile.NamedTemporaryFile(delete=False).name


class AdbRunner:
  name = 'adb'

  def __init__(self, db, scratch, prefix=''):
    self.db = db
    self.scratch = scratch
    self.prefix = prefix  # e.g. a taskset pin prefix; applied to every command

  def argv(self, argv):
    return self.db.shell(self.prefix
                         + ' '.join(shlex.quote(a) for a in argv) + ' 2>&1')

  def have(self, tool):
    return self.db.have(tool)

  def rss_kb(self, argv):
    return self.db.rss_peak_kb(' '.join(shlex.quote(a) for a in argv),
                               prefix=self.prefix)

  def tmp(self):
    return f'{self.scratch}/cost_tmp.bin'


# ---- measurement ---------------------------------------------------------------

def parse_bench(out):
  """Parse a zstd/lz4 `-b` result: '<in> -> <out> (...), <C> MB/s, <D> MB/s'.

  Both tools redraw the line with '\\r' as they go, and intermediate frames show
  only the compression speed — so we scan every '\\r'/'\\n' fragment and keep the
  LAST one that carries BOTH speeds (= the final, complete measurement)."""
  best = None
  for frag in re.split(r'[\r\n]', out):
    sz = _SIZES.search(frag)
    sp = _SPEEDS.findall(frag)
    if sz and len(sp) >= 2:
      in_b, out_b = int(sz.group(1)), int(sz.group(2))
      best = {'in_b': in_b, 'out_b': out_b,
              'ratio': round(in_b / out_b, 3) if out_b else None,
              'c_mbs': float(sp[0]), 'd_mbs': float(sp[1])}
  return best


def bench_compressor(runner, tool, level, extra, path, bin_dir, secs):
  exe = f'{bin_dir}/{tool}' if bin_dir else tool
  argv = [exe, f'-b{level}', f'-e{level}', f'-i{secs}', *extra, path]
  rc, out = runner.argv(argv)
  r = parse_bench(out)
  if r is None:
    log(f"    ! could not parse {tool} -b output:\n{out.strip()[:300]}")
  return r


def bench_gzip_host(level, path, secs):
  """gzip has no -b mode: single-pass wall timing (host only). Repeats to ~secs."""
  in_b = os.path.getsize(path)
  out_path = tempfile.NamedTemporaryFile(delete=False).name
  try:
    # compress (median of a few passes to amortize cache effects)
    ct = []
    while sum(ct) < secs or len(ct) < 2:
      t0 = time.monotonic()
      with open(out_path, 'wb') as o:
        subprocess.run(['gzip', f'-{level}', '-c', path], stdout=o,
                       stderr=subprocess.DEVNULL)
      ct.append(time.monotonic() - t0)
      if len(ct) >= 5:
        break
    out_b = os.path.getsize(out_path)
    # decompress
    dt = []
    while sum(dt) < secs or len(dt) < 2:
      t0 = time.monotonic()
      subprocess.run(['gzip', '-dc', out_path], stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
      dt.append(time.monotonic() - t0)
      if len(dt) >= 5:
        break
    cmin, dmin = min(ct), min(dt)
    return {'in_b': in_b, 'out_b': out_b,
            'ratio': round(in_b / out_b, 3) if out_b else None,
            'c_mbs': round((in_b / MB) / cmin, 1) if cmin else None,
            'd_mbs': round((in_b / MB) / dmin, 1) if dmin else None}
  finally:
    os.path.exists(out_path) and os.remove(out_path)


def bench_gzip_device(runner, level, path, secs):
  """gzip on the device (no -b mode). Timed HOST-side with overhead subtraction —
  toybox's `date +%s%N` is unreliable for sub-second timing, so instead we run a
  1-iteration and a K-iteration device loop and take per-iter = (t_K - t_1)/(K-1).
  The subtraction cancels the fixed adb + process-spawn overhead. The loop runs
  under the runner's taskset pin (via runner.argv), so the pin is respected."""
  out = f'{runner.scratch}/gz_tmp'
  _, sz = runner.argv(['wc', '-c', path])
  try:
    in_b = int(sz.split()[0])
  except (ValueError, IndexError):
    log("    ! gzip-device: could not stat input"); return None
  mb = in_b / MB

  def body(decomp):
    return (f'gzip -dc {shlex.quote(out)} >/dev/null' if decomp
            else f'gzip -{level} -c {shlex.quote(path)} >{shlex.quote(out)}')

  def loop(k, decomp):
    return ['sh', '-c', f'i=0; while [ $i -lt {k} ]; do {body(decomp)}; i=$((i+1)); done']

  def per_iter_mbs(decomp):
    t0 = time.monotonic(); runner.argv(loop(1, decomp)); t1 = time.monotonic() - t0
    k = max(2, min(64, int(secs / max(t1, 0.05)) + 1))
    t0 = time.monotonic(); runner.argv(loop(k, decomp)); tk = time.monotonic() - t0
    per = (tk - t1) / (k - 1)
    return round(mb / per, 1) if per > 0 else None

  c = per_iter_mbs(False)                 # compress first → creates `out`
  _, osz = runner.argv(['wc', '-c', out])
  try:
    out_b = int(osz.split()[0])
  except (ValueError, IndexError):
    out_b = None
  d = per_iter_mbs(True)
  return {'in_b': in_b, 'out_b': out_b,
          'ratio': round(in_b / out_b, 3) if out_b else None,
          'c_mbs': c, 'd_mbs': d}


def bench_file(runner, path, bin_dir='', secs=3):
  """Run the full shortlist over one file. Returns the result dict (JSON-ready)."""
  rows = []
  for name, tool, level, extra in SHORTLIST:
    tool_path = f'{bin_dir}/{tool}' if (bin_dir and tool != 'gzip') else tool
    if not runner.have(tool_path):
      log(f"    - {name}: skipped, `{tool}` not found on {runner.name}")
      continue
    log(f"    {name} …")
    if tool == 'gzip':
      r = (bench_gzip_host(level, path, secs) if runner.name == 'host'
           else bench_gzip_device(runner, level, path, secs))
    else:
      r = bench_compressor(runner, tool, level, extra, path, bin_dir, secs)
    if r:
      r['cfg'] = name
      rows.append(r)

  # RSS probes: window cost. zstd-6 (small window) vs zstd-6 --long (128 MB window).
  rss = {}
  zstd = f'{bin_dir}/zstd' if bin_dir else 'zstd'
  if runner.have(zstd):
    for name, extra in [('zstd-6', []), ('zstd-6-long', ['--long=27'])]:
      out_tmp = runner.tmp()
      kb = runner.rss_kb([zstd, '-6', *extra, '-q', '-f', '-o', out_tmp, path])
      rss[name] = kb
      log(f"    rss {name}: {kb} KB")
  return {'rows': rows, 'rss_kb': rss, 'in_b': os.path.getsize(path)
          if runner.name == 'host' else None}


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input', help='de-bundled .pftrace (host path, or device path '
                                'with --target adb)')
  ap.add_argument('--label', default=None)
  ap.add_argument('--target', choices=['host', 'adb'], default='host')
  ap.add_argument('--bin-dir', default='', help='dir holding zstd/lz4 (device '
                                                'scratch dir for --target adb)')
  ap.add_argument('--secs', type=int, default=3, help='per-config bench seconds')
  ap.add_argument('--json', action='store_true', help='print only the JSON block')
  args = ap.parse_args()
  label = args.label or os.path.basename(args.input)

  if args.target == 'adb':
    # find devicebench.py: same dir (manual flat layout) OR the repo tree.
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (here, os.path.join(here, '..', '..', '..', '..', 'device')):
      if os.path.exists(os.path.join(cand, 'devicebench.py')):
        sys.path.insert(0, cand)
        break
    import devicebench as db  # noqa: E402
    info = db.require_device()
    log(f"[{label}] device: {info['model']} ({info['abi']}, sdk {info['sdk']})")
    runner = AdbRunner(db, args.bin_dir or db.SCRATCH)
    dev = info
  else:
    runner = HostRunner()
    dev = {'host': os.uname().machine}

  log(f"[{label}] benching on {runner.name} ({args.secs}s/config) …")
  res = bench_file(runner, args.input, args.bin_dir, args.secs)
  res.update({'label': label, 'target': runner.name, 'device': dev})

  if not args.json:
    print(f"\n=== {label} on {runner.name} ===")
    print(f"{'config':<14}{'ratio':>7}{'comp MB/s':>11}{'decomp MB/s':>13}")
    base = next((r for r in res['rows'] if r['cfg'] == 'gzip-6'), None)
    for r in res['rows']:
      tag = ''
      if base and r is not base and r['c_mbs'] and base['c_mbs']:
        tag = f"   ({r['c_mbs']/base['c_mbs']:.1f}x gzip comp speed)"
      print(f"{r['cfg']:<14}{r['ratio'] or '-':>7}{r['c_mbs'] or '-':>11}"
            f"{r['d_mbs'] or '-':>13}{tag}")
    if res['rss_kb']:
      print("peak RSS:", ", ".join(f"{k}={v}KB" for k, v in res['rss_kb'].items()))
  print("===JSON BEGIN===")
  print(json.dumps(res))
  print("===JSON END===")


if __name__ == '__main__':
  main()
