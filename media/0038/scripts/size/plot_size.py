#!/usr/bin/env python3
"""Task-1 size charts (there was no committed generator before) + absolute-size chart.
Reads the per-trace size JSONs (task1_size.sh output, each wrapped in '===JSON BEGIN===')
and compress_sweep.json. Generic trace labels. Clarity pass.

Usage: plot_size.py <size_json_dir> <compress_sweep.json> <out_dir>
Outputs: growth_vs_sched, growth_by_category, byte_mix, per_class_amplification,
         size_absolute  (all .png in out_dir)
"""
import sys, os, json, glob
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SIZE_DIR, SWEEP, OUT = sys.argv[1], sys.argv[2], sys.argv[3]

GEN = {"aot": "always-on", "aot_boot": "always-on boot", "cold_start": "cold start",
       "first_unlock": "after unlock", "sys_random": "random 30s",
       "battery_long": "24h sparse"}
CAT_ORDER = ["aot", "aot_boot", "cold_start", "first_unlock", "sys_random", "battery_long"]
CAT_COLOR = {"aot": "#1f77b4", "aot_boot": "#9467bd", "cold_start": "#e8853a",
             "first_unlock": "#cc2222", "sys_random": "#2e8b57", "battery_long": "#888888"}


def load_size(p):
    t = open(p).read()
    i = t.find("{")
    return json.JSONDecoder().raw_decode(t[i:])[0]


recs = []
for p in sorted(glob.glob(os.path.join(SIZE_DIR, "*.json"))):
    if "ALL_JSON" in p:
        continue
    d = load_size(p)
    lbl = d["label"]                       # e.g. "first_unlock/t1"
    cat, t = lbl.split("/")
    sc = d["scenarios"]
    today = sc["all_ftrace"]["today_MB"]
    print_mb = today - sc["excl_print"]["today_MB"]
    sched_mb = sc["excl_print"]["today_MB"] - sc["excl_print_and_sched"]["today_MB"]
    other_mb = sc["excl_print_and_sched"]["today_MB"]
    recs.append(dict(
        cat=cat, label=f"{GEN[cat]}/{t}", short=GEN[cat],
        sched_pct=d["byte_share"]["compact"],
        growth=sc["all_ftrace"]["growth_data_only"],
        growth_pkt=sc["all_ftrace"]["growth_with_pkt_framing"],
        amp_sched=d["amplification"].get("compact") or 0.0,
        amp_other=d["amplification"].get("individual") or 0.0,
        today_MB=today, debundled_MB=sc["all_ftrace"]["debundled_data_only_MB"],
        print_mb=print_mb, sched_mb=sched_mb, other_mb=other_mb,
    ))
recs.sort(key=lambda r: (CAT_ORDER.index(r["cat"]), r["label"]))

sweep = {r["label"]: r for r in json.load(open(SWEEP))}


def lblpos():
    return [r["label"] for r in recs], range(len(recs)), [CAT_COLOR[r["cat"]] for r in recs]


# ---- 1. growth vs sched share (the mechanism) ----
fig, ax = plt.subplots(figsize=(9, 5.2))
xs = [r["sched_pct"] for r in recs]; ys = [r["growth"] for r in recs]
import numpy as np
a, b = np.polyfit(xs, ys, 1)
xx = np.linspace(0, max(xs) * 1.1, 50)
ax.plot(xx, a * xx + b, "--", color="#444", lw=1,
        label=f"fit: growth ≈ {b:.2f} + {a:.3f}·sched%")
seen = set()
for r in recs:
    ax.scatter(r["sched_pct"], r["growth"], s=70, color=CAT_COLOR[r["cat"]],
               label=r["short"] if r["cat"] not in seen else None, zorder=3)
    seen.add(r["cat"])
ax.set_xlabel("scheduler share of ftrace bytes (%)")
ax.set_ylabel("all-ftrace de-bundling growth (×, data-only)")
ax.set_title("Only scheduler data is expensive to de-bundle:\ngrowth is a straight line in the sched byte-share, nothing else moves it")
ax.legend(fontsize=8); ax.grid(alpha=0.3)
fig.tight_layout(); fig.savefig(f"{OUT}/growth_vs_sched.png", dpi=130); plt.close(fig)

# ---- 2. growth by trace ----
fig, ax = plt.subplots(figsize=(10, 4.6))
labels, x, cols = lblpos()
ax.bar(list(x), [r["growth"] for r in recs], color=cols, label="data-only")
ax.bar(list(x), [r["growth_pkt"] - r["growth"] for r in recs], bottom=[r["growth"] for r in recs],
       color=cols, alpha=0.4)
ax.axhline(1.0, color="k", lw=0.8, ls=":")
for i, r in enumerate(recs):
    ax.text(i, r["growth_pkt"] + 0.02, f"{r['growth']:.2f}×", ha="center", fontsize=7)
ax.set_xticks(list(x)); ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
ax.set_ylabel("de-bundling growth (×)")
ax.set_title("De-bundling growth by trace (solid = data-only, faded = incl. packet framing)")
ax.grid(True, axis="y", alpha=0.3)
fig.tight_layout(); fig.savefig(f"{OUT}/growth_by_category.png", dpi=130); plt.close(fig)

# ---- 3. byte mix (today composition: print / sched / other) ----
fig, ax = plt.subplots(figsize=(10, 4.6))
labels, x, _ = lblpos()
pr = [r["print_mb"] for r in recs]; sc = [r["sched_mb"] for r in recs]; ot = [r["other_mb"] for r in recs]
ax.bar(list(x), pr, color="#1f77b4", label="atrace print")
ax.bar(list(x), sc, bottom=pr, color="#cc2222", label="scheduler")
ax.bar(list(x), ot, bottom=[p + s for p, s in zip(pr, sc)], color="#aaaaaa", label="other kernel")
ax.set_xticks(list(x)); ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
ax.set_ylabel("today ftrace bytes (MB)")
ax.set_title("What each trace is made of (today): print vs scheduler vs other\n"
             "the scheduler slice is the only part that gets expensive to de-bundle")
ax.legend(fontsize=8); ax.grid(True, axis="y", alpha=0.3)
fig.tight_layout(); fig.savefig(f"{OUT}/byte_mix.png", dpi=130); plt.close(fig)

# ---- 4. per-class amplification ----
fig, ax = plt.subplots(figsize=(10, 4.6))
labels, x, _ = lblpos()
w = 0.4
ax.bar([i - w / 2 for i in x], [r["amp_sched"] for r in recs], w, color="#cc2222", label="scheduler (CompactSched)")
ax.bar([i + w / 2 for i in x], [r["amp_other"] for r in recs], w, color="#aaaaaa", label="everything else")
ax.axhline(1.0, color="k", lw=0.8, ls=":")
ax.set_xticks(list(x)); ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
ax.set_ylabel("de-bundling amplification (×)")
ax.set_title("Per-class de-bundling cost: scheduler ~3.5-4.7×, everything else ~1.07×")
ax.legend(fontsize=8); ax.grid(True, axis="y", alpha=0.3)
fig.tight_layout(); fig.savefig(f"{OUT}/per_class_amplification.png", dpi=130); plt.close(fig)

# ---- 5a. on-the-wire size (the §1 star chart): today bundled vs v2 de-bundled ----
fig, ax = plt.subplots(figsize=(10, 4.8))
labels, x, _ = lblpos()
w = 0.4
ax.bar([i - w / 2 for i in x], [r["today_MB"] for r in recs], w, color="#1f77b4", label="today (bundled)")
ax.bar([i + w / 2 for i in x], [r["debundled_MB"] for r in recs], w, color="#cc2222", label="v2 (de-bundled)")
for i, r in enumerate(recs):
    ax.text(i + w / 2, r["debundled_MB"] + 1, f"{r['debundled_MB']:.0f}", ha="center", fontsize=6.5, color="#cc2222")
ax.set_xticks(list(x)); ax.set_xticklabels(labels, rotation=40, ha="right", fontsize=8)
ax.set_ylabel("on-the-wire ftrace (MB, uncompressed)")
ax.set_title("Today (bundled) vs v2 (de-bundled): the SMB cost, in MB")
ax.legend(fontsize=9); ax.grid(True, axis="y", alpha=0.3)
fig.tight_layout(); fig.savefig(f"{OUT}/size_onwire.png", dpi=130); plt.close(fig)

# ---- 5b. stored (compressed) size for Part 3: today gzip vs v2 zstd-6 ----
fig, ax = plt.subplots(figsize=(10, 4.8))
tg, vz = [], []
for r in recs:
    t = r["label"].split("/")[-1]            # "t1"/"t2"
    s = sweep[f"{r['cat']}/{t}"]             # compress_sweep is keyed by original label
    tg.append(s["today_gzip6_MB"]); vz.append(s["configs"]["zstd-6"]["MB"])
ax.bar([i - w / 2 for i in x], tg, w, color="#1f77b4", label="today (bundled + gzip)")
ax.bar([i + w / 2 for i in x], vz, w, color="#2e8b57", label="v2 (de-bundled + zstd-6)")
ax.set_xticks(list(x)); ax.set_xticklabels(labels, rotation=40, ha="right", fontsize=8)
ax.set_ylabel("stored / uploaded ftrace (MB, compressed)")
ax.set_title("Final uploaded size, in MB: v2 (de-bundled + zstd-6) ≤ today (bundled + gzip)")
ax.legend(fontsize=9); ax.grid(True, axis="y", alpha=0.3)
fig.tight_layout(); fig.savefig(f"{OUT}/size_stored.png", dpi=130); plt.close(fig)

print("wrote: growth_vs_sched, growth_by_category, byte_mix, per_class_amplification, size_onwire, size_stored")
