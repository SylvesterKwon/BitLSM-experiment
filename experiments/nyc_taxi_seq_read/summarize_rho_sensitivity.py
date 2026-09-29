#!/usr/bin/env python3
"""Reduce the rho sensitivity runs to tidy CSVs.

Three result directories feed it: the read sweep (BitLSM at every rho x
predicate count c, one DB per rho loaded once and reused), count_candidates.py
over the same DBs and queries, and the ingest sweep (No Index plus every rho,
each run a fresh load). The read and candidate rows join on (c, rho, query_id).

  <output>/rho_sensitivity_queries.csv        one row per (c, rho, query)
  <output>/rho_sensitivity_query_summary.csv  one row per (c, rho)
  <output>/rho_sensitivity_default_gap.csv    one row per c
  <output>/rho_sensitivity_ingest.csv         one row per ingest arm

Statistics follow the paper's conventions: latency is the MEDIAN of the
per-query rows, with the MEAN alongside; CPU is given both ways too; byte
volumes are MEANS per query. CPU, disk read and RSS are process-wide counters, so they are named as
process measurements, not as any one stage of the query. candidate_blocks is
the number of data blocks holding at least one candidate -- a logical count,
not the device reads, which the block cache and prefetching change.

The ingest columns are: input_ms, from DB open to the last Put; ingest_ms, to
the end of the flush/compaction drain; background_cpu_s, the CPU time of
RocksDB's flush and compaction threads up to the drain; peak_rss_kb, the
process VmHWM read after the drain; db_size_bytes, `du -sb` after Close. Each
is also given relative to No Index and to rho = 0.001, the default.

Before writing, every query's matched count must agree across rho, candidates
must cover the matches, and every read cell must have its candidate rows; a
disagreement stops the script.

Usage:
    python3 experiments/nyc_taxi_seq_read/summarize_rho_sensitivity.py \\
        <read_dir> <candidates_dir> <ingest_dir> [-o <output_dir>]
"""

import argparse
import csv
import os
import re
import statistics
import sys
from collections import defaultdict

DEFAULT_RHO = 0.001
HZ = 100  # USER_HZ for /proc/<pid>/task/<tid>/stat ticks

READ_PATTERN = re.compile(
    r"^read_seq_sel[\d.e+-]+?_k(\d+)_r\d+_bitlsm_rho([\d.]+)_read_log\.csv$")
CANDIDATE_PATTERN = re.compile(
    r"^read_seq_sel[\d.e+-]+?_k(\d+)_r\d+_bitlsm_rho([\d.]+)_candidates\.csv$")
WRITE_PATTERN = re.compile(r"^(.+)_(no-index|bitlsm_rho[\d.]+)_write_log\.csv$")

QUERY_COLUMNS = [
    "c", "rho", "query_id", "filter_attrs", "latency_ms", "records_matched",
    "candidates", "candidate_blocks", "ssts_skipped", "excess_candidates",
    "candidate_ratio", "false_positive_share", "process_cpu_ms",
    "process_disk_read_bytes", "process_peak_rss_kb",
]
QUERY_SUMMARY_COLUMNS = [
    "c", "rho", "queries",
    "median_latency_ms", "mean_latency_ms", "median_latency_vs_default",
    "mean_records_matched",
    "median_candidates", "mean_candidates",
    "median_candidate_blocks", "mean_candidate_blocks",
    "mean_excess_candidates", "median_candidate_ratio",
    "median_false_positive_share",
    "median_process_cpu_ms", "mean_process_cpu_ms",
    "mean_process_disk_read_mb",
    "process_peak_rss_kb",
]
GAP_COLUMNS = [
    "c", "default_rho",
    "default_median_latency_ms", "best_rho_by_median",
    "best_median_latency_ms", "default_over_best_median",
    "default_mean_latency_ms", "best_rho_by_mean",
    "best_mean_latency_ms", "default_over_best_mean",
]
INGEST_COLUMNS = [
    "method", "rho", "records", "input_ms", "ingest_ms", "drain_ms",
    "background_cpu_s", "peak_rss_kb", "db_size_bytes",
    "ingest_ms_vs_no_index", "background_cpu_vs_no_index",
    "peak_rss_vs_no_index", "db_size_vs_no_index",
    "ingest_ms_vs_default", "background_cpu_vs_default",
    "peak_rss_vs_default", "db_size_vs_default",
]


def read_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def read_candidates(candidates_dir):
    """{(c, rho, query_id): candidate row}."""
    out = {}
    for fname in sorted(os.listdir(candidates_dir)):
        m = CANDIDATE_PATTERN.match(fname)
        if not m:
            continue
        c, rho = int(m.group(1)), float(m.group(2))
        for r in read_rows(os.path.join(candidates_dir, fname)):
            out[(c, rho, int(r["query_id"]))] = r
    return out


def read_queries(read_dir, candidates):
    rows = []
    for fname in sorted(os.listdir(read_dir)):
        m = READ_PATTERN.match(fname)
        if not m:
            continue
        c, rho = int(m.group(1)), float(m.group(2))
        for r in read_rows(os.path.join(read_dir, fname)):
            qid = int(r["query_id"])
            cand = candidates.get((c, rho, qid))
            matched = int(r["records_matched"])
            n_cand = int(cand["candidates"]) if cand else None
            rows.append({
                "c": c, "rho": rho, "query_id": qid,
                "filter_attrs": r["filter_attrs"],
                "latency_ms": int(r["time_elapsed_us"]) / 1000,
                "records_matched": matched,
                "candidates": n_cand if cand else "",
                "candidate_blocks": int(cand["candidate_blocks"]) if cand else "",
                "ssts_skipped": int(cand["ssts_skipped"]) if cand else "",
                "excess_candidates": n_cand - matched if cand else "",
                "candidate_ratio":
                    n_cand / matched if cand and matched else "",
                "false_positive_share":
                    (n_cand - matched) / n_cand if cand and n_cand else "",
                "process_cpu_ms":
                    float(r["cpu_user_ms"]) + float(r["cpu_sys_ms"]),
                "process_disk_read_bytes": int(r["disk_read_bytes"]),
                "process_peak_rss_kb": int(r["peak_rss_kb"]),
            })
    rows.sort(key=lambda r: (r["c"], r["rho"], r["query_id"]))
    return rows


def check(rows, candidates):
    """The same queries must return the same rows at every rho, and the
    candidates must be the superset the query path verifies."""
    problems = []
    matched = defaultdict(dict)
    cells = defaultdict(int)
    for r in rows:
        matched[(r["c"], r["query_id"])][r["rho"]] = r["records_matched"]
        cells[(r["c"], r["rho"])] += 1
        where = f"c={r['c']} rho={r['rho']:g} q{r['query_id']}"
        if r["candidates"] == "":
            problems.append(f"{where}: no candidate row")
        elif r["candidates"] < r["records_matched"]:
            problems.append(f"{where}: {r['candidates']} candidates < "
                            f"{r['records_matched']} matched")
    for (c, qid), by_rho in matched.items():
        if len(set(by_rho.values())) > 1:
            problems.append(f"c={c} q{qid}: matched differs across rho {by_rho}")
    for (c, rho), n in cells.items():
        n_cand = sum(1 for key in candidates if key[:2] == (c, rho))
        if n != n_cand:
            problems.append(f"c={c} rho={rho:g}: {n} read rows but "
                            f"{n_cand} candidate rows")
    return problems


def summarize_queries(rows):
    cells = defaultdict(list)
    for r in rows:
        cells[(r["c"], r["rho"])].append(r)

    def med(items, col):
        vals = [i[col] for i in items if i[col] != ""]
        return statistics.median(vals) if vals else ""

    def mean(items, col):
        vals = [i[col] for i in items if i[col] != ""]
        return statistics.mean(vals) if vals else ""

    out = []
    for (c, rho), items in sorted(cells.items()):
        out.append({
            "c": c, "rho": rho, "queries": len(items),
            "median_latency_ms": med(items, "latency_ms"),
            "mean_latency_ms": mean(items, "latency_ms"),
            "mean_records_matched": mean(items, "records_matched"),
            "median_candidates": med(items, "candidates"),
            "mean_candidates": mean(items, "candidates"),
            "median_candidate_blocks": med(items, "candidate_blocks"),
            "mean_candidate_blocks": mean(items, "candidate_blocks"),
            "mean_excess_candidates": mean(items, "excess_candidates"),
            "median_candidate_ratio": med(items, "candidate_ratio"),
            "median_false_positive_share": med(items, "false_positive_share"),
            "median_process_cpu_ms": med(items, "process_cpu_ms"),
            "mean_process_cpu_ms": mean(items, "process_cpu_ms"),
            "mean_process_disk_read_mb":
                mean(items, "process_disk_read_bytes") / (1 << 20),
            # VmHWM only grows, so the run's peak is its largest row.
            "process_peak_rss_kb": max(i["process_peak_rss_kb"] for i in items),
        })
    default = {r["c"]: r["median_latency_ms"] for r in out
               if r["rho"] == DEFAULT_RHO}
    for r in out:
        base = default.get(r["c"])
        r["median_latency_vs_default"] = (r["median_latency_ms"] / base
                                          if base else "")
    return out


def default_gap(summary):
    """What rho = 0.001 gives up against the best MEASURED rho, per c."""
    by_c = defaultdict(list)
    for r in summary:
        by_c[r["c"]].append(r)
    out = []
    for c, cells in sorted(by_c.items()):
        default = next((r for r in cells if r["rho"] == DEFAULT_RHO), None)
        if default is None:
            continue
        best_med = min(cells, key=lambda r: r["median_latency_ms"])
        best_mean = min(cells, key=lambda r: r["mean_latency_ms"])
        out.append({
            "c": c, "default_rho": DEFAULT_RHO,
            "default_median_latency_ms": default["median_latency_ms"],
            "best_rho_by_median": best_med["rho"],
            "best_median_latency_ms": best_med["median_latency_ms"],
            "default_over_best_median":
                default["median_latency_ms"] / best_med["median_latency_ms"],
            "default_mean_latency_ms": default["mean_latency_ms"],
            "best_rho_by_mean": best_mean["rho"],
            "best_mean_latency_ms": best_mean["mean_latency_ms"],
            "default_over_best_mean":
                default["mean_latency_ms"] / best_mean["mean_latency_ms"],
        })
    return out


def read_ingest(ingest_dir):
    """One row per ingest arm, from its write, thread and sample logs."""
    sizes = {}
    size_csv = os.path.join(ingest_dir, "db_size.csv")
    if os.path.exists(size_csv):
        for r in read_rows(size_csv):
            key = "no-index" if r["method"] == "no-index" else float(r["rho"])
            sizes[key] = int(r["db_size_bytes"])

    out, problems = [], []
    for fname in sorted(os.listdir(ingest_dir)):
        m = WRITE_PATTERN.match(fname)
        if not m:
            continue
        prefix = os.path.join(ingest_dir, fname[:-len("_write_log.csv")])
        tag = m.group(2)
        key = "no-index" if tag == "no-index" else float(tag[len("bitlsm_rho"):])
        write = read_rows(prefix + "_write_log.csv")
        # The last two rows are the input end and the end of the drain.
        if len(write) < 2 or write[-1]["records_written"] != \
                write[-2]["records_written"]:
            problems.append(f"{tag}: write log has no input/drain end rows")
            continue
        threads = read_rows(prefix + "_thread_log.csv")
        last = threads[-1]["timestamp_ns"]
        # Ticks are cumulative per thread, so the post-drain sample holds the
        # run's totals. rocksdb:high is flush, rocksdb:low is compaction.
        bg_ticks = sum(int(t["utime_ticks"]) + int(t["stime_ticks"])
                       for t in threads
                       if t["timestamp_ns"] == last
                       and t["comm"].startswith("rocksdb"))
        samples = read_rows(prefix + "_sample_log.csv")
        if key not in sizes:
            problems.append(f"{tag}: no db_size.csv row")
        out.append({
            "method": "no-index" if key == "no-index" else "bitlsm",
            "rho": "" if key == "no-index" else key,
            "records": int(write[-1]["records_written"]),
            "input_ms": int(write[-2]["time_elapsed_ms"]),
            "ingest_ms": int(write[-1]["time_elapsed_ms"]),
            "drain_ms": int(write[-1]["time_elapsed_ms"])
                        - int(write[-2]["time_elapsed_ms"]),
            "background_cpu_s": bg_ticks / HZ,
            "peak_rss_kb": int(samples[-1]["peak_rss_kb"]),
            "db_size_bytes": sizes.get(key, ""),
        })

    base = next((r for r in out if r["method"] == "no-index"), None)
    default = next((r for r in out if r["rho"] == DEFAULT_RHO), None)
    for r in out:
        for col, name in (("ingest_ms", "ingest_ms"),
                          ("background_cpu_s", "background_cpu"),
                          ("peak_rss_kb", "peak_rss"),
                          ("db_size_bytes", "db_size")):
            for ref, suffix in ((base, "no_index"), (default, "default")):
                ok = ref and ref[col] not in ("", 0) and r[col] != ""
                r[f"{name}_vs_{suffix}"] = r[col] / ref[col] if ok else ""
    out.sort(key=lambda r: (r["method"] != "no-index",
                            -(r["rho"] or 0)))
    return out, problems


def write_csv(path, columns, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns)
        w.writeheader()
        w.writerows(rows)
    print(f"Saved: {path}  ({len(rows)} rows)")


def main():
    ap = argparse.ArgumentParser(
        description="Summarize the rho sensitivity runs")
    ap.add_argument("read_dir", help="nyc_taxi_seq_read result directory")
    ap.add_argument("candidates_dir",
                    help="count_candidates.py result directory")
    ap.add_argument("ingest_dir", help="nyc_taxi_seq_write result directory")
    ap.add_argument("-o", "--output-dir", default=None,
                    help="Where to write the CSVs (default: read_dir)")
    args = ap.parse_args()

    candidates = read_candidates(args.candidates_dir)
    rows = read_queries(args.read_dir, candidates)
    if not rows:
        sys.exit(f"No read logs found in {args.read_dir}")
    ingest, problems = read_ingest(args.ingest_dir)
    problems += check(rows, candidates)
    if problems:
        for p in problems[:20]:
            print(f"[error] {p}", file=sys.stderr)
        sys.exit(f"{len(problems)} consistency problem(s); nothing written")

    out_dir = args.output_dir or args.read_dir
    os.makedirs(out_dir, exist_ok=True)
    summary = summarize_queries(rows)
    write_csv(os.path.join(out_dir, "rho_sensitivity_queries.csv"),
              QUERY_COLUMNS, rows)
    write_csv(os.path.join(out_dir, "rho_sensitivity_query_summary.csv"),
              QUERY_SUMMARY_COLUMNS, summary)
    write_csv(os.path.join(out_dir, "rho_sensitivity_default_gap.csv"),
              GAP_COLUMNS, default_gap(summary))
    write_csv(os.path.join(out_dir, "rho_sensitivity_ingest.csv"),
              INGEST_COLUMNS, ingest)


if __name__ == "__main__":
    main()
