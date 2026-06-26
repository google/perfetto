#!/usr/bin/env python3
"""Charts for the autonomous SMB-size sweep (run_sweep.sh).

Usage: plot_sweep.py <sweep_dir>     (reads <dir>/sweep.csv, writes <dir>/charts/)

Charts (skipped if data is missing — works on a partial/in-progress sweep):
  A_smb_vs_loss_boot.png    loss vs SMB size, one line per multiplier (real boot)
  A_smb_vs_loss_unlock.png  same for first-unlock (app launch)
  B_min_smb.png             smallest SMB that keeps loss <1%, vs multiplier, per scenario
  C_heatmap_boot.png        loss% over the (SMB × multiplier) grid, real boot
  D_realism_512.png         at 512 KB: loss vs multiplier, idle vs synthetic vs real boot/unlock
"""
import csv
import os
import statistics
import sys
import collections
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

D = sys.argv[1] if len(sys.argv) > 1 else '.'
OUT = os.path.join(D, 'charts')
os.makedirs(OUT, exist_ok=True)

rows = []
p = os.path.join(D, 'sweep.csv')
if os.path.exists(p):
  for r in csv.DictReader(open(p)):
    try:
      rows.append({
          'scen': r['scenario'], 'smb': int(r['smb_kb_o']),
          'mult': float(r['mult_o']), 'loss': float(r['loss_rate_pct']),
          'occ': float(r['peak_occ_pct']), 'starved': float(r['reader_wait_pct']),
          'inr': float(r['in_rate_mbps']),
      })
    except (KeyError, ValueError):
      pass


def med(scen, smb, mult, col='loss'):
  vs = [x[col] for x in rows if x['scen'] == scen and x['smb'] == smb and x['mult'] == mult]
  return statistics.median(vs) if vs else None


def axes(scen):
  smbs = sorted({x['smb'] for x in rows if x['scen'] == scen})
  mults = sorted({x['mult'] for x in rows if x['scen'] == scen})
  return smbs, mults


COL = plt.cm.viridis


def chart_smb_vs_loss(scen, fname, title):
  smbs, mults = axes(scen)
  if not smbs or not mults:
    return
  fig, ax = plt.subplots(figsize=(8, 5))
  for i, m in enumerate(mults):
    ys = [med(scen, s, m) for s in smbs]
    xs = [s / 1024 for s, y in zip(smbs, ys) if y is not None]
    yy = [y for y in ys if y is not None]
    if not xs:
      continue
    ax.plot(xs, yy, '-o', color=COL(i / max(1, len(mults) - 1)), label=f'{m:.0f}× the boot rate')
  ax.axhline(1.0, color='#cc2222', ls='--', lw=1, alpha=.7)
  ax.text(ax.get_xlim()[1], 1.0, ' 1% loss', color='#cc2222', fontsize=8, va='bottom', ha='right')
  ax.set_xscale('log', base=2)
  ax.set_xlabel('SMB size  (MB)  →  bigger buffer')
  ax.set_ylabel('trace data lost  (%)')
  ax.set_title(title)
  ax.legend(fontsize=8, title='load')
  ax.grid(True, alpha=.3, which='both')
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, fname), dpi=130)
  plt.close(fig)
  print('wrote', fname)


def min_smb_for(scen, mult, thresh=1.0):
  smbs, _ = axes(scen)
  for s in smbs:
    v = med(scen, s, mult)
    if v is not None and v < thresh:
      return s / 1024
  return None


def chart_min_smb():
  scens = [s for s in ['boot', 'unlock_app', 'unlock_home'] if any(x['scen'] == s for x in rows)]
  if not scens:
    return
  fig, ax = plt.subplots(figsize=(8, 5))
  colors = {'boot': '#cc2222', 'unlock_app': '#8e44ad', 'unlock_home': '#2a72c8'}
  for scen in scens:
    _, mults = axes(scen)
    xs, ys = [], []
    for m in mults:
      v = min_smb_for(scen, m)
      if v is not None:
        xs.append(m); ys.append(v)
    if xs:
      ax.plot(xs, ys, '-o', color=colors.get(scen, '#333'), label=scen.replace('_', ' '))
  ax.set_xlabel('load  (× the real boot rate)')
  ax.set_ylabel('smallest SMB that keeps loss <1%  (MB)')
  ax.set_title('How big the buffer must be to survive N× at boot / unlock')
  ax.legend(fontsize=9)
  ax.grid(True, alpha=.3)
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, 'B_min_smb.png'), dpi=130)
  plt.close(fig)
  print('wrote B_min_smb.png')


def chart_heatmap(scen, fname, title):
  smbs, mults = axes(scen)
  if len(smbs) < 2 or len(mults) < 2:
    return
  M = np.full((len(mults), len(smbs)), np.nan)
  for i, m in enumerate(mults):
    for j, s in enumerate(smbs):
      v = med(scen, s, m)
      if v is not None:
        M[i, j] = v
  fig, ax = plt.subplots(figsize=(8, 5))
  im = ax.imshow(M, aspect='auto', origin='lower', cmap='RdYlGn_r', vmin=0, vmax=30)
  ax.set_xticks(range(len(smbs))); ax.set_xticklabels([f'{s//1024 if s>=1024 else s}{"M" if s>=1024 else "K"}' for s in smbs])
  ax.set_yticks(range(len(mults))); ax.set_yticklabels([f'{m:.0f}×' for m in mults])
  ax.set_xlabel('SMB size'); ax.set_ylabel('load (× boot rate)')
  ax.set_title(title)
  for i in range(len(mults)):
    for j in range(len(smbs)):
      if not np.isnan(M[i, j]):
        ax.text(j, i, f'{M[i,j]:.1f}', ha='center', va='center', fontsize=8,
                color='black' if M[i, j] < 18 else 'white')
  fig.colorbar(im, label='trace data lost (%)')
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, fname), dpi=130)
  plt.close(fig)
  print('wrote', fname)


def chart_realism_512():
  scens = [(s, c, lab) for s, c, lab in [
      ('idle', '#2a72c8', 'idle'), ('boot', '#cc2222', 'real boot'),
      ('unlock_app', '#8e44ad', 'real first-unlock (app)'),
      ('unlock_home', '#3aa0c8', 'real first-unlock (home)')]
      if any(x['scen'] == s and x['smb'] == 512 for x in rows)]
  if not scens:
    return
  fig, ax = plt.subplots(figsize=(8, 5))
  for s, c, lab in scens:
    mults = sorted({x['mult'] for x in rows if x['scen'] == s and x['smb'] == 512})
    xs = [m for m in mults if med(s, 512, m) is not None]
    ys = [med(s, 512, m) for m in xs]
    if xs:
      ax.plot(xs, ys, '-o', color=c, label=lab)
  ax.set_xlabel('load  (× the real boot rate)')
  ax.set_ylabel('trace data lost  (%)')
  ax.set_title('At a fixed 512 KB buffer: the more real the load, the sooner it drops')
  ax.legend(fontsize=9)
  ax.grid(True, alpha=.3)
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, 'D_realism_512.png'), dpi=130)
  plt.close(fig)
  print('wrote D_realism_512.png')


def chart_idle_smb():
  """The clean SMB-size signal: idle grid has no reboot/boot-timing noise."""
  smbs, mults = axes('idle')
  if not smbs:
    return
  fig, ax = plt.subplots(figsize=(8, 5))
  for i, m in enumerate(mults):
    ys = [med('idle', s, m) for s in smbs]
    xs = [s / 1024 for s, y in zip(smbs, ys) if y is not None]
    yy = [y for y in ys if y is not None]
    if xs:
      ax.plot(xs, yy, '-o', color=COL(i / max(1, len(mults) - 1)),
              label=f'{m:.0f}× the boot rate', lw=2)
  ax.set_xscale('log', base=2)
  ax.set_xlabel('SMB size  (MB)  →  bigger buffer')
  ax.set_ylabel('trace data lost  (%)')
  ax.set_title('A bigger buffer absorbs bursts (clean signal, device idle):\n'
               'it only matters once the rate outruns the reader (~10×)')
  ax.legend(fontsize=8, title='load')
  ax.grid(True, alpha=.3, which='both')
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, 'E_idle_smb.png'), dpi=130)
  plt.close(fig)
  print('wrote E_idle_smb.png')


if __name__ == '__main__':
  if not rows:
    print('no rows in', p); sys.exit(0)
  chart_idle_smb()
  chart_smb_vs_loss('boot', 'A_smb_vs_loss_boot.png', 'Real boot: bigger buffer vs data lost')
  chart_smb_vs_loss('unlock_app', 'A_smb_vs_loss_unlock.png', 'Real first-unlock: bigger buffer vs data lost')
  chart_min_smb()
  chart_heatmap('boot', 'C_heatmap_boot.png', 'Data lost across SMB size × load (real boot)')
  chart_heatmap('unlock_app', 'C_heatmap_unlock.png', 'Data lost across SMB size × load (real first-unlock)')
  chart_realism_512()
  print('charts ->', OUT)
