#!/usr/bin/env python3
"""MyRocks integration test — perf-mode sweep (H3, harness plan D3/D7).

Measures actual execution latency + counters per (engine, index_layout,
query, plan) cell on the RELEASE build. Cold protocol per cell: a fresh
server boot empties the engine cache, then 1 cold + K warm executions, stop.
Under D7 direct I/O the restart is the whole cold mechanism — the OS page
cache is not on the read path, so there is nothing else to invalidate.

D7 measurement hygiene (enforced here):
  - build_kind must be "release" (debug wallclock is meaningless)
  - direct I/O on all engines (innodb O_DIRECT / rocksdb use_direct_reads)
    so the OS page cache is out of the experiment
  - unified 1G cache budget (buffer pool == block cache)
  - measurement args differ from load args by design; args_hash records
    them per row (datadir identity stays tied to load args)

Result fingerprints extend the H2 COUNT gate to full result sets: every
query must produce the identical (order-independent) result on every
engine and plan — any mismatch fails the sweep.

Usage:
    python3 experiments/myrocks_integration_test/perf_run.py exp_set/<params>.json [options]
"""

import csv
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (  # noqa: E402
    add_common_args, make_result_dir, maybe_run_as_daemon,
    setup_logging, teardown_logging,
)
from myrocks import metrics, server_profile  # noqa: E402
from myrocks.loader import datadir_for, ensure_loaded  # noqa: E402
from myrocks.server import MysqldServer  # noqa: E402
from myrocks.workloads.registry import make_workload  # noqa: E402

CSV_FIELDS = [
    "ts", "workload", "engine", "index_layout", "engine_params",
    "query_id", "plan",
    "chosen_access", "chosen_key",
    "cold_ms", "warm_ms_median", "warm_ms_min", "warm_ms_max", "warm_reps",
    "rows_returned", "result_fingerprint", "fp_match",
] + metrics.KEY_COUNTERS + [
    "lsm_state", "server_args_hash", "mysql_commit", "bitlsm_commit",
]


def run(config_path, dry_run, start_from):
    with open(config_path) as f:
        config = json.load(f)
    if config.get("build_kind") != "release":
        raise SystemExit("perf-mode requires build_kind=release (D7) — "
                         f"got {config.get('build_kind')!r}")

    exp_set = os.path.splitext(os.path.basename(config_path))[0]
    exp_name = os.path.basename(os.path.dirname(os.path.abspath(__file__)))
    exp_label = f"{exp_name}_{exp_set}"
    workload = make_workload(config)
    profile = server_profile.resolve(config.get("server_common"))
    if not profile["direct_io"]:
        # The cold protocol is a server restart and nothing else. That only
        # empties the engine cache, so without direct I/O the OS page cache
        # survives every cell and no run after the first is cold.
        raise SystemExit("perf-mode requires direct_io (D7) — got false")
    warm_reps = config.get("warm_reps", 5)
    queries = workload.queries()
    if config.get("queries", "all") != "all":
        queries = {q: queries[q] for q in config["queries"]}
    cells = config["cells"]
    total = sum(len(queries) * len(c["plans"]) for c in cells)

    out_dir = make_result_dir(exp_label)
    log_file, log_path = setup_logging(exp_label)
    try:
        print(f"config : {config_path}")
        print(f"label  : {exp_label}")
        print(f"cells  : {total} (warm_reps={warm_reps})")
        print(f"server : {profile}")
        print(f"output : {out_dir}")
        if dry_run:
            for c in cells:
                print(f"  [{c['engine']}/{c['index_layout']}] "
                      f"plans={c['plans']} args="
                      f"{server_profile.build_args(c['engine'], profile)}")
            return 0

        os.makedirs(out_dir, exist_ok=True)
        csv_path = os.path.join(out_dir, "perf_mode.csv")
        fp_seen = {}      # query_id -> first fingerprint (engine/plan-blind)
        fp_mismatch = []
        idx = 0
        with open(csv_path, "w", newline="") as cf:
            writer = csv.DictWriter(cf, fieldnames=CSV_FIELDS)
            writer.writeheader()
            for cell in cells:
                engine, layout = cell["engine"], cell["index_layout"]
                eparams = cell.get("engine_params")
                identity = workload.identity(engine, layout, eparams)
                ensure_loaded(workload, engine, layout, "release", eparams)
                with open(os.path.join(datadir_for(identity),
                                       "LOADED.json")) as f:
                    marker = json.load(f)
                sec_idx = workload.secondary_indexes(layout)
                for qid, sql in queries.items():
                    for plan in cell["plans"]:
                        idx += 1
                        if idx < start_from:
                            continue
                        srv = MysqldServer(datadir_for(identity), "release",
                                           extra_args=server_profile.build_args(
                                               engine, profile))
                        with srv:
                            # Warm-up before the timed cold run, all engines
                            # symmetric: open each table (one-row SELECT) so
                            # cold_ms measures query I/O, not dictionary /
                            # table-open overhead. For bitlsm additionally
                            # force the estimator stats current (synchronous
                            # rebuild, ~1.3s/boot measured): InnoDB/MyRocks
                            # ANALYZE stats persist and are warm at boot,
                            # while the bitlsm estimator builds async ~3-5s
                            # after first open — without this every auto plan
                            # is chosen against the sysvar fallback (and can
                            # differ from the EXPLAIN recorded after the
                            # runs). Touches one row + SABI metadata only —
                            # the data cache stays cold for the timed run.
                            wconn = srv.connect(database=workload.name)
                            wcur = wconn.cursor()
                            # LOCK TABLES READ instantiates every handler
                            # (triggering lazy estimator attach) while reading
                            # zero rows by construction -- the cold run's data
                            # blocks stay untouched.
                            wcur.execute("LOCK TABLES " + ", ".join(
                                f"{t} READ" for t in workload.tables()))
                            wcur.execute("UNLOCK TABLES")
                            if engine == "bitlsm":
                                wcur.execute("SET GLOBAL "
                                             "rocksdb_bitlsm_estimator_refresh"
                                             " = 1")
                            wcur.close()
                            wconn.close()
                            row = metrics.run_perf_cell(
                                srv, workload.name, qid, sql, plan,
                                secondary_indexes=sec_idx,
                                session_vars=cell.get("session_vars"),
                                warm_reps=warm_reps,
                                sidecar_path=os.path.join(
                                    out_dir, "counters",
                                    f"{engine}-{layout}-{qid}-{plan}.json"))
                            binfo = srv.build_info()
                            args_hash = srv.args_hash()
                        fp = row["result_fingerprint"]
                        if qid not in fp_seen:
                            fp_seen[qid] = fp
                        row["fp_match"] = int(fp == fp_seen[qid])
                        if not row["fp_match"]:
                            fp_mismatch.append((engine, layout, qid, plan))
                            print(f"RESULT MISMATCH {engine}/{layout} "
                                  f"{qid} {plan}")
                        row.update({
                            "ts": time.strftime("%H:%M:%S"),
                            "workload": workload.name,
                            "engine": engine,
                            "index_layout": layout,
                            "engine_params": ";".join(
                                f"{k}={v}" for k, v in
                                sorted((eparams or {}).items())),
                            "lsm_state": marker.get("lsm_state"),
                            "server_args_hash": args_hash,
                            "mysql_commit": binfo["mysql_commit"][:12],
                            "bitlsm_commit": binfo["bitlsm_commit"][:12],
                        })
                        writer.writerow(row)
                        cf.flush()
                        print(f"[{idx}/{total}] {engine}/{layout} {qid} "
                              f"{plan}: cold={row['cold_ms']}ms "
                              f"warm={row['warm_ms_median']}ms "
                              f"access={row['chosen_access']} "
                              f"key={row['chosen_key']}")
        print(f"result crosscheck: {len(fp_mismatch)} mismatches")
        for m in fp_mismatch:
            print(f"  MISMATCH {m[0]}/{m[1]} {m[2]} {m[3]}")
        print(f"done -> {csv_path}")
        return 1 if fp_mismatch else 0
    finally:
        teardown_logging(log_file, log_path)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="MyRocks integration test perf sweep")
    add_common_args(parser)
    args = parser.parse_args()
    maybe_run_as_daemon(args)
    sys.exit(run(args.config, dry_run=args.dry_run,
                 start_from=args.start_from))


if __name__ == "__main__":
    main()
