#!/usr/bin/env python3
"""Render the Task-3 compression charts (PNG) from the collected JSON.

Inputs (any subset; charts that lack data are skipped):
  --device data/cost_device.json   (run_device.py: per-core comp/decomp MB/s, RSS)
  --host   data/cost_host.json     (cost_bench host runs, list of result objects)
  --ratio  data/ratio_sweep.json   (compress_sweep.py: ratio/level/block/window, 12 traces)
  --out    data/charts

Charts:
  1 pareto_big.png   ratio vs compression speed on the Pixel big core (the money chart)
  2 speed_cores.png  compression MB/s vs level, per core (little/mid/big), core spread
  3 ratio_level.png  compression ratio vs level, per codec (host sweep, dense trace)
  4 block_size.png   zstd ratio vs block size (the per-flush block lever)
  5 vs_today.png     stored size vs today (gzip) per trace, chosen configs
"""
import argparse
import json
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

COL = {'gzip': '#888888', 'lz4': '#e8853a', 'zstd': '#2a72c8'}
MARK = {'gzip': 's', 'lz4': '^', 'zstd': 'o'}


_TRACE_NAMES = [
    ('aot_boot', 'always-on boot'), ('aot', 'always-on'),
    ('cold_start', 'cold start'), ('first_unlock', 'after unlock'),
    ('sys_random', 'random 30s'), ('battery_long', '24h sparse'),
]
def clean_label(s):
  if '.v2.pftrace' in s:
    s = s.split('/')[-1].replace('.v2.pftrace', '').replace('_t', '/t')
  for _k, _v in _TRACE_NAMES:
    if s.startswith(_k):
      return _v + s[len(_k):]
  return s


def codec_of(cfg):
  return cfg.split('-')[0]


def level_of(cfg):
  p = cfg.split('-')
  try:
    return int(p[1])
  except (IndexError, ValueError):
    return None


def load(p):
  return json.load(open(p)) if p and os.path.exists(p) else None


CORE_NICE = {'little': 'little (A55)', 'mid': 'mid (A78)', 'big': 'big (X1)'}


def chart_pareto(device, outdir, cluster, fname, note=''):
  recs = [d for d in device if d.get('cluster') == cluster]
  if not recs:
    return
  # use the densest trace (largest in_b) for the headline scatter
  d = max(recs, key=lambda x: x.get('in_b') or 0)
  fig, ax = plt.subplots(figsize=(7.5, 5))
  for codec in ('gzip', 'lz4', 'zstd'):
    pts = [(r['c_mbs'], r['ratio'], level_of(r['cfg']))
           for r in d['rows'] if codec_of(r['cfg']) == codec
           and 'long' not in r['cfg'] and r['c_mbs'] and r['ratio']]
    pts.sort()
    if not pts:
      continue
    xs, ys, lv = zip(*pts)
    ax.plot(xs, ys, '-', color=COL[codec], marker=MARK[codec], label=codec, alpha=.85)
    for x, y, l in pts:
      ax.annotate(f'{l}', (x, y), fontsize=7, color=COL[codec],
                  xytext=(3, 3), textcoords='offset points')
  ax.set_xscale('log')
  ax.set_xlabel(f'compression speed (MB/s, Pixel {CORE_NICE.get(cluster, cluster)} core)'
                '  →  faster')
  ax.set_ylabel('compression ratio  →  smaller file')
  ax.set_title(f"Ratio vs speed on the phone, {clean_label(d['label'])}, "
               f"{cluster} core{note}\n(up-and-right is better; number = codec level)")
  ax.grid(True, which='both', alpha=.25)
  ax.legend()
  fig.tight_layout(); fig.savefig(f'{outdir}/{fname}', dpi=130); plt.close(fig)


def chart_speed_cores(device, outdir):
  order = {'little': 0, 'mid': 1, 'big': 2}
  clusters = sorted({d['cluster'] for d in device if d.get('cluster')},
                    key=lambda c: order.get(c, 9))
  if not clusters:
    return
  d0 = max((d for d in device), key=lambda x: x.get('in_b') or 0)
  label = d0['label']; clabel = clean_label(label)
  fig, ax = plt.subplots(figsize=(7.5, 5))
  styles = {'little': ':', 'mid': '--', 'big': '-'}
  for cl in clusters:
    rec = next((d for d in device if d['cluster'] == cl and d['label'] == label), None)
    if not rec:
      continue
    pts = [(level_of(r['cfg']), r['c_mbs']) for r in rec['rows']
           if codec_of(r['cfg']) == 'zstd' and 'long' not in r['cfg'] and r['c_mbs']]
    pts = sorted(p for p in pts if p[0] is not None)
    if pts:
      xs, ys = zip(*pts)
      ax.plot(xs, ys, styles.get(cl, '-'), color=COL['zstd'], marker='o',
              label=f'zstd, {cl} core')
  ax.set_yscale('log')
  ax.set_xlabel('zstd level'); ax.set_ylabel('compression speed (MB/s, log)')
  ax.set_title(f"Cost is which CORE, not which level, {clabel}")
  ax.grid(True, which='both', alpha=.25); ax.legend()
  fig.tight_layout(); fig.savefig(f'{outdir}/2_speed_cores.png', dpi=130); plt.close(fig)


def chart_ratio_level(ratio, outdir):
  if not ratio:
    return
  # dense representative trace
  d = next((r for r in ratio if r['label'] == 'first_unlock/t1'), ratio[0])
  cfgs = d['configs']
  fig, ax = plt.subplots(figsize=(7.5, 5))
  for codec in ('gzip', 'lz4', 'zstd'):
    pts = []
    for k, v in cfgs.items():
      if codec_of(k) == codec and '@' not in k and 'long' not in k and v['ratio']:
        lv = level_of(k)
        if lv is not None:
          pts.append((lv, v['ratio']))
    pts.sort()
    if pts:
      xs, ys = zip(*pts)
      ax.plot(xs, ys, color=COL[codec], marker=MARK[codec], label=codec)
  ax.set_xlabel('codec level'); ax.set_ylabel('compression ratio (in/out)')
  ax.set_title(f"Ratio vs level, {clean_label(d['label'])}\n(zstd-3..6 is the knee; more level buys little)")
  ax.grid(True, alpha=.25); ax.legend()
  fig.tight_layout(); fig.savefig(f'{outdir}/3_ratio_level.png', dpi=130); plt.close(fig)


def chart_block_size(ratio, outdir):
  if not ratio:
    return
  d = next((r for r in ratio if r['label'] == 'first_unlock/t1'), ratio[0])
  order = ['512K', '1M', '2M', '4M', 'full']
  fig, ax = plt.subplots(figsize=(7.5, 5))
  for L in (3, 12):
    pts = []
    for b in order:
      v = d['configs'].get(f'zstd-{L}@{b}')
      if v and v['ratio']:
        pts.append((b, v['ratio']))
    if pts:
      xs, ys = zip(*pts)
      ax.plot(range(len(xs)), ys, marker='o', label=f'zstd-{L}')
      ax.set_xticks(range(len(order))); ax.set_xticklabels(order)
  ax.set_xlabel('compression block size (independent per-flush blocks)')
  ax.set_ylabel('compression ratio (in/out)')
  ax.set_title(f"Bigger blocks compress better, {clean_label(d['label'])}")
  ax.grid(True, alpha=.25); ax.legend()
  fig.tight_layout(); fig.savefig(f'{outdir}/4_block_size.png', dpi=130); plt.close(fig)


def chart_vs_today(ratio, outdir):
  if not ratio:
    return
  # the star: gzip (today's codec) + lz4's fast & best levels + zstd-3/-6, all
  # measured as v2 de-bundled + codec vs today's bundled + gzip (so de-bundling's
  # size cost is already included). Codec-grouped colours: grey gzip, orange lz4,
  # blue zstd.
  series = [
      ('gzip-6', 'de-bundled + gzip-6 (same codec)', '#888888'),
      ('lz4-1', 'de-bundled + lz4-1 (fast)', '#f0b27a'),
      ('lz4-12', 'de-bundled + lz4-12 (best lz4)', '#d35400'),
      ('zstd-3', 'de-bundled + zstd-3 (cheap default)', '#5dade2'),
      ('zstd-6', 'de-bundled + zstd-6 (more shrink)', '#1f618d'),
  ]
  labels = [clean_label(r['label']) for r in ratio]
  n = len(series)
  fig, ax = plt.subplots(figsize=(11.5, 5.6))
  w = 0.16
  for i, (c, nice, col) in enumerate(series):
    ys = [r['configs'].get(c, {}).get('vs_today') for r in ratio]
    xs = [x + (i - (n - 1) / 2) * w for x in range(len(labels))]
    ax.bar(xs, [y or 0 for y in ys], width=w, label=nice, color=col)
  ax.axhline(1.0, color='k', lw=1.6, ls='--')
  ax.text(len(labels) - 0.4, 1.03, "CURRENT = today (bundled + gzip-6) = 1.0", ha='right',
          va='bottom', fontsize=8.5, fontweight='bold')
  ax.set_xticks(range(len(labels)))
  ax.set_xticklabels(labels, rotation=40, ha='right', fontsize=8)
  ax.set_ylabel('final uploaded size ÷ today\n(below 1.0 = smaller than today)')
  ax.set_title("Final uploaded size vs today, all 12 traces (lower = smaller)\n"
               "dashed line = CURRENT shipped size (bundled + gzip-6)   |   "
               "every BAR = v2 DE-BUNDLED stream + a codec\n"
               "only zstd drops below today; de-bundled + gzip-6 and even lz4-12 stay bigger")
  ax.grid(True, axis='y', alpha=.25)
  ax.legend(ncol=3, fontsize=8, loc='upper center', bbox_to_anchor=(0.5, -0.26))
  fig.text(0.5, -0.02,
           "Measured on the host (Linux); compression ratio is architecture-independent, "
           "so identical on the device. Sizes are whole-stream.",
           ha='center', va='top', fontsize=7.5, style='italic', color='#444444', wrap=True)
  fig.tight_layout(); fig.savefig(f'{outdir}/5_vs_today.png', dpi=130, bbox_inches='tight')
  plt.close(fig)


# ---- overlay variants (multiple traces per chart) ------------------------------
def _dev_rec(device, sub, cluster):
  return next((d for d in device if sub in d['label'] and d.get('cluster') == cluster),
              None)


def _ratio_rec(ratio, label):
  return next((r for r in ratio if r['label'] == label), None)


# (substring, nice name, linestyle, marker), dense vs sparse extremes
OVL_DEVICE = [('first_unlock_t1', 'dense (after unlock)', '-', 'o'),
              ('battery_long_t1', 'sparse (24h)', ':', 's')]
OVL_RATIO = [('first_unlock/t1', 'dense (after unlock)', '-', 'o'),
             ('battery_long/t1', 'sparse (24h)', ':', 's')]


def chart_pareto_overlay(device, outdir):
  fig, ax = plt.subplots(figsize=(8, 5.5))
  for sub, nice, ls, mk in OVL_DEVICE:
    d = _dev_rec(device, sub, 'little')
    if not d:
      continue
    for codec in ('gzip', 'lz4', 'zstd'):
      pts = sorted((r['c_mbs'], r['ratio'], level_of(r['cfg'])) for r in d['rows']
                   if codec_of(r['cfg']) == codec and 'long' not in r['cfg']
                   and r['c_mbs'] and r['ratio'])
      if not pts:
        continue
      xs, ys, _ = zip(*pts)
      ax.plot(xs, ys, ls=ls, color=COL[codec], marker=mk, ms=4, alpha=.85,
              label=f'{codec}, {nice}')
      if sub == 'first_unlock_t1' and codec == 'zstd':
        for x, y, l in pts:
          ax.annotate(f'{l}', (x, y), fontsize=6.5, color=COL['zstd'],
                      xytext=(3, 3), textcoords='offset points')
  ax.set_xscale('log')
  ax.set_xlabel('compression speed (MB/s, Pixel little core)  →  faster')
  ax.set_ylabel('compression ratio  →  smaller file')
  ax.set_title("Ratio vs speed on the little core, dense vs sparse\n"
               "(zstd above-and-right of gzip & lz4 in both bands; "
               "sparse compresses ~2× better)")
  ax.grid(True, which='both', alpha=.25)
  ax.legend(fontsize=7, ncol=3)
  fig.tight_layout(); fig.savefig(f'{outdir}/1_pareto_little.png', dpi=130)
  plt.close(fig)


def chart_ratio_level_overlay(ratio, outdir):
  fig, ax = plt.subplots(figsize=(8, 5.5))
  for label, nice, ls, mk in OVL_RATIO:
    d = _ratio_rec(ratio, label)
    if not d:
      continue
    for codec in ('gzip', 'lz4', 'zstd'):
      pts = sorted((level_of(k), v['ratio']) for k, v in d['configs'].items()
                   if codec_of(k) == codec and '@' not in k and 'long' not in k
                   and v['ratio'] and level_of(k) is not None)
      if pts:
        xs, ys = zip(*pts)
        ax.plot(xs, ys, ls=ls, color=COL[codec], marker=mk, ms=4,
                label=f'{codec}, {nice}')
  ax.set_xlabel('codec level'); ax.set_ylabel('compression ratio (in/out)')
  ax.set_title("Ratio vs level, dense vs sparse\n"
               "(same shape on both; zstd always on top, lz4 below gzip)")
  ax.grid(True, alpha=.25); ax.legend(fontsize=7, ncol=2)
  fig.tight_layout(); fig.savefig(f'{outdir}/3_ratio_level.png', dpi=130)
  plt.close(fig)


def chart_block_overlay(ratio, outdir):
  order = ['512K', '1M', '2M', '4M', 'full']
  fig, ax = plt.subplots(figsize=(8, 5.5))
  for label, nice, ls, mk in OVL_DEVICE_TO_RATIO:
    d = _ratio_rec(ratio, label)
    if not d:
      continue
    pts = [d['configs'].get(f'zstd-12@{b}', {}).get('ratio') for b in order]
    if all(p for p in pts):
      ax.plot(range(len(order)), pts, ls=ls, marker=mk, label=f'zstd-12, {nice}')
  ax.set_xticks(range(len(order))); ax.set_xticklabels(order)
  ax.set_xlabel('compression block size (independent per-flush blocks)')
  ax.set_ylabel('compression ratio (in/out)')
  ax.set_title("Bigger blocks compress better, across workloads (zstd-12)")
  ax.grid(True, alpha=.25); ax.legend(fontsize=8)
  fig.tight_layout(); fig.savefig(f'{outdir}/4_block_size.png', dpi=130)
  plt.close(fig)


OVL_DEVICE_TO_RATIO = [('first_unlock/t1', 'after unlock (busy, dense events)', '-', 'o'),
                       ('aot/t1', 'always-on (mixed load)', '--', 'D'),
                       ('battery_long/t1', '24h sparse (mostly idle)', ':', 's')]


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--device'); ap.add_argument('--host'); ap.add_argument('--ratio')
  ap.add_argument('--out', default='data/charts')
  ap.add_argument('--overlay', action='store_true',
                  help='multi-trace overlay variants (dense/mid/sparse)')
  args = ap.parse_args()
  os.makedirs(args.out, exist_ok=True)
  device, host, ratio = load(args.device), load(args.host), load(args.ratio)
  if args.overlay:
    if device:
      chart_pareto_overlay(device, args.out)
      chart_speed_cores(device, args.out)
      chart_pareto(device, args.out, 'big', '1b_pareto_big_reference.png',
                   note=' (REFERENCE, traced does not run here)')
    if ratio:
      chart_ratio_level_overlay(ratio, args.out)
      chart_block_overlay(ratio, args.out)
      chart_vs_today(ratio, args.out)
    print('overlay charts ->', args.out, sorted(os.listdir(args.out)))
    return
  if device:
    # traced runs on little+mid (task_profiles ProcessCapacityHigh), little is the
    # realistic floor (main chart); big is reference only (traced doesn't run there).
    chart_pareto(device, args.out, 'little', '1_pareto_little.png')
    chart_pareto(device, args.out, 'big', '1b_pareto_big_reference.png',
                 note=' (REFERENCE, traced does not run here)')
    chart_speed_cores(device, args.out)
  if ratio:
    chart_ratio_level(ratio, args.out); chart_block_size(ratio, args.out)
    chart_vs_today(ratio, args.out)
  print('charts ->', args.out, sorted(os.listdir(args.out)))


if __name__ == '__main__':
  main()
