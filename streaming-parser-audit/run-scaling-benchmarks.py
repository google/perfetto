#!/usr/bin/env python3
"""Compare table-retaining and sink-only parsing as trace length increases."""
from pathlib import Path
import json
import statistics
import subprocess
import time

root = Path(__file__).resolve().parent
binary = root.parent / 'out/mac_release/streaming_parser_benchmark'
runs = []
for repeat in range(5):
    modes = ['full', 'frontier'] if repeat % 2 == 0 else ['frontier', 'full']
    for mode in modes:
        start = time.perf_counter()
        result = subprocess.run([str(binary), mode,
            '/tmp/perfetto-streaming-scale/sched-moving.pftrace'],
            text=True, capture_output=True, check=True)
        row = json.loads(result.stdout)
        row.update(repeat=repeat, wall_s=time.perf_counter() - start)
        runs.append(row)
        print(json.dumps(row), flush=True)
        (root / 'benchmark-scaling-runs.json').write_text(json.dumps(runs, indent=2))
one_million = json.loads((root / 'benchmark-summary.json').read_text())['results']
summary = []
for mode in ('full', 'frontier'):
    first = next(r for r in one_million if r['workload'] == 'moving' and r['mode'] == mode)
    summary.append(dict(rows=1000000, mode=mode, cpu_s=first['cpu_s'], peak_rss_mib=first['peak_rss_mib']))
    matching = [r for r in runs if r['mode'] == mode]
    summary.append(dict(rows=4000000, mode=mode,
        cpu_s=statistics.median(r['cpu_s'] for r in matching),
        peak_rss_mib=statistics.median(r['peak_rss_bytes'] for r in matching) / 2**20,
        thread_state_rows=matching[0]['thread_state_rows'],
        thread_state_ts_capacity_bytes=matching[0]['thread_state_ts_capacity_bytes']))
(root / 'benchmark-scaling-summary.json').write_text(json.dumps(dict(
    methodology='Five processes per point, same release binary and four CPUs/fixed thread identities. 1MiB ordered-input sorter drains. Full has no sink; frontier includes checksum sinks. Scale input generator: generate-benchmark-input.py 4000000 /tmp/perfetto-streaming-scale',
    results=summary), indent=2))
print(json.dumps(summary, indent=2))
