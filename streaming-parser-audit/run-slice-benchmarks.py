#!/usr/bin/env python3
"""Repeat the same SQL-free binary in all four storage modes."""
from pathlib import Path
import json
import platform
import statistics
import subprocess
import time

root = Path(__file__).resolve().parent
binary = root.parent / 'out/mac_release/streaming_parser_benchmark'
runs = []
for workload in ('repeated', 'unique'):
    for repeat in range(5):
        modes = ['full', 'sink', 'drop', 'slice']
        if repeat % 2:
            modes.reverse()
        for mode in modes:
            start = time.perf_counter()
            result = subprocess.run([str(binary), mode, str(root / f'slices-{workload}.pftrace')],
                                    text=True, capture_output=True, check=True)
            row = json.loads(result.stdout)
            row.update(workload=workload, repeat=repeat, wall_s=time.perf_counter() - start)
            runs.append(row)
            (root / 'benchmark-slices-runs.json').write_text(json.dumps(runs, indent=2))
            print(json.dumps(row), flush=True)
summary = []
for workload in ('repeated', 'unique'):
    for mode in ('full', 'sink', 'drop', 'slice'):
        matching = [r for r in runs if r['workload'] == workload and r['mode'] == mode]
        summary.append(dict(workload=workload, mode=mode,
                            cpu_s=statistics.median(r['cpu_s'] for r in matching),
                            wall_s=statistics.median(r['wall_s'] for r in matching),
                            peak_rss_mib=statistics.median(r['peak_rss_bytes'] for r in matching) / 2**20,
                            slice_rows=matching[0]['slice_rows'], slice_frontier=matching[0]['slice_frontier'],
                            arg_rows=matching[0]['arg_rows'], arg_frontier=matching[0]['arg_frontier']))
    checksums = {r['checksum'] for r in runs if r['workload'] == workload and r['mode'] != 'full'}
    assert len(checksums) == 1, (workload, checksums)
(root / 'benchmark-slices-summary.json').write_text(json.dumps(dict(platform=platform.platform(),
    baseline='Full storage, same release binary; not an unmodified-checkout overhead comparison',
    methodology='5 fresh processes per mode/workload, alternating mode order; ru_utime+ru_stime, macOS ru_maxrss bytes; no SQL engine; globally ordered synthetic input; raw ftrace table disabled; 1MiB forced-sorter flushes; checksum sinks do not store output',
    results=summary), indent=2))
print(json.dumps(summary, indent=2))
