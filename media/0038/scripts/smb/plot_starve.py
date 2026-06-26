#!/usr/bin/env python3
"""Charts for the controlled-starvation SMB grid (synth_starve.sh).
Usage: plot_starve.py <starve_dir>   (reads starve.csv, writes charts/)

  S1_smb_curves_<mult>.png  loss vs SMB size, one line per starvation level (clean)
  S2_heatmap_<mult>.png     loss over (starvation level × SMB size)
  S3_starve_check.png       measured reader-starvation% vs busy-loop count (knob works)
"""
import csv, os, sys, statistics, collections
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

D = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = os.path.join(D, 'charts'); os.makedirs(OUT, exist_ok=True)
# the grid file is either starve.csv (busy-loop-worker knob) or nice.csv (reader-nice knob)
# NB: the busy-loop knob spawns N busy-loop *threads* on cpus 0-5 (NOT cores; the
# device has 8). It got cgroup-throttled (reader_wait flat ~19% at every level), so
# it's an UNRELIABLE starvation proxy — prefer the reader-nice knob (nice.csv).
p = os.path.join(D, 'starve.csv')
LEVELCOL, LEVELNAME = 'starve_n', 'busy-loop workers'
if not os.path.exists(p):
    p = os.path.join(D, 'nice.csv'); LEVELCOL, LEVELNAME = 'reader_nice', 'reader nice'
rows = []
if os.path.exists(p):
    for r in csv.DictReader(open(p)):
        try:
            rows.append({'n': int(r[LEVELCOL]), 'smb': int(r['smb_kb_o']),
                         'mult': float(r['mult_o']), 'loss': float(r['loss_rate_pct']),
                         'starved': float(r['reader_wait_pct'])})
        except (KeyError, ValueError):
            pass


def med(n, smb, mult, col='loss'):
    v = [x[col] for x in rows if x['n'] == n and x['smb'] == smb and x['mult'] == mult]
    return statistics.median(v) if v else None


def smb_curves(mult):
    ns = sorted({x['n'] for x in rows if x['mult'] == mult})
    smbs = sorted({x['smb'] for x in rows if x['mult'] == mult})
    if not ns or not smbs:
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    cmap = plt.cm.plasma
    for i, n in enumerate(ns):
        ys = [med(n, s, mult) for s in smbs]
        xs = [s / 1024 for s, y in zip(smbs, ys) if y is not None]
        yy = [y for y in ys if y is not None]
        if xs:
            ax.plot(xs, yy, '-o', color=cmap(i / max(1, len(ns) - 1)),
                    label=f'{LEVELNAME}={n}', lw=2)
    ax.set_xscale('log', base=2)
    ax.set_xlabel('SMB size  (MB)  →  bigger buffer')
    ax.set_ylabel('trace data lost  (%)')
    ax.set_title(f'A bigger buffer helps less as the reader is starved more\n'
                 f'(controlled load, in-rate = {mult:.0f}× the boot rate)')
    ax.legend(fontsize=8, title='reader starvation')
    ax.grid(True, alpha=.3, which='both')
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f'S1_smb_curves_{mult:.0f}x.png'), dpi=130)
    plt.close(fig); print(f'wrote S1_smb_curves_{mult:.0f}x.png')


def heatmap(mult):
    ns = sorted({x['n'] for x in rows if x['mult'] == mult})
    smbs = sorted({x['smb'] for x in rows if x['mult'] == mult})
    if len(ns) < 2 or len(smbs) < 2:
        return
    M = np.full((len(ns), len(smbs)), np.nan)
    for i, n in enumerate(ns):
        for j, s in enumerate(smbs):
            v = med(n, s, mult)
            if v is not None:
                M[i, j] = v
    fig, ax = plt.subplots(figsize=(8, 5))
    im = ax.imshow(M, aspect='auto', origin='lower', cmap='RdYlGn_r', vmin=0, vmax=30)
    ax.set_xticks(range(len(smbs))); ax.set_xticklabels([f'{s//1024}M' if s >= 1024 else f'{s}K' for s in smbs])
    ax.set_yticks(range(len(ns))); ax.set_yticklabels([f'{n}' for n in ns])
    ax.set_xlabel('SMB size'); ax.set_ylabel(LEVELNAME)
    ax.set_title(f'Data lost across SMB size × reader starvation ({mult:.0f}× rate)')
    for i in range(len(ns)):
        for j in range(len(smbs)):
            if not np.isnan(M[i, j]):
                ax.text(j, i, f'{M[i,j]:.1f}', ha='center', va='center', fontsize=8,
                        color='black' if M[i, j] < 18 else 'white')
    fig.colorbar(im, label='trace data lost (%)')
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f'S2_heatmap_{mult:.0f}x.png'), dpi=130)
    plt.close(fig); print(f'wrote S2_heatmap_{mult:.0f}x.png')


def starve_check():
    ns = sorted({x['n'] for x in rows})
    if not ns:
        return
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ys = [statistics.median([x['starved'] for x in rows if x['n'] == n]) for n in ns]
    ax.plot(ns, ys, '-o', color='#cc2222', lw=2)
    ax.set_xlabel(LEVELNAME)
    ax.set_ylabel('reader stuck waiting for a CPU  (%)')
    ax.set_title('busy-loop knob check: did more workers starve the reader?'
                 '  (NB: cgroup-throttled → flat ~19%, knob unreliable)')
    ax.grid(True, alpha=.3)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, 'S3_starve_check.png'), dpi=130)
    plt.close(fig); print('wrote S3_starve_check.png')


if __name__ == '__main__':
    if not rows:
        print('no rows in', p); sys.exit(0)
    for m in sorted({x['mult'] for x in rows}):
        smb_curves(m); heatmap(m)
    starve_check()
    print('charts ->', OUT)
