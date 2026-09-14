#!/usr/bin/env python3
"""Reduce the ULID key-correlation read run to two tidy CSVs.

The run reads one ULID DB per method (bitlsm, bitlsm-global, embedded,
no-index) and count_candidates.py adds, for the two BitLSM arms, how many rows
SABI's bins selected per query. This joins the two on (workload, method,
query_id) and splits the queries by whether any predicate is on a
time-correlated attribute -- the ULID key carries tpep_pickup_datetime, and
dropoff follows pickup -- because that is what changes behaviour: a single such
predicate is where global bins lose resolution and where a block's zone map can
prune at all.

  <output>/key_correlation_queries.csv  one row per (workload, method, query)
  <output>/key_correlation_summary.csv  one row per (k, group, method)

Summary statistics follow the paper's conventions: latency is the MEDIAN of
the per-query rows; candidates, candidate blocks and data read are MEANS per
query. data_read_mb is disk_read_mb - idx_read_mb and still includes SABI
blocks loaded when an SST is first opened, so candidates and candidate blocks
are the cleaner mechanism metrics.

Before writing, the matched count of every query must agree across methods and
be covered by both BitLSM arms' candidates; a disagreement stops the script.

Usage:
    python3 experiments/nyc_taxi_seq_read/summarize_key_correlation.py \
        <read_result_dir> <candidates_result_dir> [-o <output_dir>]
"""

import argparse
import csv
import os
import re
import statistics
import sys
from collections import defaultdict

READ_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_read_log\.csv$")
CANDIDATE_PATTERN = re.compile(
    r"^read_seq_sel([\d.]+)_k(\d+)_r(\d+)_(.+)_candidates\.csv$")

TIME_CORRELATED_ATTRS = {"tpep_pickup_datetime", "tpep_dropoff_datetime"}

# Method key as it appears in the file names -> label used in the paper.
METHOD_LABELS = {
    "bitlsm_rho0.001": "BitLSM",
    "bitlsm-global_rho0.001": "BitLSM (global bins)",
    "embedded_bloom_bits10": "Bloom + Zone Map",
    "no-index": "No Index",
}

QUERY_COLUMNS = [
    "method", "label", "selectivity", "k", "query_id", "filter_attrs",
    "time_correlated", "time_elapsed_ms", "records_matched",
    "data_read_mb", "candidates", "candidate_blocks", "ssts_skipped",
]
SUMMARY_COLUMNS = [
    "selectivity", "k", "group", "method", "label", "queries",
    "median_latency_ms", "mean_records_matched", "mean_data_read_mb",
    "mean_candidates", "mean_candidate_blocks",
]


def is_time_correlated(filter_attrs):
    attrs = {a.strip() for a in filter_attrs.split(",") if a.strip()}
    return bool(attrs & TIME_CORRELATED_ATTRS)


def read_candidates(candidates_dir):
    """{(selectivity, k, method, query_id): candidate row}."""
    out = {}
    for fname in sorted(os.listdir(candidates_dir)):
        m = CANDIDATE_PATTERN.match(fname)
        if not m:
            continue
        selectivity, k, _, method = m.groups()
        with open(os.path.join(candidates_dir, fname), newline="") as f:
            for r in csv.DictReader(f):
                out[(selectivity, int(k), method, int(r["query_id"]))] = r
    return out


def read_queries(read_dir, candidates):
    rows = []
    for fname in sorted(os.listdir(read_dir)):
        m = READ_PATTERN.match(fname)
        if not m:
            continue
        selectivity, k, _, method = m.groups()
        k = int(k)
        with open(os.path.join(read_dir, fname), newline="") as f:
            for r in csv.DictReader(f):
                qid = int(r["query_id"])
                cand = candidates.get((selectivity, k, method, qid))
                rows.append({
                    "method": method,
                    "label": METHOD_LABELS.get(method, method),
                    "selectivity": selectivity,
                    "k": k,
                    "query_id": qid,
                    "filter_attrs": r["filter_attrs"],
                    "time_correlated": is_time_correlated(r["filter_attrs"]),
                    "time_elapsed_ms": float(r["time_elapsed_ms"]),
                    "records_matched": int(r["records_matched"]),
                    "data_read_mb": float(r["disk_read_mb"])
                                    - float(r["idx_read_mb"]),
                    "candidates": int(cand["candidates"]) if cand else "",
                    "candidate_blocks":
                        int(cand["candidate_blocks"]) if cand else "",
                    "ssts_skipped": int(cand["ssts_skipped"]) if cand else "",
                })
    rows.sort(key=lambda r: (float(r["selectivity"]), r["k"], r["method"],
                             r["query_id"]))
    return rows


def check(rows, candidates):
    """Every method must return the same rows, and BitLSM's candidates must
    cover them; otherwise the comparison is not measuring the same queries."""
    problems = []
    by_query = defaultdict(dict)
    for r in rows:
        by_query[(r["selectivity"], r["k"], r["query_id"])][r["method"]] = r
    for key, methods in by_query.items():
        matched = {m: r["records_matched"] for m, r in methods.items()}
        if len(set(matched.values())) > 1:
            problems.append(f"sel{key[0]} k{key[1]} q{key[2]}: matched "
                            f"differs across methods {matched}")
        for m, r in methods.items():
            if r["candidates"] != "" and r["candidates"] < r["records_matched"]:
                problems.append(f"sel{key[0]} k{key[1]} q{key[2]} {m}: "
                                f"{r['candidates']} candidates < "
                                f"{r['records_matched']} matched")
    expected = [m for m in METHOD_LABELS if m.startswith("bitlsm")]
    cells = {(r["selectivity"], r["k"]) for r in rows}
    for s, k in sorted(cells):
        for m in expected:
            n_read = sum(1 for r in rows
                         if (r["selectivity"], r["k"], r["method"]) == (s, k, m))
            n_cand = sum(1 for key in candidates if key[:3] == (s, k, m))
            if n_read and n_read != n_cand:
                problems.append(f"sel{s} k{k} {m}: {n_read} read rows but "
                                f"{n_cand} candidate rows")
    return problems


def summarize(rows):
    cells = defaultdict(list)
    for r in rows:
        group = "time_correlated" if r["time_correlated"] else "uncorrelated"
        for g in (group, "all"):
            cells[(r["selectivity"], r["k"], g, r["method"])].append(r)

    def mean_of(items, col):
        vals = [i[col] for i in items if i[col] != ""]
        return round(statistics.mean(vals), 3) if vals else ""

    out = []
    order = {"time_correlated": 0, "uncorrelated": 1, "all": 2}
    methods = list(METHOD_LABELS)
    for (s, k, g, m), items in sorted(
            cells.items(),
            key=lambda kv: (float(kv[0][0]), kv[0][1], order[kv[0][2]],
                            methods.index(kv[0][3])
                            if kv[0][3] in methods else len(methods))):
        out.append({
            "selectivity": s, "k": k, "group": g, "method": m,
            "label": METHOD_LABELS.get(m, m),
            "queries": len(items),
            "median_latency_ms":
                statistics.median(i["time_elapsed_ms"] for i in items),
            "mean_records_matched": mean_of(items, "records_matched"),
            "mean_data_read_mb": mean_of(items, "data_read_mb"),
            "mean_candidates": mean_of(items, "candidates"),
            "mean_candidate_blocks": mean_of(items, "candidate_blocks"),
        })
    return out


def write_csv(path, columns, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns)
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(
        description="Summarize the ULID key-correlation read run")
    ap.add_argument("read_dir", help="nyc_taxi_seq_read result directory")
    ap.add_argument("candidates_dir",
                    help="count_candidates.py result directory")
    ap.add_argument("-o", "--output-dir", default=None,
                    help="Where to write the CSVs (default: read_dir)")
    args = ap.parse_args()

    candidates = read_candidates(args.candidates_dir)
    rows = read_queries(args.read_dir, candidates)
    if not rows:
        sys.exit(f"No read logs found in {args.read_dir}")

    problems = check(rows, candidates)
    if problems:
        for p in problems[:20]:
            print(f"[error] {p}", file=sys.stderr)
        sys.exit(f"{len(problems)} consistency problem(s); nothing written")

    out_dir = args.output_dir or args.read_dir
    os.makedirs(out_dir, exist_ok=True)
    queries_path = os.path.join(out_dir, "key_correlation_queries.csv")
    summary_path = os.path.join(out_dir, "key_correlation_summary.csv")
    summary = summarize(rows)
    write_csv(queries_path, QUERY_COLUMNS, rows)
    write_csv(summary_path, SUMMARY_COLUMNS, summary)
    print(f"Saved: {queries_path}  ({len(rows)} query rows)")
    print(f"Saved: {summary_path}  ({len(summary)} cells)")


if __name__ == "__main__":
    main()
