#!/usr/bin/env python3
"""Charts for the controlled cold-start sweep (run_cold.sh).
Usage: plot_cold.py <cold_dir>

  CM_multiplier.png  loss vs write rate during a real cold-start (Study M), median
                     + min/max band — the clean "does 5x break it" curve.
  CS_smb.png         loss vs SMB size during the cold-start at 5x (Study S).
"""
import csv, os, sys, statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

D = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = os.path.join(D, 'charts'); os.makedirs(OUT, exist_ok=True)
rows = []
p = os.path.join(D, 'cold.csv')
if os.path.exists(p):
    for r in csv.DictReader(open(p)):
        try:
            rows.append({'study': r['study'], 'mult': float(r['mult_o']), 'smb': int(r['smb_kb_o']),
                         'loss': float(r['loss_rate_pct']), 'inr': float(r['in_rate_mbps']),
                         'drain': float(r['drain_rate_mbps']), 'starved': float(r['reader_wait_pct'])})
        except (KeyError, ValueError):
            pass


def agg(sel, key, col):
    g = {}
    for x in rows:
        if sel(x):
            g.setdefault(x[key], []).append(x[col])
    return g


def chart_multiplier():
    g = agg(lambda x: x['study'] == 'M', 'mult', 'loss')
    gi = agg(lambda x: x['study'] == 'M', 'mult', 'inr')
    if not g:
        return
    mults = sorted(g)
    xs = [statistics.median(gi[m]) for m in mults]
    md = [statistics.median(g[m]) for m in mults]
    lo = [min(g[m]) for m in mults]; hi = [max(g[m]) for m in mults]
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.fill_between(xs, lo, hi, color='#cc2222', alpha=.15, label='run-to-run range')
    ax.plot(xs, md, '-o', color='#cc2222', lw=2, label='median loss')
    for m, x, y in zip(mults, xs, md):
        ax.annotate(f'{m:.0f}×', (x, y), fontsize=8, xytext=(4, 4), textcoords='offset points')
    ax.set_xlabel('data written to the SMB  (MB/s)  →  more  (1× = the real boot rate)')
    ax.set_ylabel('trace data lost  (%)')
    ax.set_title('During a REAL cold-app-start (controlled trigger, repeatable):\n'
                 'how much extra write load the SMB takes before it drops')
    ax.legend(fontsize=9); ax.grid(True, alpha=.3)
    fig.tight_layout(); fig.savefig(os.path.join(OUT, 'CM_multiplier.png'), dpi=130)
    plt.close(fig); print('wrote CM_multiplier.png')


def chart_smb():
    g = agg(lambda x: x['study'] == 'S', 'smb', 'loss')
    if not g:
        return
    smbs = sorted(g)
    xs = [s / 1024 for s in smbs]
    md = [statistics.median(g[s]) for s in smbs]
    lo = [min(g[s]) for s in smbs]; hi = [max(g[s]) for s in smbs]
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.fill_between(xs, lo, hi, color='#2a72c8', alpha=.15, label='run-to-run range')
    ax.plot(xs, md, '-o', color='#2a72c8', lw=2, label='median loss')
    ax.set_xscale('log', base=2)
    ax.set_xlabel('SMB size  (MB)  →  bigger buffer')
    ax.set_ylabel('trace data lost  (%)')
    ax.set_title('Bigger buffer during a real cold-start storm (5× write load)')
    ax.legend(fontsize=9); ax.grid(True, alpha=.3, which='both')
    fig.tight_layout(); fig.savefig(os.path.join(OUT, 'CS_smb.png'), dpi=130)
    plt.close(fig); print('wrote CS_smb.png')


if __name__ == '__main__':
    if not rows:
        print('no rows', p); sys.exit(0)
    chart_multiplier(); chart_smb()
    # table
    g = agg(lambda x: x['study'] == 'M', 'mult', 'loss')
    gs = agg(lambda x: x['study'] == 'M', 'mult', 'starved')
    print("\ncold-start Study M — loss% (median[n], range), starved%:")
    for m in sorted(g):
        v = g[m]; print(f"  {m:.0f}×: {statistics.median(v):.2f}% [n={len(v)}] ({min(v):.1f}-{max(v):.1f})  starved={statistics.median(gs[m]):.0f}%")
