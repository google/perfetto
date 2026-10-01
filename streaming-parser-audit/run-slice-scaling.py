#!/usr/bin/env python3
"""Four-million-child scaling, fixed track/name/argument dictionaries."""
from pathlib import Path
import json
import statistics
import subprocess
root = Path(__file__).resolve().parent
binary = root.parent / 'out/mac_release/streaming_parser_benchmark'
runs = []
for repeat in range(5):
    for mode in (['full', 'slice'] if repeat % 2 == 0 else ['slice', 'full']):
        result = subprocess.run([str(binary), mode, '/tmp/perfetto-slice-scale/slices-repeated.pftrace'], text=True, capture_output=True, check=True)
        row = json.loads(result.stdout)
        row.update(repeat=repeat, count=4000000)
        runs.append(row)
        print(json.dumps(row), flush=True)
        (root / 'benchmark-slices-scaling-runs.json').write_text(json.dumps(runs, indent=2))
summary = []
previous = json.loads((root / 'benchmark-slices-summary.json').read_text())['results']
for mode in ('full', 'slice'):
    old = next(r for r in previous if r['mode'] == mode and r['workload'] == 'repeated')
    summary.append(dict(count=1000000, mode=mode, cpu_s=old['cpu_s'], peak_rss_mib=old['peak_rss_mib']))
    matching = [r for r in runs if r['mode'] == mode]
    summary.append(dict(count=4000000, mode=mode, cpu_s=statistics.median(r['cpu_s'] for r in matching), peak_rss_mib=statistics.median(r['peak_rss_bytes'] for r in matching)/2**20, slice_rows=matching[0]['slice_rows'], slice_frontier=matching[0]['slice_frontier']))
baseline = json.loads(subprocess.run([str(binary), 'sink', '/tmp/perfetto-slice-scale/slices-repeated.pftrace'], text=True, capture_output=True, check=True).stdout)
assert baseline['checksum'] == next(r['checksum'] for r in runs if r['mode'] == 'slice')
(root / 'benchmark-slices-scaling-equivalence.json').write_text(json.dumps(baseline, indent=2))
assert len({r['checksum'] for r in runs if r['mode'] == 'slice'}) == 1
assert next(r['checksum'] for r in runs if r['mode'] == 'slice') != '0'
(root / 'benchmark-slices-scaling-summary.json').write_text(json.dumps(dict(methodology='Five fresh-process medians, same SQL-free release binary, fixed dictionaries, ordered input with forced 1MiB sorter drains; input generation and tests completed before timing', results=summary), indent=2))
print(json.dumps(summary, indent=2))
