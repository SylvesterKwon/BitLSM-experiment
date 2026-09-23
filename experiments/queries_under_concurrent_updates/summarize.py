#!/usr/bin/env python3
"""Reduce a queries_under_concurrent_updates result directory to two tidy CSVs.

Several directories at once produce one combined table instead, written next
to the last of them: queries_under_concurrent_updates_windows_all.csv /
queries_under_concurrent_updates_summary_all.csv, the same rows with a
leading `c` column saying which query set each came from.

queries_under_concurrent_updates_windows.csv  one row per (method, W, window): what the figure plots -- QPS,
             median latency, the writer's issued count and backlog, the LSM
             stall deltas and levels at the window's end.
queries_under_concurrent_updates_summary.csv  one row per (method, W): totals, the no-load QPS of the same
             method (its W = 0 run) and the ratio, the overload verdict.

plot_latency_vs_w.py imports collect() from here, so the table and the figure are
one computation. Both the mean and the median of the per-query latencies are carried: the
figure plots the mean, the query-performance section reports medians. Byte
volumes are totals over the window/run (writer-side
flush/compaction volume, unlike the per-query index-read bytes of the read
experiments).

Usage:
    python3 experiments/queries_under_concurrent_updates/summarize.py <result_dir>
"""

import argparse
import csv
import json
import os
import re
import sys

import numpy as np

META_PATTERN = re.compile(
    r"^(?P<stem>.+)_(?P<method>bitlsm_rho[\d.]+|embedded-postings_il\d+|"
    r"embedded_bloom_bits\d+)_w(?P<rate>[\d.]+)_meta\.json$")

# Written once per sweep by run.py's write_sweep_meta(); not a run file, so
# collect() must not treat it as an unrecognized run.
SWEEP_META = "sweep_meta.json"

# Overload verdict; every threshold lives here. A run is overloaded when the
# writer could not keep the schedule: it issued less than this share of W,
# or ended more than one second of updates behind, or its backlog was
# positive and still rising over the last N windows.
OVERLOAD_RATE_FRACTION = 0.95
OVERLOAD_TAIL_WINDOWS = 3

WINDOW_COLUMNS = [
    "method", "rate", "window", "t_start_s", "t_end_s", "queries", "qps",
    "median_latency_ms", "mean_matched", "updates_issued", "backlog_end",
    "lag_mean_us", "put_mean_us", "put_max_us", "stall_delays", "stall_stops",
    "pending_compaction_mb", "l0_files", "running_compactions", "memtable_mb",
]
RUN_COLUMNS = [
    "method", "rate", "target_rate", "actual_rate", "qps_total", "qps_noload",
    "qps_ratio", "overloaded", "failed", "queries_completed",
    "mean_latency_ms", "median_latency_ms",
    "updates_scheduled", "updates_issued", "unissued", "backlog_at_end",
    "in_flight_at_end", "drain_ms", "wrapped", "stall_delays", "stall_stops",
    "flush_gb", "compact_write_gb", "matched_drift", "settle_ms", "preload_ms",
]

GB = float(1 << 30)
MB = float(1 << 20)


def parse_rate(text):
    v = float(text)
    return int(v) if v == int(v) else v


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def load_run(result_dir, prefix, method, rate):
    with open(os.path.join(result_dir, prefix + "_meta.json")) as f:
        meta = json.load(f)
    return {
        "method": method, "rate": rate, "meta": meta,
        "queries": read_csv(os.path.join(result_dir, prefix + "_query_log.csv")),
        "writes": read_csv(os.path.join(result_dir, prefix + "_write_log.csv")),
        "lsm": read_csv(os.path.join(result_dir, prefix + "_lsm_log.csv")),
    }


def _last_before(rows, key, limit):
    """Last row whose integer `key` is < limit (rows are in ascending order)."""
    hit = None
    for r in rows:
        if int(r[key]) < limit:
            hit = r
        else:
            break
    return hit


def window_rows(run):
    meta = run["meta"]
    window_s = meta["window_s"]
    duration_s = meta["duration_s"]
    n = int(round(duration_s / window_s))
    if abs(duration_s / window_s - n) > 1e-9:
        print(f"[warn] duration_s {duration_s} is not a multiple of window_s "
              f"{window_s}; the last window is truncated", file=sys.stderr)
    out = []
    for w in range(n):
        lo_ms, hi_ms = int(w * window_s * 1000), int((w + 1) * window_s * 1000)
        last = w == n - 1  # a query ending exactly at T is in the window (in_window says so)
        q = [r for r in run["queries"] if r["in_window"] == "1"
             and lo_ms <= int(r["end_ms"]) < hi_ms + (1 if last else 0)]
        row = {
            "method": run["method"], "rate": run["rate"], "window": w,
            "t_start_s": lo_ms / 1000, "t_end_s": hi_ms / 1000,
            "queries": len(q), "qps": round(len(q) / window_s, 3),
            "median_latency_ms": round(float(np.median([int(r["latency_us"]) for r in q])) / 1000, 3) if q else "",
            "mean_matched": round(float(np.mean([int(r["records_matched"]) for r in q])), 1) if q else "",
        }
        lo_s, hi_s = lo_ms / 1000, hi_ms / 1000
        secs = [r for r in run["writes"] if lo_s <= int(r["sec"]) < hi_s]
        if secs:
            prev = _last_before(run["writes"], "sec", lo_s)
            issued_before = int(prev["issued_cum"]) if prev else 0
            counts = [int(r["put_count"]) for r in secs]
            n_put = sum(counts)
            row.update({
                "updates_issued": int(secs[-1]["issued_cum"]) - issued_before,
                "backlog_end": int(secs[-1]["backlog_end"]),
                "lag_mean_us": round(sum(int(r["lag_mean_us"]) * c for r, c in zip(secs, counts)) / n_put) if n_put else "",
                "put_mean_us": round(sum(int(r["put_mean_us"]) * c for r, c in zip(secs, counts)) / n_put) if n_put else "",
                "put_max_us": max(int(r["put_max_us"]) for r in secs),
            })
        else:
            row.update({"updates_issued": "", "backlog_end": "", "lag_mean_us": "",
                        "put_mean_us": "", "put_max_us": ""})
        end = _last_before(run["lsm"], "t_ms", hi_ms)
        start = _last_before(run["lsm"], "t_ms", lo_ms) or (run["lsm"][0] if run["lsm"] else None)
        if end and start:
            row.update({
                "stall_delays": int(end["stall_delays"]) - int(start["stall_delays"]),
                "stall_stops": int(end["stall_stops"]) - int(start["stall_stops"]),
                "pending_compaction_mb": round(int(end["pending_compaction_bytes"]) / MB, 1),
                "l0_files": int(end["l0_files"]),
                "running_compactions": int(end["running_compactions"]),
                "memtable_mb": round(int(end["memtable_bytes"]) / MB, 1),
            })
        else:
            row.update({"stall_delays": "", "stall_stops": "", "pending_compaction_mb": "",
                        "l0_files": "", "running_compactions": "", "memtable_mb": ""})
        out.append(row)
    return out


def matched_drift(run):
    """Relative change of the mean match count between the first and the last
    complete pass over the query set. Uniform overwrites keep the marginal
    distribution, so this should sit near 0; it is a sanity check that the
    updates did not skew what the queries return."""
    qc = run["meta"]["query_count"]
    by_pass = {}
    for r in run["queries"]:
        if r["in_window"] == "1":
            by_pass.setdefault(int(r["pass"]), []).append(int(r["records_matched"]))
    complete = [p for p in sorted(by_pass) if len(by_pass[p]) == qc]
    if len(complete) < 2:
        return ""
    first, last = np.mean(by_pass[complete[0]]), np.mean(by_pass[complete[-1]])
    return round(float(last / first - 1), 4) if first else ""


def run_summary(run, windows):
    meta = run["meta"]
    t = meta["totals"]
    rate = run["rate"]
    duration_s = meta["duration_s"]
    lat = [int(r["latency_us"]) for r in run["queries"] if r["in_window"] == "1"]
    # The writer's last write-log row can be for sec == T (its last Put
    # finished more than a second after T), whose backlog is
    # ScheduledBy(T+1) - issued -- roughly W too large. updates_unissued is
    # the driver's own ScheduledBy(T) - issued, the true B(T).
    backlog_end = t["updates_unissued"] if rate > 0 else 0
    tail = [w["backlog_end"] for w in windows[-OVERLOAD_TAIL_WINDOWS:] if w["backlog_end"] != ""]
    rising = (len(tail) == OVERLOAD_TAIL_WINDOWS and all(b > 0 for b in tail)
              and all(a < b for a, b in zip(tail, tail[1:])))
    overloaded = rate > 0 and (t["actual_update_rate"] < OVERLOAD_RATE_FRACTION * rate
                               or backlog_end > rate or rising)
    l0, l1 = t["lsm_at_t0"], t["lsm_at_end"]
    loaded = rate > 0
    return {
        "method": run["method"], "rate": rate, "target_rate": rate,
        "actual_rate": round(t["actual_update_rate"], 2) if loaded else 0,
        "qps_total": round(t["queries_completed_in_window"] / duration_s, 3),
        "qps_noload": "", "qps_ratio": "",
        "overloaded": bool(overloaded),
        "failed": bool(t.get("failed", False)),
        "queries_completed": t["queries_completed_in_window"],
        "mean_latency_ms": round(float(np.mean(lat)) / 1000, 3) if lat else "",
        "median_latency_ms": round(float(np.median(lat)) / 1000, 3) if lat else "",
        "updates_scheduled": t["updates_scheduled"] if loaded else "",
        "updates_issued": t["updates_issued"] if loaded else "",
        "unissued": t["updates_unissued"] if loaded else "",
        "backlog_at_end": backlog_end if loaded else "",
        "in_flight_at_end": t["queries_in_flight_at_end"],
        "drain_ms": t["drain_ms"],
        "wrapped": bool(t["wrapped"]),
        "stall_delays": l1["stall_delays"] - l0["stall_delays"],
        "stall_stops": l1["stall_stops"] - l0["stall_stops"],
        "flush_gb": round((l1["flush_bytes"] - l0["flush_bytes"]) / GB, 4),
        "compact_write_gb": round((l1["compact_write_bytes"] - l0["compact_write_bytes"]) / GB, 4),
        "matched_drift": matched_drift(run),
        "settle_ms": meta.get("settle", {}).get("total_ms", ""),
        "preload_ms": meta.get("preload", {}).get("ms", ""),
    }


def collect(result_dir):
    """({(method, rate): run row}, {(method, rate): [window rows]})."""
    runs, windows = {}, {}
    for fname in sorted(os.listdir(result_dir)):
        if fname == SWEEP_META or not fname.endswith("_meta.json"):
            continue
        m = META_PATTERN.match(fname)
        if not m:
            print(f"[skip] unrecognized run file: {fname}", file=sys.stderr)
            continue
        method, rate = m["method"], parse_rate(m["rate"])
        try:
            run = load_run(result_dir, fname[:-len("_meta.json")], method, rate)
        except (OSError, ValueError, KeyError) as e:
            print(f"[skip] {fname}: {e}", file=sys.stderr)
            continue
        ws = window_rows(run)
        windows[(method, rate)] = ws
        runs[(method, rate)] = run_summary(run, ws)
    for (method, rate), r in runs.items():
        noload = runs.get((method, 0))
        if noload and noload["qps_total"]:
            r["qps_noload"] = noload["qps_total"]
            r["qps_ratio"] = round(r["qps_total"] / noload["qps_total"], 4)
    return runs, windows


def query_set(result_dir):
    """The query set a sweep replayed, as the `c` of its file names
    (…_k<N>_…). Falls back to the directory's own name."""
    for fname in sorted(os.listdir(result_dir)):
        m = re.search(r"_k(\d+)_", fname)
        if m:
            return int(m.group(1))
    return os.path.basename(os.path.normpath(result_dir))


def write_combined(result_dir, collected):
    """One table across several sweeps: the per-sweep rows with a leading `c`
    column saying which query set they came from. `collected` is a list of
    (c, runs, windows). Written next to the last sweep's own CSVs."""
    wpath = os.path.join(result_dir, "queries_under_concurrent_updates_windows_all.csv")
    rpath = os.path.join(result_dir, "queries_under_concurrent_updates_summary_all.csv")
    with open(wpath, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["c"] + WINDOW_COLUMNS)
        w.writeheader()
        for c, _, windows in collected:
            for key in sorted(windows, key=lambda k: (k[1], k[0])):
                w.writerows(dict(row, c=c) for row in windows[key])
    with open(rpath, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["c"] + RUN_COLUMNS)
        w.writeheader()
        for c, runs, _ in collected:
            for key in sorted(runs, key=lambda k: (k[1], k[0])):
                w.writerow(dict(runs[key], c=c))
    return wpath, rpath


def write_csvs(result_dir, runs, windows):
    wpath = os.path.join(result_dir, "queries_under_concurrent_updates_windows.csv")
    rpath = os.path.join(result_dir, "queries_under_concurrent_updates_summary.csv")
    keys = sorted(runs, key=lambda k: (k[1], k[0]))
    with open(wpath, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=WINDOW_COLUMNS)
        w.writeheader()
        for k in keys:
            w.writerows(windows[k])
    with open(rpath, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=RUN_COLUMNS)
        w.writeheader()
        w.writerows(runs[k] for k in keys)
    return wpath, rpath


def main():
    ap = argparse.ArgumentParser(
        description="Reduce queries_under_concurrent_updates result directories to tidy CSVs")
    ap.add_argument("result_dirs", nargs="+")
    args = ap.parse_args()
    # One directory keeps its own CSVs; several produce the combined table
    # only, so a multi-sweep deliverable is a single pair of files.
    combining = len(args.result_dirs) > 1
    collected = []
    for result_dir in args.result_dirs:
        runs, windows = summarize_dir(result_dir, write=not combining)
        if runs:
            collected.append((query_set(result_dir), runs, windows))
    if not collected:
        return
    if len(collected) > 1:
        wpath, rpath = write_combined(args.result_dirs[-1], collected)
        print(f"Saved: {wpath}  ({sum(len(v) for _, _, ws in collected for v in ws.values())} windows)")
        print(f"Saved: {rpath}  ({sum(len(r) for _, r, _ in collected)} runs)")


def summarize_dir(result_dir, write=True):
    """Print one directory's runs, writing its CSVs unless the caller is
    combining several directories into one table. Returns (runs, windows)."""
    runs, windows = collect(result_dir)
    if not runs:
        print(f"No *_meta.json found in {result_dir}.")
        return {}, {}
    sweep_runs_path = os.path.join(result_dir, "sweep_runs.jsonl")
    if os.path.exists(sweep_runs_path):
        with open(sweep_runs_path) as f:
            n_success = sum(1 for line in f if line.strip()
                            and json.loads(line).get("rc") == 0)
        if n_success != len(runs):
            print(f"[warn] sweep_runs.jsonl has {n_success} successful runs but "
                  f"{len(runs)} were summarized", file=sys.stderr)
    if write:
        wpath, rpath = write_csvs(result_dir, runs, windows)
        print(f"Saved: {wpath}  ({sum(len(v) for v in windows.values())} windows)")
        print(f"Saved: {rpath}  ({len(runs)} runs)")
    for k in sorted(runs, key=lambda k: (k[1], k[0])):
        r = runs[k]
        flag = "  OVERLOADED" if r["overloaded"] else ""
        flag += "  FAILED" if r["failed"] else ""
        print(f"  {k[0]:28s} W={k[1]:>6}  actual={r['actual_rate']:>8}  "
              f"qps={r['qps_total']:>8}  ratio={r['qps_ratio']}{flag}")
    return runs, windows


if __name__ == "__main__":
    main()
