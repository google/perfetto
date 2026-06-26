#!/usr/bin/env python3
"""Push arm64 zstd/lz4 + a trace subset to the Pixel and run cost_bench on each.

Thin orchestrator over the shared device layer (../../../../device/devicebench.py)
and the bench (../scripts/cost_bench.py). This is the reusable pattern for "run a
host-built tool over device-resident files": Task 2 can copy this shape and swap
the payload.

PRE: build arm64 `zstd` and `lz4` into one dir (see ../../../device/README.md),
and have de-bundled .pftrace files on this host.

USAGE
  python3 run_device.py ~/Desktop/tracing_v2/expanded/{aot_t1,first_unlock_t1,battery_long_t1}.pftrace \
      --bin-dir ~/arm64bin --secs 3 --out cost_device.json
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)             # cost_bench.py lives beside this file
import cost_bench as cb    # noqa: E402
cb._add_devicebench_to_path()        # locates tracing_v2/device/devicebench.py
import devicebench as db   # noqa: E402


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('traces', nargs='+', help='de-bundled .pftrace files (host paths)')
  ap.add_argument('--bin-dir', required=True, help='host dir holding arm64 zstd & lz4')
  ap.add_argument('--secs', type=int, default=3)
  ap.add_argument('--cores', default='all',
                  help="'all' = one core per cluster (little/mid/big), 'none' = "
                       "unpinned, or a comma list of cpu indices (e.g. 0,4,7)")
  ap.add_argument('--out', default='cost_device.json')
  args = ap.parse_args()

  info = db.require_device()
  print(f"device: {info['model']} {info['abi']} sdk{info['sdk']}", file=sys.stderr)
  if 'arm64' not in info['abi']:
    print(f"WARN: device abi is {info['abi']} — ensure your zstd/lz4 match it",
          file=sys.stderr)

  # Resolve which cores to pin to.
  if args.cores == 'none':
    cores = [{'name': 'unpinned', 'cpu': None}]
  elif args.cores == 'all':
    cores = db.clusters() or [{'name': 'unpinned', 'cpu': None}]
  else:
    cores = [{'name': f'cpu{c}', 'cpu': int(c)} for c in args.cores.split(',')]
  print("cores:", ", ".join(f"{c['name']}=cpu{c['cpu']}"
                            + (f"(cap {c.get('cap')})" if c.get('cap') else '')
                            for c in cores), file=sys.stderr)

  d = db.mktemp_dir('tv2_latency')
  try:
    for tool in ('zstd', 'lz4'):
      p = os.path.join(os.path.expanduser(args.bin_dir), tool)
      if os.path.exists(p):
        db.push(p, f'{d}/{tool}')
      else:
        print(f"WARN: {p} missing — {tool} will be skipped on device", file=sys.stderr)

    results = []
    for t in args.traces:
      t = os.path.expanduser(t)
      parent = os.path.basename(os.path.dirname(t))
      label = f"{parent}/{os.path.basename(t)}" if parent else os.path.basename(t)
      dev_path = f'{d}/{os.path.basename(t)}'
      print(f"[{label}] push …", file=sys.stderr)
      db.push(t, dev_path)
      for core in cores:
        prefix = db.taskset_prefix(core['cpu']) if core['cpu'] is not None else ''
        if core['cpu'] is not None and not prefix:
          print("WARN: taskset unavailable on device — running unpinned",
                file=sys.stderr)
        print(f"[{label}] core={core['name']} (cpu{core['cpu']}) bench …",
              file=sys.stderr)
        runner = cb.AdbRunner(db, d, prefix=prefix)
        res = cb.bench_file(runner, dev_path, bin_dir=d, secs=args.secs)
        res.update({'label': label, 'target': 'adb', 'device': info,
                    'cluster': core['name'], 'cpu': core['cpu'],
                    'in_b': os.path.getsize(t)})
        results.append(res)
      db.shell(f'rm -f {dev_path}')  # free device space between traces

    with open(args.out, 'w') as f:
      json.dump(results, f, indent=2)
    print("===JSON BEGIN===")
    print(json.dumps(results))
    print("===JSON END===")
    print(f"wrote {args.out}", file=sys.stderr)
  finally:
    db.rm_rf(d)  # never leave traces/binaries on the device


if __name__ == '__main__':
  main()
