#!/usr/bin/env python3
# RFC rerun — aggregate the raw duplication sweeps into the deliverables the
# RFC data-requests doc asks for:
#   1. results/rfc_sessions.csv  — consolidated medians, exact requested columns.
#   2. SUMMARY.md                — median tables per sub-sweep (1a-1d) + a
#      per-condition "sessions before loss" crossing table.
# Re-runnable at any time (incremental): reads whatever is in rfc_raw.csv so far.
#
#   python3 aggregate.py [path/to/rfc_rerun]
import csv, sys, os, statistics as st

RFC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..")
RAW = os.path.join(RFC, "results", "rfc_raw.csv")
OUTCSV = os.path.join(RFC, "results", "rfc_sessions.csv")
SUMMARY = os.path.join(RFC, "SUMMARY.md")
COND_ORDER = ["idle", "cold_start", "dex2oat", "real_boot", "first_unlock"]


def p90(xs):
    s = sorted(xs)
    if not s:
        return None
    if len(s) == 1:
        return s[0]
    i = 0.9 * (len(s) - 1)
    lo = int(i)
    return s[lo] + (s[lo + 1] - s[lo]) * (i - lo)


def load():
    if not os.path.exists(RAW):
        return []
    rows = []
    with open(RAW) as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "sweep": r["sweep"], "condition": r["condition"],
                    "sessions": int(r["sessions"]), "smb": int(r["smb_set"]),
                    "nice": int(r["nice_set"]), "rep": r["rep"],
                    "loss": float(r["loss_rate_pct"]), "in": float(r["in_rate_mbps"]),
                    "wait": float(r["reader_wait_pct"]), "occ": float(r["peak_occ_pct"]),
                })
            except (KeyError, ValueError):
                continue
    return rows


def agg(rows):
    """group by (sweep,condition,sessions,smb,nice) -> aggregated medians."""
    g = {}
    for r in rows:
        g.setdefault((r["sweep"], r["condition"], r["sessions"], r["smb"], r["nice"]), []).append(r)
    out = {}
    for k, rs in g.items():
        loss = [r["loss"] for r in rs]
        out[k] = {
            "reps": len(rs), "loss_med": st.median(loss), "loss_p90": p90(loss),
            "in": st.median([r["in"] for r in rs]), "wait": st.median([r["wait"] for r in rs]),
            "occ": st.median([r["occ"] for r in rs]),
        }
    return out


def write_csv(a):
    with open(OUTCSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["model", "sweep", "condition", "n_sessions", "smb_kb", "reader_nice",
                    "reps", "in_rate_mbps", "reader_wait_pct", "peak_occ_pct",
                    "loss_pct_median", "loss_pct_p90"])
        for (sweep, cond, N, smb, nice) in sorted(a):
            v = a[(sweep, cond, N, smb, nice)]
            w.writerow(["dup", sweep, cond, N, smb, nice, v["reps"],
                        f"{v['in']:.2f}", f"{v['wait']:.1f}", f"{v['occ']:.1f}",
                        f"{v['loss_med']:.4f}", f"{v['loss_p90']:.4f}"])


def conds_present(a, sweep):
    return [c for c in COND_ORDER if any(k[0] == sweep and k[1] == c for k in a)]


def fmt(x):
    return "-" if x is None else (f"{x:.3f}" if x < 10 else f"{x:.1f}")


def table_1a(a):
    Ns = sorted({k[2] for k in a if k[0] == "1a"})
    if not Ns:
        return "_1a: no data yet._\n"
    out = ["### 1a — loss % vs sessions (512 KB, nice 0), duplication\n",
           "| condition | " + " | ".join(f"{n}×" for n in Ns) + " |",
           "|---|" + "--:|" * len(Ns)]
    for c in conds_present(a, "1a"):
        cells = [fmt(a.get(("1a", c, n, 512, 0), {}).get("loss_med")) for n in Ns]
        out.append(f"| {c} | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def table_1b(a):
    smbs = sorted({k[3] for k in a if k[0] == "1b"})
    Ns = sorted({k[2] for k in a if k[0] == "1b"})
    if not smbs:
        return "_1b: no data yet._\n"
    out = ["### 1b — loss % vs SMB size (nice 0), duplication\n"]
    for n in Ns:
        out += [f"\n**N = {n} sessions**\n",
                "| condition | " + " | ".join(f"{s}K" for s in smbs) + " |",
                "|---|" + "--:|" * len(smbs)]
        for c in conds_present(a, "1b"):
            cells = [fmt(a.get(("1b", c, n, s, 0), {}).get("loss_med")) for s in smbs]
            out.append(f"| {c} | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def table_1c(a):
    Ns = sorted({k[2] for k in a if k[0] == "1c"})
    nices = sorted({k[4] for k in a if k[0] == "1c"})
    if not Ns:
        return "_1c: no data yet._\n"
    out = ["### 1c — idle ceiling grid: loss % vs sessions by reader nice (512 KB)\n",
           "| nice | " + " | ".join(f"{n}×" for n in Ns) + " |", "|---|" + "--:|" * len(Ns)]
    for y in nices:
        cells = [fmt(a.get(("1c", "idle", n, 512, y), {}).get("loss_med")) for n in Ns]
        out.append(f"| {y} | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def table_1d(a):
    Ns = sorted({k[2] for k in a if k[0] == "1d"})
    nices = sorted({k[4] for k in a if k[0] == "1d"})
    if not Ns:
        return "_1d: no data yet._\n"
    out = ["### 1d — the nice fix: loss % vs sessions, nice 0 vs −10 (512 KB), duplication\n"]
    for c in conds_present(a, "1d"):
        out += [f"\n**{c}**\n",
                "| nice | " + " | ".join(f"{n}×" for n in Ns) + " |", "|---|" + "--:|" * len(Ns)]
        for y in nices:
            cells = [fmt(a.get(("1d", c, n, 512, y), {}).get("loss_med")) for n in Ns]
            out.append(f"| {y} | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def crossing_table(a):
    """Per condition (from 1a): sessions N where median loss crosses 0.1% / 1%."""
    Ns = sorted({k[2] for k in a if k[0] == "1a"})
    def cross(cond, thr):
        prev_n, prev_l = None, None
        for n in Ns:
            v = a.get(("1a", cond, n, 512, 0))
            if not v:
                continue
            l = v["loss_med"]
            if l >= thr:
                if prev_n is None:
                    return f"≤{n}"
                # linear interpolate between prev_n and n
                if l == prev_l:
                    return f"~{n}"
                x = prev_n + (n - prev_n) * (thr - prev_l) / (l - prev_l)
                return f"~{x:.1f}"
            prev_n, prev_l = n, l
        return f">{Ns[-1]}" if Ns else "-"
    out = ["### Sessions before loss (from 1a, 512 KB, duplication)\n",
           "| condition | N @ 0.1% loss | N @ 1% loss |", "|---|--:|--:|"]
    for c in conds_present(a, "1a"):
        out.append(f"| {c} | {cross(c,0.1)} | {cross(c,1.0)} |")
    return "\n".join(out) + "\n"


def main():
    rows = load()
    a = agg(rows)
    os.makedirs(os.path.dirname(OUTCSV), exist_ok=True)
    write_csv(a)
    n_cells = len(a)
    n_raw = len(rows)
    with open(SUMMARY, "w") as f:
        f.write("# RFC rerun — faithful (duplication) session sweeps: results\n\n")
        f.write(f"_Auto-generated by `aggregate.py`. Raw rows: {n_raw}; aggregated cells: {n_cells}._\n")
        f.write("_Model = duplication (`--dup-mult N` = N concurrent sessions on the real timeline)._\n\n")
        f.write(crossing_table(a) + "\n")
        f.write(table_1a(a) + "\n")
        f.write(table_1b(a) + "\n")
        f.write(table_1d(a) + "\n")
        f.write(table_1c(a) + "\n")
    print(f"[aggregate] {n_raw} raw rows -> {n_cells} cells; wrote {OUTCSV} + {SUMMARY}")


if __name__ == "__main__":
    main()
