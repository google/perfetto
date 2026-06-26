#!/usr/bin/env python3
# RFC rerun — the three charts the data-requests doc asks for, from the
# consolidated rfc_sessions.csv (duplication model). Written into task-2/charts/.
# Re-runnable any time; skips a chart cleanly if its data isn't in yet.
#   python3 plots.py [path/to/rfc_rerun]
import csv, os, sys, math
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import LogLocator, FuncFormatter, MultipleLocator

RFC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..")
CSV = os.path.join(RFC, "results", "rfc_sessions.csv")
CHARTS = os.path.abspath(os.path.join(RFC, "..", "charts"))
os.makedirs(CHARTS, exist_ok=True)

COND_STYLE = {  # color + label, in legend order
    "idle":        ("#2e7d32", "idle (control)"),
    "cold_start":  ("#1565c0", "cold app start"),
    "dex2oat":     ("#e08e0b", "heavy compile load"),
    "real_boot":   ("#c0392b", "real boot"),
    "first_unlock":("#7b1fa2", "first unlock"),
}


def load():
    if not os.path.exists(CSV):
        return []
    with open(CSV) as f:
        return list(csv.DictReader(f))


def sel(rows, sweep, **kw):
    out = []
    for r in rows:
        if r["sweep"] != sweep:
            continue
        if all(str(r[k]) == str(v) for k, v in kw.items()):
            out.append(r)
    return out


def ymax(v):  # floor tiny values so log scale shows them
    return max(float(v), 1e-4)


def _human(y):  # 0.0002 -> "0.0002", 2 -> "2", 20 -> "20"
    return f"{y:g}"


def _minor_log_label(y, _):  # label only the 2x and 5x minor ticks per decade
    if y <= 0:
        return ""
    mant = y / 10 ** math.floor(math.log10(y) + 1e-9)
    return _human(y) if (abs(mant - 2) < 0.1 or abs(mant - 5) < 0.1) else ""


def style_logy(ax):  # denser, fully-labelled log y axis
    ax.yaxis.set_major_locator(LogLocator(base=10, numticks=15))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: _human(y)))
    ax.yaxis.set_minor_locator(LogLocator(base=10, subs=(2, 3, 4, 5, 6, 7, 8, 9), numticks=15))
    ax.yaxis.set_minor_formatter(FuncFormatter(_minor_log_label))
    ax.tick_params(axis="y", which="minor", labelsize=7)


def style_linx(ax, major=25, minor=5):  # denser linear x axis (MB/s)
    ax.xaxis.set_major_locator(MultipleLocator(major))
    ax.xaxis.set_minor_locator(MultipleLocator(minor))


def chart_sessions(rows):
    fig, ax = plt.subplots(figsize=(7.5, 5))
    any_data = False
    for cond, (col, lbl) in COND_STYLE.items():
        pts = sorted(sel(rows, "1a", condition=cond, smb_kb=512, reader_nice=0),
                     key=lambda r: int(r["n_sessions"]))
        if not pts:
            continue
        any_data = True
        xs = [float(r["in_rate_mbps"]) for r in pts]
        ys = [ymax(r["loss_pct_median"]) for r in pts]
        ax.plot(xs, ys, "o-", color=col, label=lbl, lw=2)
    if not any_data:
        plt.close(fig); return None
    ax.set_yscale("log")
    ax.axhline(0.1, ls=":", color="grey", lw=1); ax.text(14, 0.115, "0.1% loss", color="grey", fontsize=8)
    ax.axhline(1.0, ls="--", color="grey", lw=1); ax.text(14, 1.15, "1% loss", color="grey", fontsize=8)
    ax.axvspan(19.6, 32.6, color="#2e8b57", alpha=0.12)
    ax.text(26, ax.get_ylim()[0] * 3, "de-bundling\n~20-33 MB/s", ha="center", color="#2e8b57", fontsize=8)
    ax.set_xlabel("SMB write rate (MB/s)")
    ax.set_ylabel("data lost (%, log)")
    ax.set_title("How much ftrace MB/s the SMB absorbs, by device condition\n(512 KB; de-bundling itself adds only ~20-33 MB/s, shaded)")
    style_logy(ax); style_linx(ax, 25, 5)
    ax.legend(); ax.grid(True, which="both", alpha=0.25)
    fig.tight_layout()
    out = os.path.join(CHARTS, "sessions_under_load.png"); fig.savefig(out, dpi=110); plt.close(fig)
    return out


def chart_buffer(rows, N=10):
    fig, ax = plt.subplots(figsize=(7.5, 5))
    any_data = False
    for cond, (col, lbl) in COND_STYLE.items():
        pts = sorted(sel(rows, "1b", condition=cond, n_sessions=N, reader_nice=0),
                     key=lambda r: int(r["smb_kb"]))
        if not pts:
            continue
        any_data = True
        xs = [int(r["smb_kb"]) for r in pts]
        ys = [ymax(r["loss_pct_median"]) for r in pts]
        ax.plot(xs, ys, "o-", color=col, label=lbl, lw=2)
    if not any_data:
        plt.close(fig); return None
    ax.set_yscale("log"); ax.set_xscale("log", base=2)
    ax.set_xticks([256, 512, 1024, 2048, 4096]); ax.set_xticklabels(["256K", "512K", "1M", "2M", "4M"])
    ax.axhline(1.0, ls="--", color="grey", lw=1)
    ax.set_xlabel("SMB size"); ax.set_ylabel("data lost (%, log)")
    ax.set_title(f"Does a bigger SMB help? loss vs buffer size at ~130 MB/s (10x the real rate)\n(by condition; flat = reader starved, memory can't help)")
    style_logy(ax)
    ax.legend(); ax.grid(True, which="both", alpha=0.25)
    fig.tight_layout()
    out = os.path.join(CHARTS, "buffer_vs_condition.png"); fig.savefig(out, dpi=110); plt.close(fig)
    return out


def chart_nicefix(rows):
    heavy = [c for c in ("dex2oat", "real_boot", "first_unlock")
             if sel(rows, "1d", condition=c)]
    if not heavy:
        return None
    fig, axes = plt.subplots(1, len(heavy), figsize=(5 * len(heavy), 4.4), sharey=True, squeeze=False)
    for ax, cond in zip(axes[0], heavy):
        for nice, col, lbl in [(0, "#c0392b", "nice 0"), (-10, "#2e7d32", "nice −10")]:
            pts = sorted(sel(rows, "1d", condition=cond, smb_kb=512, reader_nice=nice),
                         key=lambda r: int(r["n_sessions"]))
            if not pts:
                continue
            xs = [float(r["in_rate_mbps"]) for r in pts]
            ys = [ymax(r["loss_pct_median"]) for r in pts]
            ax.plot(xs, ys, "o-", color=col, label=lbl, lw=2)
        ax.set_yscale("log"); ax.axhline(1.0, ls="--", color="grey", lw=1)
        ax.set_title(COND_STYLE[cond][1]); ax.set_xlabel("SMB write rate (MB/s)")
        style_logy(ax); style_linx(ax, 25, 5)
        ax.grid(True, which="both", alpha=0.25); ax.legend()
    axes[0][0].set_ylabel("data lost (%, log)")
    fig.suptitle("Cutting boot loss by lowering the reader's nice (0 vs −10), faithful model, 512 KB")
    fig.tight_layout()
    out = os.path.join(CHARTS, "nice_fix_faithful.png"); fig.savefig(out, dpi=110); plt.close(fig)
    return out


def main():
    rows = load()
    made = [f for f in (chart_sessions(rows), chart_buffer(rows), chart_nicefix(rows)) if f]
    print("[plots] wrote:", ", ".join(os.path.basename(m) for m in made) if made else "nothing yet")


if __name__ == "__main__":
    main()
