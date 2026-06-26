#!/usr/bin/env python3
"""Exec-summary hero: three panels over the SAME five representative traces, so it
reads as one story left-to-right: (1) bigger on the wire, (2) ~no extra loss at the
real rate, (3) back to ~today after zstd-3. Output: ../findings_hero.png"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = os.path.join(os.path.dirname(__file__), "..", "findings_hero.png")
GREEN, ORANGE, GREY, RED = "#2e8b57", "#e8853a", "#888888", "#c0392b"

# one trace of each type, consistent order across all panels (sorted by de-bundle x)
LABELS   = ["24h sparse", "always-on", "always-on\nboot", "first\nunlock", "random\n30s"]
DEBUNDLE = [0.94, 1.43, 1.44, 1.75, 1.83]           # raw on-wire x today
LOSS     = [0.0002, 0.001, 0.12, 0.001, 0.001]      # SMB loss % at the real rate, by scenario
ZSTD3    = [0.646, 0.935, 0.974, 1.147, 1.096]      # uploaded x today, zstd-3
x = range(len(LABELS))


def color_vs1(vals):           # below today = green, above = orange
    return [GREEN if v <= 1.0 else ORANGE for v in vals]


fig, (a1, a2, a3) = plt.subplots(1, 3, figsize=(15, 5.2))
fig.suptitle("The firehose, end to end (five representative traces)\n"
             "bigger on the wire, near-0 loss at the real rate (a real boot the "
             "exception), back to ~today after zstd-3", fontsize=12.5, fontweight="bold")

# --- Panel 1: de-bundling size increase ---
a1.bar(x, DEBUNDLE, color=color_vs1(DEBUNDLE))
a1.axhline(1.0, color="k", ls="--", lw=1.3)
a1.text(len(x) - 0.5, 1.02, "today", ha="right", va="bottom", fontsize=8, fontweight="bold")
for i, v in zip(x, DEBUNDLE):
    a1.text(i, v + 0.03, f"{v:.2f}×", ha="center", fontsize=8.5, fontweight="bold")
a1.set_ylim(0, 2.1)
a1.set_title("(1) De-bundling: bigger on the wire\n(raw size into the SMB)", fontsize=10.5)
a1.set_ylabel("size ÷ today")
a1.set_xticks(list(x)); a1.set_xticklabels(LABELS, fontsize=8)
a1.grid(True, axis="y", alpha=.25)

# --- Panel 2: data loss at the real rate (log) ---
loss_colors = [ORANGE if v >= 0.01 else GREEN for v in LOSS]   # boot flagged as the exception
a2.bar(x, LOSS, color=loss_colors)
a2.set_yscale("log")
a2.set_ylim(0.0001, 1.0)
for i, v in zip(x, LOSS):
    a2.text(i, v * 1.3, f"{v:g}%", ha="center", fontsize=8.5, fontweight="bold")
a2.set_title("(2) Data loss at the real rate\n(~0 everywhere except a real boot)",
             fontsize=10.5)
a2.set_ylabel("SMB data loss (%, log)")
a2.set_xticks(list(x)); a2.set_xticklabels(LABELS, fontsize=8)
a2.grid(True, axis="y", which="both", alpha=.25)
a2.text(2.05, 0.012, "boot is the exception: replaying it through the SMB\nduring a boot "
        "drops 0.12% even at the real rate (starved\nreader), rising to ~0.1–0.5% under "
        "de-bundling", ha="center", va="center", fontsize=7, color=RED)

# --- Panel 3: uploaded size after zstd-3 vs today ---
a3.bar(x, ZSTD3, color=color_vs1(ZSTD3))
a3.axhline(1.0, color="k", ls="--", lw=1.3)
a3.text(len(x) - 0.5, 1.02, "today", ha="right", va="bottom", fontsize=8, fontweight="bold")
for i, v in zip(x, ZSTD3):
    a3.text(i, v + 0.02, f"{v:.2f}×", ha="center", fontsize=8.5, fontweight="bold")
a3.set_ylim(0, 1.35)
a3.set_title("(3) After zstd-3 in traced: back to ~today\n(cheap default; zstd-6 goes lower)",
             fontsize=10.5)
a3.set_ylabel("uploaded size ÷ today")
a3.set_xticks(list(x)); a3.set_xticklabels(LABELS, fontsize=8)
a3.grid(True, axis="y", alpha=.25)

fig.text(0.5, 0.01,
         "Data loss (panel 2) is the median of 3 device reps per scenario, on a Pixel Fold.",
         ha="center", va="bottom", fontsize=7.5, style="italic", color="#444444")
fig.tight_layout(rect=[0, 0.05, 1, 0.92])
fig.savefig(OUT, dpi=130)
print("wrote", os.path.abspath(OUT))
