"""Synthetic queries_under_concurrent_updates result directory for the summarize and
plot tests. Every number is chosen so the expected statistics are exact:
T = 6 s in 2 s windows, three queries in the set, one query finishing every
500 ms at 400 us each -> 4 queries per window, QPS 2.0, four complete passes.
"""

import json
import os

QUERY_COUNT = 3
DURATION_S = 6
WINDOW_S = 2
METHODS = ("bitlsm_rho0.001", "embedded-postings_il2", "embedded-postings_il0")

LSM_KEYS = ["running_flushes", "running_compactions", "flush_pending",
            "compaction_pending", "pending_compaction_bytes", "l0_files",
            "memtable_bytes", "immutable_memtables", "write_stopped",
            "delayed_write_rate", "stall_stops", "stall_delays", "flush_bytes",
            "compact_read_bytes", "compact_write_bytes", "flush_count",
            "compaction_count"]


def _lsm_row(t_ms):
    s = t_ms // 1000
    return {"running_flushes": 0, "running_compactions": 1 if s == 6 else 0,
            "flush_pending": 0, "compaction_pending": 0,
            "pending_compaction_bytes": t_ms * 1000, "l0_files": s,
            "memtable_bytes": 1048576, "immutable_memtables": 0,
            "write_stopped": 0, "delayed_write_rate": 0, "stall_stops": 0,
            "stall_delays": s, "flush_bytes": t_ms * 100,
            "compact_read_bytes": 0, "compact_write_bytes": t_ms * 200,
            "flush_count": s, "compaction_count": 0}


def write_run(root, method, rate, actual_rate=None, tail_backlog=(0, 0, 0),
              matched_last_pass=100):
    """One run's four files. `tail_backlog` is the writer's backlog at the end
    of each of the three windows; `actual_rate` defaults to `rate`."""
    prefix = os.path.join(root, f"read_seq_sel0.0001_k3_r300_{method}_w{rate:g}")
    n = DURATION_S * 2  # queries completed inside T
    last_pass = n // QUERY_COUNT - 1
    with open(prefix + "_query_log.csv", "w") as f:
        f.write("seq,pass,query_id,query_attr_num,filter_attrs,start_ms,end_ms,"
                "latency_us,records_matched,in_window\n")
        for i in range(n + 1):  # +1: one query finishes after T
            p, qid = divmod(i, QUERY_COUNT)
            end_ms = i * 500 + 400
            in_window = 1 if end_ms <= DURATION_S * 1000 else 0
            matched = matched_last_pass if p >= last_pass else 100
            f.write(f"{i},{p},{qid},3,\"a,b,c\",{end_ms - 1},{end_ms},400,"
                    f"{matched},{in_window}\n")

    with open(prefix + "_lsm_log.csv", "w") as f:
        f.write("t_ms," + ",".join(LSM_KEYS) + ",rss_kb\n")
        for t_ms in range(0, DURATION_S * 1000 + 1, 1000):
            row = _lsm_row(t_ms)
            f.write(f"{t_ms}," + ",".join(str(row[k]) for k in LSM_KEYS) + ",1000\n")

    unissued = 0
    if rate > 0:
        with open(prefix + "_write_log.csv", "w") as f:
            f.write("sec,scheduled_cum,issued_cum,backlog_end,lag_mean_us,"
                    "lag_max_us,put_mean_us,put_max_us,put_count\n")
            for sec in range(DURATION_S):
                backlog = tail_backlog[sec // WINDOW_S] if sec % WINDOW_S == WINDOW_S - 1 else 0
                scheduled = rate * (sec + 1)
                f.write(f"{sec},{scheduled},{scheduled - backlog},{backlog},"
                        f"{10 * (sec + 1)},20,15,30,{rate}\n")
        unissued = tail_backlog[-1]

    lsm_t0 = {k: 0 for k in LSM_KEYS}
    lsm_end = _lsm_row(DURATION_S * 1000)
    lsm_end["stall_stops"] = 1
    meta = {
        "binding": method.split("_")[0], "update_rate": rate,
        "duration_s": DURATION_S, "window_s": WINDOW_S, "seed": 42,
        "query_count": QUERY_COUNT,
        "preload": {"wanted": rate * DURATION_S, "loaded": rate * DURATION_S, "ms": 5},
        "settle": {"scan_ms": 3, "total_ms": 7},
        "totals": {
            "queries_completed_in_window": n, "queries_in_flight_at_end": 1,
            "queries_started": n + 1, "qps": n / DURATION_S,
            "actual_update_rate": rate if actual_rate is None else actual_rate,
            "updates_scheduled": rate * DURATION_S,
            "updates_issued": rate * DURATION_S - unissued,
            "updates_unissued": unissued, "drain_ms": 3, "wrapped": False,
            "verify_failures": 0, "failed": False,
            "lsm_at_t0": lsm_t0, "lsm_at_end": lsm_end, "lsm_after_drain": lsm_end,
        },
    }
    with open(prefix + "_meta.json", "w") as f:
        json.dump(meta, f, indent=1)
    return prefix


def make_result_dir(root, overloaded_method=None, rising_method=None):
    """Three methods at W = 0 and W = 100. `overloaded_method` runs at 90/s
    (rate rule); `rising_method` gets an extra W = 200 run whose backlog rises
    1, 2, 3 over the last three windows (tail rule) while its rate holds."""
    os.makedirs(root, exist_ok=True)
    for m in METHODS:
        write_run(root, m, 0)
        write_run(root, m, 100, actual_rate=90 if m == overloaded_method else None,
                  matched_last_pass=110)
    if rising_method:
        write_run(root, rising_method, 200, tail_backlog=(1, 2, 3))
    return root
