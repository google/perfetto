#!/usr/bin/env python3
"""Aggregate the realistic-reader sweeps (r1..r4) into median tables (by column
name, so it survives CSV column changes). Usage: analyze_realistic.py [results]"""
import csv, os, statistics, sys, collections

R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'results')


def load(n):
    p = os.path.join(R, n)
    return list(csv.DictReader(open(p))) if os.path.exists(p) else []


def grp(rows, key):
    g = collections.defaultdict(list)
    for r in rows:
        g[r[key]].append(r)
    return g


def med(rows, col):
    return statistics.median(float(r[col]) for r in rows)


def mx(rows, col):
    return max(float(r[col]) for r in rows)


def show(title, rows, keyfn, cols):
    print(f"\n### {title}\n")
    print("| " + " | ".join(c for c, _ in cols) + " |")
    print("|" + "|".join("---" for _ in cols) + "|")
    for k in sorted(rows, key=keyfn):
        rs = rows[k]
        print("| " + " | ".join(f(k, rs) for _, f in cols) + " |")


# R1 — loss vs in-rate
g = grp(load('r1_inrate.csv'), 'rate_target_mbps')
show("R1 — loss vs in-rate (8 writers, 512 KB, realistic reader)", g,
     lambda k: float(k), [
         ("in-rate tgt MB/s", lambda k, rs: f"{float(k):.0f}"),
         ("in MB/s", lambda k, rs: f"{med(rs,'in_rate_mbps'):.0f}"),
         ("drain MB/s", lambda k, rs: f"{med(rs,'drain_rate_mbps'):.0f}"),
         ("loss% med", lambda k, rs: f"{med(rs,'loss_rate_pct'):.3f}"),
         ("loss% max", lambda k, rs: f"{mx(rs,'loss_rate_pct'):.3f}"),
         ("peak occ%", lambda k, rs: f"{med(rs,'peak_occ_pct'):.0f}"),
     ])

# R2 — loss vs writers
g = grp(load('r2_writers.csv'), 'label')
show("R2 — loss vs writer count (realistic reader)", g,
     lambda k: (int(k.split('_w')[1]), int(k.split('rate')[1].split('_')[0])), [
         ("config", lambda k, rs: k),
         ("writers", lambda k, rs: k.split('_w')[1]),
         ("loss% med", lambda k, rs: f"{med(rs,'loss_rate_pct'):.3f}"),
         ("loss% max", lambda k, rs: f"{mx(rs,'loss_rate_pct'):.3f}"),
         ("drain MB/s", lambda k, rs: f"{med(rs,'drain_rate_mbps'):.0f}"),
     ])

# R3 — wake interval robustness
g = grp(load('r3_wake.csv'), 'wake_ms')
show("R3 — robustness to flush interval, fill-trigger ON (200 MB/s)", g,
     lambda k: float(k), [
         ("wake ms", lambda k, rs: f"{float(k):.0f}"),
         ("loss% med", lambda k, rs: f"{med(rs,'loss_rate_pct'):.3f}"),
         ("peak occ%", lambda k, rs: f"{med(rs,'peak_occ_pct'):.0f}"),
     ])

# R4 — operating points
g = grp(load('r4_operating.csv'), 'label')
order = {'firehose_steady_30': 0, 'firehose_steady_50': 1, 'atrace_sdk_100': 2,
         'sched_burst_300': 3, 'sched_burst_500': 4}
show("R4 — realistic de-bundling operating points (8 writers, 512 KB)", g,
     lambda k: order.get(k, 9), [
         ("scenario", lambda k, rs: k),
         ("in MB/s", lambda k, rs: f"{med(rs,'in_rate_mbps'):.0f}"),
         ("loss% med", lambda k, rs: f"{med(rs,'loss_rate_pct'):.3f}"),
         ("peak occ%", lambda k, rs: f"{med(rs,'peak_occ_pct'):.0f}"),
     ])
