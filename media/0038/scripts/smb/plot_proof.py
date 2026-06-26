#!/usr/bin/env python3
"""The drain-ceiling proof chart (proof_sweep.sh).
Usage: plot_proof.py <proof_dir>

  P1_drain_ceiling.png   loss vs in-rate, one line per reader-health level,
                         with each level's drain ceiling marked — the proof that
                         loss lifts off exactly when in-rate crosses the ceiling.
"""
import csv, os, sys, statistics, collections
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

D = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = os.path.join(D, 'charts'); os.makedirs(OUT, exist_ok=True)
rows = []
p = os.path.join(D, 'proof.csv')
if os.path.exists(p):
    for r in csv.DictReader(open(p)):
        try:
            rows.append({'nice': int(r['reader_nice']), 'mult': float(r['mult_o']),
                         'loss': float(r['loss_rate_pct']), 'inr': float(r['in_rate_mbps']),
                         'drain': float(r['drain_rate_mbps'])})
        except (KeyError, ValueError):
            pass

NICE_LABEL = {0: 'reader healthy (nice 0)', 5: 'reader mildly squeezed (nice 5)',
              10: 'reader starved (nice 10)'}
COL = {0: '#2a72c8', 5: '#b54fc0', 10: '#cc2222'}


def med(nice, mult, col):
    v = [x[col] for x in rows if x['nice'] == nice and x['mult'] == mult]
    return statistics.median(v) if v else None


def ceiling(nice):
    """drain ceiling = max sustained drain (the drain at the highest multiplier,
    where the reader is overloaded and drain saturates)."""
    mults = sorted({x['mult'] for x in rows if x['nice'] == nice})
    return med(nice, mults[-1], 'drain') if mults else None


def chart():
    nices = sorted({x['nice'] for x in rows})
    if not nices:
        return
    fig, ax = plt.subplots(figsize=(8.5, 5.5))
    for nice in nices:
        mults = sorted({x['mult'] for x in rows if x['nice'] == nice})
        xs = [med(nice, m, 'inr') for m in mults]
        ys = [med(nice, m, 'loss') for m in mults]
        c = COL.get(nice, '#333')
        ax.plot(xs, ys, '-o', color=c, lw=2, label=NICE_LABEL.get(nice, f'nice {nice}'))
        cap = ceiling(nice)
        if cap:
            ax.axvline(cap, color=c, ls=':', lw=1.4, alpha=.8)
            ax.text(cap, ax.get_ylim()[1]*0.92, f' drain ceiling\n ≈{cap:.0f} MB/s',
                    color=c, fontsize=7.5, va='top')
    ax.set_xlabel('data written to the SMB  (MB/s)  →  more  (1×→10× the boot rate)')
    ax.set_ylabel('trace data lost  (%)')
    ax.set_title('Loss lifts off exactly when the rate crosses the reader\'s drain ceiling —\n'
                 'and starvation drops the ceiling, so the same 5× breaks a starved reader but not a healthy one')
    ax.legend(fontsize=9)
    ax.grid(True, alpha=.3)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, 'P1_drain_ceiling.png'), dpi=130)
    plt.close(fig); print('wrote P1_drain_ceiling.png')


if __name__ == '__main__':
    if not rows:
        print('no rows', p); sys.exit(0)
    chart()
    # also dump the proof table
    print("\nreader-health | drain ceiling | loss @1x | @5x | @10x")
    for nice in sorted({x['nice'] for x in rows}):
        print(f"  nice {nice:2}: ceil≈{ceiling(nice):.0f} MB/s | "
              f"{med(nice,1,'loss'):.2f}% | {med(nice,5,'loss'):.2f}% | {med(nice,10,'loss'):.2f}%")
