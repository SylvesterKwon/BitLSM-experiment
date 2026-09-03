#!/usr/bin/env python3
"""Reduce a read_perf result directory to one tidy CSV per cell.

perf_mode.csv carries one row per (cell, query) with counters attached; a
writing agent needs the per-cell table the figures are drawn from, with the
aggregation already applied. This emits one row per cell, plus the per-query
rows in a second file, so prose and figures cannot disagree.

Two statistics, chosen for different reasons:

  * across repetitions of one query, the median -- interference only ever
    makes a run slower, so the distribution is one-sided and a single slow
    sample would drag a mean. perf_run.py already reduces cold_reps this way,
    so cold_ms arrives here as that median.
  * across queries, the geometric mean -- latencies span four orders of
    magnitude within a workload (SSB: 4ms to 36s), and an arithmetic mean
    over that range reports the two slowest queries and nothing else.

Cold is the headline: under direct I/O a fresh server is the whole cold
mechanism, and it is the number that answers "what does this cost when the
data is not already in memory".

A cell is (engine, index_layout, plan, session_vars). session_vars is part of
the key, not decoration: two cells can differ in nothing else, and collapsing
them silently overwrites one with the other.

Usage:
    python3 experiments/myrocks_integration_test/summarize_query_performance.py <result_dir>
    python3 experiments/myrocks_integration_test/summarize_query_performance.py <result_dir> -o out.csv
"""

import argparse
import csv
import os
import statistics as st
import sys

# The cell key. session_vars included deliberately -- see the module docstring.
CELL_KEY = ["engine", "index_layout", "plan", "session_vars"]

# Physical read counters, per engine. The LSM engines report block-cache
# misses (a count of blocks) and InnoDB reports buffer-pool reads (a count of
# pages); bytes are derived from the unit size rather than compared directly,
# because rocksdb_bytes_read counts user bytes returned, not bytes read from
# disk, and comparing it against innodb_data_read compares two different
# things.
IO_COUNT = {"innodb": "innodb_buffer_pool_reads",
            "myrocks": "rocksdb_block_cache_miss",
            "bitlsm": "rocksdb_block_cache_miss"}


def geomean(values):
    return st.geometric_mean(values) if values else None


def read_rows(result_dir):
    path = os.path.join(result_dir, "perf_mode.csv")
    if not os.path.isfile(path):
        sys.exit(f"No perf_mode.csv in {result_dir}")
    with open(path, newline="") as f:
        return [r for r in csv.DictReader(f) if r["cold_ms"]]


def per_query(rows):
    """One row per (cell, query), carrying the numbers a figure would plot."""
    out = []
    for r in rows:
        eng = r["engine"]
        io = r.get(IO_COUNT.get(eng, ""), "")
        out.append({
            **{k: r[k] for k in CELL_KEY},
            "workload": r["workload"],
            "query_id": r["query_id"],
            "cold_ms": float(r["cold_ms"]),
            "cold_ms_min": float(r["cold_ms_min"]) if r.get("cold_ms_min") else None,
            "cold_ms_max": float(r["cold_ms_max"]) if r.get("cold_ms_max") else None,
            "cold_reps": int(r["cold_reps"]) if r.get("cold_reps") else 1,
            "warm_ms_median": float(r["warm_ms_median"]) if r["warm_ms_median"] else None,
            "rows_returned": int(r["rows_returned"]) if r["rows_returned"] else None,
            "chosen_access": r["chosen_access"],
            "chosen_key": r["chosen_key"],
            "io_reads": int(io) if io else None,
            "fp_match": int(r["fp_match"]) if r["fp_match"] else None,
        })
    return out


def per_cell(qrows):
    """One row per cell: the geometric mean over its queries, and the spread
    that says whether the aggregate can be trusted."""
    cells = {}
    for r in qrows:
        cells.setdefault(tuple(r[k] for k in CELL_KEY), []).append(r)
    out = []
    for key, group in cells.items():
        cold = [r["cold_ms"] for r in group]
        warm = [r["warm_ms_median"] for r in group if r["warm_ms_median"]]
        io = [r["io_reads"] for r in group if r["io_reads"] is not None]
        # Widest single-query repetition spread in the cell: the honest answer
        # to "how much of this number is measurement noise".
        spreads = [r["cold_ms_max"] / r["cold_ms_min"]
                   for r in group
                   if r["cold_ms_min"] and r["cold_ms_max"] and r["cold_ms_min"] > 0]
        out.append({
            **dict(zip(CELL_KEY, key)),
            "workload": group[0]["workload"],
            "queries": len(group),
            "cold_reps": group[0]["cold_reps"],
            "cold_geomean_ms": round(geomean(cold), 3),
            "cold_median_ms": round(st.median(cold), 3),
            "cold_min_ms": round(min(cold), 3),
            "cold_max_ms": round(max(cold), 3),
            "max_rep_spread": round(max(spreads), 3) if spreads else None,
            "warm_geomean_ms": round(geomean(warm), 3) if warm else None,
            "io_reads_total": sum(io) if io else None,
            "fp_mismatches": sum(1 for r in group if r["fp_match"] == 0),
        })
    out.sort(key=lambda r: (r["workload"], r["cold_geomean_ms"]))
    return out


def add_ratios(cells):
    """Every cell against the fastest cell of its workload, so a reader can
    see the ordering without dividing anything by hand."""
    best = {}
    for r in cells:
        w = r["workload"]
        if w not in best or r["cold_geomean_ms"] < best[w]:
            best[w] = r["cold_geomean_ms"]
    for r in cells:
        r["vs_best"] = round(r["cold_geomean_ms"] / best[r["workload"]], 3)
    return cells


def write_csv(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(
        description="Summarize a read_perf sweep to CSV")
    ap.add_argument("result_dir")
    ap.add_argument("-o", "--output", default=None,
                    help="Output CSV (default: <result_dir>/query_perf_summary.csv)")
    args = ap.parse_args()

    qrows = per_query(read_rows(args.result_dir))
    if not qrows:
        print("No measured cells found.")
        return
    cells = add_ratios(per_cell(qrows))

    out_path = args.output or os.path.join(
        args.result_dir, "query_perf_summary.csv")
    write_csv(out_path, cells)
    q_path = os.path.splitext(out_path)[0] + "_by_query.csv"
    write_csv(q_path, qrows)

    print(f"Saved: {out_path}  ({len(cells)} cells)")
    print(f"Saved: {q_path}  ({len(qrows)} query rows)")
    bad = sum(r["fp_mismatches"] for r in cells)
    if bad:
        print(f"WARNING: {bad} result-fingerprint mismatches in this sweep")


if __name__ == "__main__":
    main()
