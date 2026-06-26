#!/usr/bin/env python3
# Plot the Task-9 rung-1 result: does a strong negative nice close the
# starvation loss? loss vs write-multiplier + reader runqueue-wait, one curve
# per reader-nice level, under the real combined device load.
#   python3 plot_niceboost.py results/niceboost_YYYYMMDD_HHMMSS
import csv, sys, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

outdir = sys.argv[1]
rows = list(csv.DictReader(open(f"{outdir}/nice_boost.csv")))
NICES = ["0", "-10", "-20"]
COL = {"0": "#c0392b", "-10": "#e08e0b", "-20": "#2e7d32"}
LBL = {"0": "nice 0 (baseline)", "-10": "nice −10", "-20": "nice −20"}

def med(load, nice, mult, col):
    v = [float(r[col]) for r in rows
         if r["load"] == load and r["reader_nice"] == nice and r["mult"] == mult and r[col]]
    return st.median(v) if v else None

mults = sorted({r["mult"] for r in rows if r["load"] == "combined"}, key=int)
xinr = [med("combined", "0", m, "in_rate_mbps") for m in mults]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.6))

# left: loss vs multiplier (log y), one curve per nice
for n in NICES:
    y = [max(med("combined", n, m, "loss_rate_pct"), 1e-4) for m in mults]
    ax1.plot([int(m) for m in mults], y, "o-", color=COL[n], label=LBL[n], lw=2)
ax1.set_yscale("log")
ax1.axhline(1.0, ls=":", color="grey", lw=1)
ax1.text(int(mults[0]), 1.15, "1% loss", color="grey", fontsize=8)
ax1.set_xlabel("write multiplier  (1× = real boot rate ≈ 13 MB/s)")
ax1.set_ylabel("data lost (%, log)")
ax1.set_title("A strong negative nice closes the starvation loss\n(combined load: dex2oat + app-storm)")
ax1.set_xticks([int(m) for m in mults])
ax1.set_xticklabels([f"{m}×\n{int(i)}MB/s" for m, i in zip(mults, xinr)])
ax1.legend(); ax1.grid(True, which="both", alpha=0.25)

# right: reader runqueue-wait vs multiplier = the mechanism
for n in NICES:
    y = [med("combined", n, m, "reader_wait_pct") for m in mults]
    ax2.plot([int(m) for m in mults], y, "o-", color=COL[n], label=LBL[n], lw=2)
ax2.set_xlabel("write multiplier")
ax2.set_ylabel("reader runqueue-wait (% of time)")
ax2.set_title("...because it cuts how long the reader\nwaits for a CPU")
ax2.set_xticks([int(m) for m in mults])
ax2.set_xticklabels([f"{m}×" for m in mults])
ax2.legend(); ax2.grid(True, alpha=0.25)

fig.tight_layout()
out = f"{sys.argv[2] if len(sys.argv) > 2 else 'charts/J_nice_boost'}.png"
fig.savefig(out, dpi=110)
print("wrote", out)
