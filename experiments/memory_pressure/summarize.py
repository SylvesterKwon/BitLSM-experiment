#!/usr/bin/env python3
"""Reduce a budget-sweep result directory to one tidy CSV.

The per-query logs are the source of truth, but a writing agent needs a table
it can read without re-deriving statistics from 84 files. This emits one row
per (c, budget, method) carrying exactly the numbers the figures plot, using
the same statistics they use, so prose and figures cannot disagree.

Usage:
    python3 experiments/memory_pressure/summarize.py <result_dir>
    python3 experiments/memory_pressure/summarize.py <result_dir> -o out.csv
"""

import argparse
import csv
import os
import re

import numpy as np

BUDGET_DIR = re.compile(r"^mb(\d+)$")
FILE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$")

# Budget 0 is the unbounded anchor: index blocks are pinned on the heap
# outside any cache, which is a different mechanism, not a larger budget.
UNBOUNDED = 0


def summarize_run(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return None
    lat = [float(r["time_elapsed_ms"]) / 1000.0 for r in rows]
    idx = [float(r["idx_read_mb"]) for r in rows]
    data = [max(0.0, float(r["disk_read_mb"]) - float(r["idx_read_mb"]))
            for r in rows]
    return {
        "queries": len(rows),
        "avg_latency_s": round(float(np.mean(lat)), 4),
        "median_latency_s": round(float(np.median(lat)), 4),
        "total_latency_s": round(sum(lat), 2),
        "avg_index_read_mb_per_query": round(sum(idx) / len(rows), 3),
        "avg_data_read_mb_per_query": round(sum(data) / len(rows), 3),
        "total_index_read_gb": round(sum(idx) / 1024.0, 3),
        "total_data_read_gb": round(sum(data) / 1024.0, 3),
        "avg_index_reads_per_query": round(
            sum(int(r["idx_reads"]) for r in rows) / len(rows), 1),
        "peak_rss_gb": round(
            max(float(r["peak_rss_kb"]) for r in rows) / 1048576.0, 3),
        "records_matched": sum(int(r["records_matched"]) for r in rows),
    }


def collect(result_dir):
    out = []
    for entry in sorted(os.listdir(result_dir)):
        sub = os.path.join(result_dir, entry)
        if not os.path.isdir(sub):
            continue
        m = BUDGET_DIR.match(entry)
        if m:
            budget = int(m.group(1))
        elif entry == "default":
            budget = UNBOUNDED
        else:
            continue
        for fname in sorted(os.listdir(sub)):
            fm = FILE_PATTERN.match(fname)
            if not fm:
                continue
            stats = summarize_run(os.path.join(sub, fname))
            if stats is None:
                continue
            row = {
                "selectivity": fm.group(1),
                "c": int(fm.group(2)),
                "budget_mb": budget,
                "method": fm.group(4),
            }
            row.update(stats)
            out.append(row)
    out.sort(key=lambda r: (r["c"], r["budget_mb"], r["method"]))
    return out


def add_speedups(rows):
    """Attach, per cell, the ratio of the best baseline to BitLSM.

    'Best' means the fastest baseline in that cell, so the speedup is always
    measured against the strongest configuration the baseline had available.
    """
    by_cell = {}
    for r in rows:
        by_cell.setdefault((r["c"], r["budget_mb"]), []).append(r)
    for cell, group in by_cell.items():
        ours = [r for r in group if r["method"].startswith("bitlsm")]
        base = [r for r in group if not r["method"].startswith("bitlsm")]
        if not ours or not base:
            for r in group:
                r["speedup_vs_best_baseline"] = ""
            continue
        best = min(base, key=lambda r: r["median_latency_s"])
        for r in group:
            r["speedup_vs_best_baseline"] = (
                round(best["median_latency_s"] / ours[0]["median_latency_s"], 2)
                if r is ours[0] else "")
    return rows


def main():
    ap = argparse.ArgumentParser(description="Summarize a budget sweep to CSV")
    ap.add_argument("result_dir")
    ap.add_argument("-o", "--output", default=None,
                    help="Output CSV (default: <result_dir>/memory_pressure_summary.csv)")
    args = ap.parse_args()

    rows = add_speedups(collect(args.result_dir))
    if not rows:
        print("No runs found.")
        return
    out_path = args.output or os.path.join(
        args.result_dir, "memory_pressure_summary.csv")
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"Saved: {out_path}  ({len(rows)} runs)")


if __name__ == "__main__":
    main()
