#!/usr/bin/env python3
"""Task 02 — duplication vs time-warp load model: medians + chart.

Reads results/dup_*/dup.csv (mode,nmult,smb_kb,rep,block,<harness cols...>) and
emits median tables + a 2-panel chart (loss-vs-rate, buffer-sweep)."""
import sys, os, csv, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = sys.argv[1] if len(sys.argv) > 1 else None
if not OUT:
    cands = sorted([d for d in os.listdir("results") if d.startswith("dup_")])
    OUT = os.path.join("results", cands[-1])
csvp = os.path.join(OUT, "dup.csv")

rows = list(csv.DictReader(open(csvp)))
def f(r, k):
    try: return float(r[k])
    except: return None
# harness col names are in the header after the 5 prepended ones
def med(mode, block, key, **eq):
    vals = []
    for r in rows:
        if r["mode"] != mode or r["block"] != block: continue
        if any(r[k] != str(v) for k, v in eq.items()): continue
        v = f(r, key)
        if v is not None: vals.append(v)
    return statistics.median(vals) if vals else float("nan")

# ---- Block A: loss vs N @512K ----
MULTS = [1, 3, 5, 7, 10, 14]
print("=== BLOCK A: loss vs N @512K (median) ===")
print(f"{'N':>4} {'in(MB/s)':>9} | {'warp loss%':>10} {'occ%':>6} | {'dup loss%':>10} {'occ%':>6}")
A = {"warp": {"in": [], "loss": []}, "dup": {"in": [], "loss": []}}
for m in MULTS:
    wi = med("warp", "A", "in_rate_mbps", nmult=m)
    wl = med("warp", "A", "loss_rate_pct", nmult=m)
    wo = med("warp", "A", "peak_occ_pct", nmult=m)
    di = med("dup", "A", "in_rate_mbps", nmult=m)
    dl = med("dup", "A", "loss_rate_pct", nmult=m)
    do = med("dup", "A", "peak_occ_pct", nmult=m)
    print(f"{str(m)+'x':>4} {wi:>9.1f} | {wl:>10.4f} {wo:>6.1f} | {dl:>10.4f} {do:>6.1f}")
    A["warp"]["in"].append(wi); A["warp"]["loss"].append(wl)
    A["dup"]["in"].append(di); A["dup"]["loss"].append(dl)

# ---- Block B: buffer sweep @10x ----
SMBS = [256, 512, 1024, 2048, 4096]
print("\n=== BLOCK B: buffer sweep @10x (median loss%) ===")
print(f"{'SMB':>7} | {'warp':>9} {'dup':>9}")
B = {"warp": [], "dup": []}
for s in SMBS:
    wl = med("warp", "B", "loss_rate_pct", smb_kb=s)
    dl = med("dup", "B", "loss_rate_pct", smb_kb=s)
    print(f"{str(s)+'K':>7} | {wl:>9.4f} {dl:>9.4f}")
    B["warp"].append(wl); B["dup"].append(dl)

# ---- chart ----
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))
ax1.plot(A["warp"]["in"], A["warp"]["loss"], "o-", color="#c0392b", label="time-warp = faster stream (wrong model, pessimistic)")
ax1.plot(A["dup"]["in"], A["dup"]["loss"], "s-", color="#2471a3", label="duplication = N concurrent sessions (faithful)")
ax1.axvline(125, ls=":", color="gray"); ax1.text(126, ax1.get_ylim()[1]*0.5, "healthy reader\nceiling ~125", fontsize=8, color="gray")
# annotate session counts at each duplication point (in-rate / ~13 MB/s/session)
for xin, yl, n in zip(A["dup"]["in"], A["dup"]["loss"], MULTS):
    if xin and xin == xin:
        ax1.annotate(f"{n}", (xin, yl), textcoords="offset points", xytext=(0, 6),
                     fontsize=8, color="#2471a3", ha="center")
ax1.set_xlabel("SMB write rate (MB/s)  =  concurrent sessions × ~13 MB/s"); ax1.set_ylabel("loss %")
ax1.set_title("How many concurrent sessions one SMB takes (idle, 512 KB)\n"
              "duplication is the faithful N-sessions model; time-warp overstates loss")
ax1.legend(fontsize=8); ax1.grid(alpha=0.3)

x = range(len(SMBS))
ax2.plot(x, B["warp"], "o-", color="#c0392b", label="time-warp @10× (sustained 130 > ceiling)")
ax2.plot(x, B["dup"], "s-", color="#2471a3", label="duplication @10× (bursty, the realistic shape)")
ax2.set_xticks(list(x)); ax2.set_xticklabels([f"{s}K" if s < 1024 else f"{s//1024}M" for s in SMBS])
ax2.set_xlabel("SMB size"); ax2.set_ylabel("loss %")
ax2.set_title("Block B — does a bigger buffer help?\n(fixed 10×, idle, nice 0)")
ax2.legend(fontsize=9); ax2.grid(alpha=0.3)
fig.tight_layout()
chart = os.path.join("charts", "K_dup_vs_warp.png")
fig.savefig(chart, dpi=110)
print(f"\nchart -> {chart}")
