#!/usr/bin/env python3
"""Render the Task-2 SMB device-stress charts (PNG) from the collected CSVs.

Inputs (in ../results):
  device_sweep.csv  idle/dex2oat/appstorm/combined x multiplier (the 2-D surface)
  real_events.csv   real_boot / first_unlock x multiplier (run during a reboot)
  host_smoke.csv    the host x86 bench (for the host-vs-device contrast)

Charts (../charts):
  1 loss_vs_rate.png   loss% vs in-rate, one line per scenario — the money chart
  2 breakpoint.png     where each scenario starts losing (bar) — margin shrinks with realism
  3 starvation.png     reader starvation vs scenario — why loss appears (the cause)
"""
import csv
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'results')
OUT = os.path.join(HERE, '..', 'charts')
os.makedirs(OUT, exist_ok=True)


def load(name):
  p = os.path.join(RES, name)
  return list(csv.DictReader(open(p))) if os.path.exists(p) else []


def series(rows, label_prefix, xcol, ycol, mult_max=None):
  pts = []
  for r in rows:
    if not r['label'].startswith(label_prefix):
      continue
    m = float(r.get('multiplier', 0))
    if mult_max and m > mult_max:
      continue
    pts.append((float(r[xcol]), float(r[ycol])))
  pts.sort()
  return pts


# colours: greyscale host, warm = synthetic load, red = real events
STYLE = {
    'host':      ('#999999', 's', '-',  'host x86 (free fast core)'),
    'idle':      ('#2a72c8', 'o', '-',  'device, idle'),
    'combined':  ('#e8853a', '^', '-',  'device, synthetic load (dex2oat+apps)'),
    'real_boot': ('#cc2222', 'D', '-',  'during a REAL boot'),
    'firstunlock_real': ('#8e44ad', 'v', '-', 'during a REAL first-unlock'),
}


def chart_loss_vs_rate():
  dev = load('device_sweep.csv')
  real = load('real_events.csv')
  host = load('host_smoke.csv')
  fig, ax = plt.subplots(figsize=(8, 5))

  def plot(pts, key):
    if not pts:
      return
    c, mk, ls, lab = STYLE[key]
    xs, ys = zip(*pts)
    ax.plot(xs, ys, ls, color=c, marker=mk, label=lab, alpha=.9, linewidth=2)

  plot(series(dev, 'idle', 'in_rate_mbps', 'loss_rate_pct'), 'idle')
  plot(series(dev, 'combined', 'in_rate_mbps', 'loss_rate_pct'), 'combined')
  # real events: csv has scenario,multiplier,...,in_mbps,loss_rate_pct
  rb = sorted((float(r['in_mbps']), float(r['loss_rate_pct']))
              for r in real if r['scenario'] == 'real_boot')
  fu = sorted((float(r['in_mbps']), float(r['loss_rate_pct']))
              for r in real if r['scenario'] == 'first_unlock')
  if rb:
    xs, ys = zip(*rb); ax.plot(xs, ys, '-', color='#cc2222', marker='D',
                               label=STYLE['real_boot'][3], linewidth=2)
  if fu:
    xs, ys = zip(*fu); ax.plot(xs, ys, '-', color='#8e44ad', marker='v',
                               label=STYLE['firstunlock_real'][3], linewidth=2)

  ax.axvspan(12, 94, color='#dddddd', alpha=.5, zorder=0)
  ax.text(16, ax.get_ylim()[1]*0.0+2, '1 session\n(~13 MB/s)',
          fontsize=8, color='#555555')
  ax.set_xlabel('SMB write rate (MB/s)  ≈  concurrent sessions × ~13 MB/s  →  more')
  ax.set_ylabel('trace data lost  (%)  →  worse')
  ax.set_title('Time-warp model (secondary): the busier the device,\n'
               'the sooner the reader starves and drops — see text for why this is pessimistic')
  ax.set_xlim(0, 190)
  ax.legend(fontsize=8, loc='upper left')
  ax.grid(True, alpha=.3)
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, '1_loss_vs_rate.png'), dpi=130)
  print('wrote 1_loss_vs_rate.png')


def first_break(pts, thresh=1.0):
  """First x where y crosses thresh (linear-interp); else None."""
  prev = None
  for x, y in pts:
    if y >= thresh:
      if prev and prev[1] < thresh and y != prev[1]:
        x0, y0 = prev
        return x0 + (x - x0) * (thresh - y0) / (y - y0)
      return x
    prev = (x, y)
  return None


def chart_breakpoint():
  dev = load('device_sweep.csv')
  real = load('real_events.csv')
  host = load('host_smoke.csv')
  bars = []  # (label, break_in_rate, colour)
  for load_name, col in [('idle', '#2a72c8'), ('dex2oat', '#3aa0c8'),
                         ('appstorm', '#e8b53a'), ('combined', '#e8853a')]:
    bars.append((f'device {load_name}',
                 first_break(series(dev, load_name, 'in_rate_mbps', 'loss_rate_pct')), col))
  for scen, nice, col in [('real_boot', 'REAL boot', '#cc2222'),
                          ('first_unlock', 'REAL first-unlock', '#8e44ad')]:
    pts = sorted((float(r['in_mbps']), float(r['loss_rate_pct']))
                 for r in real if r['scenario'] == scen)
    bars.append((nice, first_break(pts), col))
  bars = [(l, v, c) for l, v, c in bars if v]
  fig, ax = plt.subplots(figsize=(8, 4.5))
  labels = [b[0] for b in bars]
  vals = [b[1] for b in bars]
  cols = [b[2] for b in bars]
  ax.barh(range(len(bars)), vals, color=cols)
  ax.set_yticks(range(len(bars)))
  ax.set_yticklabels(labels, fontsize=9)
  ax.invert_yaxis()
  for i, v in enumerate(vals):
    ax.text(v + 5, i, f'{v:.0f} MB/s', va='center', fontsize=8)
  ax.axvspan(0, 13, color='#dddddd', alpha=.5, zorder=0)
  ax.text(2, len(bars)-0.3, '1 session', fontsize=8, color='#555')
  ax.set_xlabel('rate it can take before losing data  (MB/s)  ≈  sessions × ~13 MB/s')
  ax.set_title('Time-warp model (secondary): where each condition starts dropping\n'
               '(a busy device starves the reader, so its ceiling — and session headroom — falls)')
  ax.grid(True, axis='x', alpha=.3)
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, '2_breakpoint.png'), dpi=130)
  print('wrote 2_breakpoint.png')


def chart_starvation():
  """Same write rate (5x), three environments. Holding the data rate fixed,
  show that as the device gets busier the reader starves more and drops more —
  so the loss is caused by the reader losing the CPU, not by the data rate."""
  dev = load('device_sweep.csv')
  real = load('real_events.csv')

  def at5(rows, prefix):
    for r in rows:
      if r['label'].startswith(prefix) and abs(float(r['multiplier']) - 5) < 0.01:
        return float(r['reader_wait_pct']), float(r['loss_rate_pct'])
    return None

  def at5_real(scen):
    for r in real:
      if r['scenario'] == scen and abs(float(r['multiplier']) - 5) < 0.01:
        return float(r['reader_wait_pct']), float(r['loss_rate_pct'])
    return None

  cases = [('idle', at5(dev, 'idle'), '#2a72c8'),
           ('synthetic load\n(dex2oat+apps)', at5(dev, 'combined'), '#e8853a'),
           ('a REAL boot', at5_real('real_boot'), '#cc2222')]
  cases = [(n, v, c) for n, v, c in cases if v]
  labels = [c[0] for c in cases]
  starved = [c[1][0] for c in cases]
  loss = [c[1][1] for c in cases]
  cols = [c[2] for c in cases]

  fig, ax = plt.subplots(figsize=(8, 4.8))
  x = range(len(cases))
  w = 0.36
  b1 = ax.bar([i - w/2 for i in x], starved, w, color='#bbbbbb',
              label='reader stuck waiting for a CPU (%)')
  b2 = ax.bar([i + w/2 for i in x], loss, w, color=cols,
              label='trace data lost (%)')
  ax.set_xticks(list(x))
  ax.set_xticklabels(labels, fontsize=9)
  for i in x:
    ax.text(i - w/2, starved[i] + 1, f'{starved[i]:.0f}%', ha='center', fontsize=8, color='#666')
    ax.text(i + w/2, loss[i] + 1, f'{loss[i]:.0f}%', ha='center', fontsize=8, color=cols[i])
  ax.set_ylabel('% (both bars)')
  ax.set_title('Same data rate (5×), three environments:\n'
               'the busier the device, the more the reader starves — and the more it drops')
  ax.legend(fontsize=8, loc='upper left')
  ax.grid(True, axis='y', alpha=.3)
  fig.tight_layout()
  fig.savefig(os.path.join(OUT, '3_starvation.png'), dpi=130)
  print('wrote 3_starvation.png')


if __name__ == '__main__':
  chart_loss_vs_rate()
  chart_breakpoint()
  chart_starvation()
  print('charts ->', OUT)
