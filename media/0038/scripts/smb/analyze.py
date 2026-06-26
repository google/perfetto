#!/usr/bin/env python3
"""tracing-v2 Task 02 — aggregate the v0 sweep CSVs into median tables.

Each config is run REPS times (same label). The reader/writer hand-off race is
timing-sensitive, so we report the MEDIAN across reps (and the spread) rather
than a single noisy run. Prints compact markdown tables to stdout.

Usage: analyze.py [results_dir]
"""
import csv
import os
import statistics
import sys

RESULTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), '..', 'results')


def load(name):
    path = os.path.join(RESULTS, name)
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def group_median(rows, key_cols):
    """Group rows by the tuple of key_cols, return list of dicts with medians."""
    groups = {}
    for r in rows:
        k = tuple(r[c] for c in key_cols)
        groups.setdefault(k, []).append(r)
    out = []
    for k, rs in groups.items():
        d = {c: k[i] for i, c in enumerate(key_cols)}
        for col in ('loss_rate_pct', 'in_rate_mbps', 'drain_rate_mbps',
                    'peak_occ_pct', 'near_full_pct', 'write_ns_per_msg',
                    'data_losses_global'):
            vals = [float(x[col]) for x in rs]
            d[col + '_med'] = statistics.median(vals)
            d[col + '_min'] = min(vals)
            d[col + '_max'] = max(vals)
        d['_n'] = len(rs)
        out.append(d)
    return out


def fnum(x, p=2):
    return f"{x:.{p}f}"


def table(title, rows, cols):
    print(f"\n### {title}\n")
    print("| " + " | ".join(h for h, _ in cols) + " |")
    print("|" + "|".join("---" for _ in cols) + "|")
    for r in rows:
        print("| " + " | ".join(fn(r) for _, fn in cols) + " |")


def main():
    # E1 reader ceiling
    e1 = group_median(load('e1_reader_ceiling.csv'), ['writers'])
    e1.sort(key=lambda r: int(r['writers']))
    table("E1 — reader ceiling (flat-out producers, unlimited reader)", e1, [
        ("writers", lambda r: r['writers']),
        ("in MB/s", lambda r: fnum(r['in_rate_mbps_med'], 0)),
        ("drain MB/s (med)", lambda r: fnum(r['drain_rate_mbps_med'], 0)),
        ("drain min..max", lambda r: f"{r['drain_rate_mbps_min']:.0f}..{r['drain_rate_mbps_max']:.0f}"),
        ("loss% (med)", lambda r: fnum(r['loss_rate_pct_med'])),
        ("write ns/msg", lambda r: fnum(r['write_ns_per_msg_med'], 0)),
    ])

    # E2 knee
    e2 = group_median(load('e2_knee.csv'), ['writers', 'rate_target_mbps'])
    e2.sort(key=lambda r: (int(r['writers']), float(r['rate_target_mbps'])))
    table("E2 — loss-onset knee (sweep target in-rate)", e2, [
        ("W", lambda r: r['writers']),
        ("rate tgt", lambda r: fnum(float(r['rate_target_mbps']), 0)),
        ("in MB/s (med)", lambda r: fnum(r['in_rate_mbps_med'], 0)),
        ("loss% med", lambda r: fnum(r['loss_rate_pct_med'])),
        ("loss% min..max", lambda r: f"{r['loss_rate_pct_min']:.2f}..{r['loss_rate_pct_max']:.2f}"),
        ("peak occ%", lambda r: fnum(r['peak_occ_pct_med'], 0)),
    ])

    # E3 SMB size
    e3 = group_median(load('e3_smb_size.csv'), ['chunks'])
    e3.sort(key=lambda r: int(r['chunks']))
    table("E3 — SMB size sweep (8 writers, 150 MB/s target)", e3, [
        ("chunks", lambda r: r['chunks']),
        ("SMB KB", lambda r: str(int(r['chunks']) * 256 // 1024)),
        ("loss% med", lambda r: fnum(r['loss_rate_pct_med'])),
        ("loss% min..max", lambda r: f"{r['loss_rate_pct_min']:.2f}..{r['loss_rate_pct_max']:.2f}"),
        ("peak occ%", lambda r: fnum(r['peak_occ_pct_med'], 0)),
        ("drain MB/s", lambda r: fnum(r['drain_rate_mbps_med'], 0)),
    ])

    # E4 writers at fixed aggregate rate
    e4 = group_median(load('e4_writers.csv'), ['writers'])
    e4.sort(key=lambda r: int(r['writers']))
    table("E4 — writer-count contention (fixed 100 MB/s aggregate)", e4, [
        ("writers", lambda r: r['writers']),
        ("loss% med", lambda r: fnum(r['loss_rate_pct_med'])),
        ("loss% min..max", lambda r: f"{r['loss_rate_pct_min']:.2f}..{r['loss_rate_pct_max']:.2f}"),
        ("drain MB/s", lambda r: fnum(r['drain_rate_mbps_med'], 0)),
        ("peak occ%", lambda r: fnum(r['peak_occ_pct_med'], 0)),
        ("write ns/msg", lambda r: fnum(r['write_ns_per_msg_med'], 0)),
    ])

    # E5 reader pacing
    e5 = group_median(load('e5_reader_pacing.csv'), ['drain_target_mbps'])
    e5.sort(key=lambda r: float(r['drain_target_mbps']))
    table("E5 — reader aggressiveness (8 writers, 100 MB/s; drain tgt 0=flat out)", e5, [
        ("drain tgt", lambda r: fnum(float(r['drain_target_mbps']), 0)),
        ("loss% med", lambda r: fnum(r['loss_rate_pct_med'])),
        ("loss% min..max", lambda r: f"{r['loss_rate_pct_min']:.2f}..{r['loss_rate_pct_max']:.2f}"),
        ("drain MB/s", lambda r: fnum(r['drain_rate_mbps_med'], 0)),
    ])

    # E6 message size
    e6 = group_median(load('e6_msg_size.csv'), ['msg_size'])
    e6.sort(key=lambda r: int(r['msg_size']))
    table("E6 — message size / fragmentation (8 writers, 150 MB/s)", e6, [
        ("msg B", lambda r: r['msg_size']),
        ("loss% med", lambda r: fnum(r['loss_rate_pct_med'])),
        ("drain MB/s", lambda r: fnum(r['drain_rate_mbps_med'], 0)),
        ("write ns/msg", lambda r: fnum(r['write_ns_per_msg_med'], 0)),
        ("peak occ%", lambda r: fnum(r['peak_occ_pct_med'], 0)),
    ])


if __name__ == '__main__':
    main()
