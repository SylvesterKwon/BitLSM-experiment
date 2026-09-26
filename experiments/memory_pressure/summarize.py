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
    lat = [float(r["time_elapsed_ms"]) for r in rows]
    idx = [float(r["idx_read_mb"]) for r in rows]
    data = [max(0.0, float(r["disk_read_mb"]) - float(r["idx_read_mb"]))
            for r in rows]
    # Present only in runs made with the CPU instrumentation; older result
    # directories are summarised without these columns rather than with zeros,
    # which would read as "no CPU time" instead of "not measured".
    has_cpu = "cpu_user_ms" in rows[0]
    if has_cpu:
        cpu = [float(r["cpu_user_ms"]) + float(r["cpu_sys_ms"]) for r in rows]
        # Wall clock a query did not spend on a CPU. io_uring completions do
        # not burn CPU while outstanding, so on an otherwise idle machine this
        # is device wait.
        io_wait = [max(0.0, w - c) for w, c in zip(lat, cpu)]
    # honk_player writes these columns in every run but fills them only under
    # EXP_CACHE_STATS; without it they are all zero, which means not measured.
    has_blocks = "dataudi_miss" in rows[0] and any(
        int(r["dataudi_miss"]) or int(r["dataudi_hit"]) or int(r["keyidx_hit"])
        or int(r["keyidx_miss"]) for r in rows)
    return {
        "queries": len(rows),
        "mean_latency_ms": round(float(np.mean(lat)), 1),
        "median_latency_ms": round(float(np.median(lat)), 1),
        "total_latency_ms": round(sum(lat)),
        "mean_index_read_mb_per_query": round(sum(idx) / len(rows), 3),
        "mean_data_read_mb_per_query": round(sum(data) / len(rows), 3),
        "total_index_read_gb": round(sum(idx) / 1024.0, 3),
        "total_data_read_gb": round(sum(data) / 1024.0, 3),
        "mean_index_reads_per_query": round(
            sum(int(r["idx_reads"]) for r in rows) / len(rows), 1),
        **({"mean_cpu_ms_per_query": round(sum(cpu) / len(rows), 1),
            "mean_cpu_user_ms_per_query": round(
                sum(float(r["cpu_user_ms"]) for r in rows) / len(rows), 1),
            "mean_cpu_sys_ms_per_query": round(
                sum(float(r["cpu_sys_ms"]) for r in rows) / len(rows), 1),
            "mean_io_wait_ms_per_query": round(sum(io_wait) / len(rows), 1),
            "cpu_fraction": round(sum(cpu) / sum(lat), 3) if sum(lat) else 0.0}
           if has_cpu else {}),
        **({"mean_data_blocks_per_query": round(
                sum(int(r["dataudi_miss"]) for r in rows) / len(rows), 1),
            "mean_rocksdb_index_miss_per_query": round(
                sum(int(r["keyidx_miss"]) for r in rows) / len(rows), 1)}
           if has_blocks else {}),
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
        best = min(base, key=lambda r: r["median_latency_ms"])
        for r in group:
            r["speedup_vs_best_baseline"] = (
                round(best["median_latency_ms"] / ours[0]["median_latency_ms"], 2)
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
