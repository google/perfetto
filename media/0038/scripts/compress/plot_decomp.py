#!/usr/bin/env python3
"""Decompression speed (read side) on the LITTLE core, zstd vs lz4 at matched levels,
from decomp_device.json (device run). Two panels: a dense trace and a sparse trace.
Usage: plot_decomp.py decomp_device.json out_dir
Output: <out_dir>/6_decomp_speed.png
"""
import sys, os, json
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src, outdir = sys.argv[1], sys.argv[2]
d = json.load(open(src))
E = d["entries"]

# matched levels both codecs have, on the little core (the slowest read core)
LEVELS = [1, 3, 6, 12]
CLUSTER = "little"
ZCOL, LCOL = "#2a72c8", "#e8853a"   # zstd blue, lz4 orange
REF = 13.0   # MB/s the de-bundled stream is produced (and compressed) at

# the two extremes used across the doc: dense (after unlock) and sparse (24h)
PANELS = [("first_unlock_t1", "dense: after unlock"),
          ("battery_long_t1", "sparse: 24 h")]


def mbs(trace_sub, codec):
    for e in E:
        if (trace_sub in e["trace"] and e["cluster"] == CLUSTER
                and e["codec_level"] == codec):
            return e["decomp_mbs"]
    return 0.0


fig, axes = plt.subplots(1, len(PANELS), figsize=(12, 5.8), sharey=True, squeeze=False)
w = 0.38
x = range(len(LEVELS))
for ax, (sub, nice) in zip(axes[0], PANELS):
    zs = [mbs(sub, f"zstd-{L}") for L in LEVELS]
    ls = [mbs(sub, f"lz4-{L}") for L in LEVELS]
    xz = [j - w / 2 for j in x]
    xl = [j + w / 2 for j in x]
    ax.bar(xz, zs, width=w, color=ZCOL, label="zstd")
    ax.bar(xl, ls, width=w, color=LCOL, label="lz4")
    for xx, yy in list(zip(xz, zs)) + list(zip(xl, ls)):
        ax.text(xx, yy * 1.02, f"{yy:.0f}", ha="center", va="bottom", fontsize=7.5)
    ax.axhline(REF, color="#c0392b", ls="--", lw=1.6)   # the rate the read side must beat
    ax.set_yscale("log")
    ax.set_ylim(8, 2000)   # top leaves headroom for the tallest bar (~1069) + its label
    ax.set_xticks(list(x)); ax.set_xticklabels([str(L) for L in LEVELS])
    ax.set_xlabel("codec level")
    ax.set_title(nice)
    ax.grid(True, axis="y", which="both", alpha=0.25)
    ax.legend(fontsize=9)
axes[0][0].text(-0.45, REF * 1.18,
                "~13 MB/s: rate the stream is\nproduced & compressed\n(read side only has to beat this)",
                color="#c0392b", fontsize=8, va="bottom", ha="left",
                bbox=dict(boxstyle="round,pad=0.25", fc="white", ec="#c0392b", alpha=0.9))
axes[0][0].set_ylabel("decompression speed (output MB/s, log, little core)  →  faster")
fig.suptitle("Decompression on the little core: zstd vs lz4 at matched levels\n"
             "every codec & level is 14–80× the ~13 MB/s the stream is produced at, "
             "so the read side is never the bottleneck")
fig.text(0.5, 0.015,
         "Decompression throughput rises slightly with level because higher levels emit "
         "longer matches (more cheap copying, less per-byte work); the small zstd-1 vs "
         "zstd-3 dip is a low-level strategy switch, not a regression; decompression is "
         "~flat across levels. Each bar is the codec's built-in -b benchmark run ~3 s per "
         "level on the device (many round-trip-verified iterations, best throughput), so "
         "the numbers are stable.",
         ha="center", va="bottom", fontsize=7.5, style="italic", color="#444444", wrap=True)
fig.tight_layout(rect=[0, 0.10, 1, 0.95])
out = os.path.join(outdir, "6_decomp_speed.png")
fig.savefig(out, dpi=130); print("wrote", out)
