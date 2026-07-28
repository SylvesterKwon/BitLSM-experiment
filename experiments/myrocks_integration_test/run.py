#!/usr/bin/env python3
"""MyRocks integration test — plan-mode sweep (SSB-flat).

Tests the BitLSM x MyRocks integration end-to-end at the SQL level: index
selection (auto vs hinted plans), estimator quality (q-error vs cached
ground truth), and per-engine plan choices over the same data.

For each (engine, index_layout) cell: boot the identity-cached datadir,
then for each query x plan run one plan-mode cell (fresh session; EXPLAIN
JSON + optimizer_trace) and append a CSV row. Ground-truth COUNTs are
engine-independent (all engines load the byte-identical flat file) and are
cached once per (workload, query) under the data directory. Every engine's
own COUNT is re-run and verified against the cached truth (cross-engine
correctness gate) — any mismatch is reported and fails the sweep.

Usage:
    python3 experiments/myrocks_integration_test/run.py exp_set/<params>.json [options]
"""

import csv
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (  # noqa: E402
    add_common_args, make_result_dir, maybe_run_as_daemon,
    setup_logging, teardown_logging,
)
from myrocks import driver as drv  # noqa: E402
from myrocks.loader import datadir_for, ensure_loaded, is_loaded, server_for  # noqa: E402
from myrocks.workloads.registry import make_workload  # noqa: E402

CSV_FIELDS = [
    "ts", "workload", "engine", "index_layout", "query_id", "plan",
    "chosen_access", "chosen_key",
    "bi_chosen", "bi_est_rows", "actual_rows", "engine_rows", "count_match",
    "q_error", "query_cost",
    "explain_ms", "trace_bytes", "chosen_plan",
    "lsm_state", "server_args_hash", "mysql_commit", "bitlsm_commit",
]


def extract_where(sql: str):
    """Single-table flat queries only: text between WHERE and GROUP/ORDER."""
    m = re.search(r"\bWHERE\b(.*?)(\bGROUP BY\b|\bORDER BY\b|;|$)",
                  sql, re.S | re.I)
    return m.group(1).strip() if m else None


class GroundTruth:
    """(query_id -> actual matching rows), persisted next to the data so
    every engine and estimator iteration scores against the same truth.

    Doubles as the cross-engine correctness gate (H2b): all engines load
    the byte-identical flat file, so each engine's own COUNT must equal
    the cached truth. The first engine to see a query seeds the cache;
    every later engine verifies against it."""

    def __init__(self, workload):
        tables = workload.tables()
        assert len(tables) == 1, "GroundTruth assumes single-table workloads"
        self.table = tables[0]
        self.path = os.path.join(
            os.path.dirname(workload.data_file(self.table)),
            f"ground_truth.{workload.name}.json")
        self.cache = {}
        if os.path.exists(self.path):
            with open(self.path) as f:
                self.cache = json.load(f)

    def count_on_engine(self, sql, conn):
        """This engine's own matching-row count for the query's WHERE."""
        where = extract_where(sql)
        if where is None:
            return None
        cur = conn.cursor()
        cur.execute(f"SELECT COUNT(*) FROM {self.table} WHERE {where}")
        n = cur.fetchone()[0]
        cur.close()
        return n

    def check(self, qid, engine_count):
        """Returns (truth, match). Seeds the cache on first sight."""
        if engine_count is None:
            return self.cache.get(qid), None
        if qid not in self.cache:
            self.cache[qid] = engine_count
            with open(self.path, "w") as f:
                json.dump(self.cache, f, indent=2, sort_keys=True)
            return engine_count, True
        return self.cache[qid], self.cache[qid] == engine_count


def run(config_path, dry_run, start_from):
    with open(config_path) as f:
        config = json.load(f)

    exp_set = os.path.splitext(os.path.basename(config_path))[0]
    exp_name = os.path.basename(os.path.dirname(os.path.abspath(__file__)))
    exp_label = f"{exp_name}_{exp_set}"
    workload = make_workload(config)
    build_kind = config.get("build_kind", "debug")
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
        print(f"cells  : {len(cells)} x {len(queries)} queries -> {total} rows")
        print(f"output : {out_dir}")
        if dry_run:
            for c in cells:
                identity = workload.identity(c["engine"], c["index_layout"])
                state = "cached" if is_loaded(identity) else "NEEDS LOAD"
                print(f"  [{c['engine']}/{c['index_layout']}] {state} "
                      f"plans={c['plans']}")
            return

        os.makedirs(out_dir, exist_ok=True)
        csv_path = os.path.join(out_dir, "plan_mode.csv")
        truth = GroundTruth(workload)
        mismatches = []
        checked = 0
        idx = 0
        with open(csv_path, "w", newline="") as cf:
            writer = csv.DictWriter(cf, fieldnames=CSV_FIELDS)
            writer.writeheader()

            for cell in cells:
                engine, layout = cell["engine"], cell["index_layout"]
                identity = workload.identity(engine, layout)
                ensure_loaded(workload, engine, layout, build_kind)
                with open(os.path.join(datadir_for(identity),
                                       "LOADED.json")) as f:
                    marker = json.load(f)
                srv = server_for(workload, engine, layout, build_kind)
                # M5 A/B axis: "bitlsm_estimator": false pins the M4b sysvar
                # fallback (readonly server flag); default/true = estimator on.
                estimator_on = config.get("bitlsm_estimator", True)
                if engine == "bitlsm" and not estimator_on:
                    srv.extra_args.append("--rocksdb-bitlsm-estimator=0")
                sec_idx = workload.secondary_indexes(layout)
                with srv:
                    binfo = srv.build_info()
                    truth_conn = srv.connect(database=workload.name)
                    if engine == "bitlsm" and estimator_on:
                        # Measurement protocol (registry §2.5): force the
                        # estimator's stats current before any EXPLAIN, so
                        # estimates never race the async refresh worker.
                        # The tables MUST be opened first — on a cold server
                        # the registry is empty and refresh is a silent
                        # no-op (documented trap; caused an all-fallback
                        # q-error artifact in the 2026-07-28 rho sweep).
                        rc = truth_conn.cursor()
                        for t in workload.tables():
                            rc.execute(f"SELECT 1 FROM {t} LIMIT 1")
                            rc.fetchall()
                        rc.execute(
                            "SET GLOBAL rocksdb_bitlsm_estimator_refresh = 1")
                        rc.close()
                    for qid, sql in queries.items():
                        engine_rows = truth.count_on_engine(sql, truth_conn)
                        actual, match = truth.check(qid, engine_rows)
                        if match is not None:
                            checked += 1
                            if not match:
                                mismatches.append(
                                    (engine, layout, qid, engine_rows, actual))
                                print(f"COUNT MISMATCH {engine}/{layout} "
                                      f"{qid}: engine={engine_rows} "
                                      f"truth={actual}")
                        for plan in cell["plans"]:
                            idx += 1
                            if idx < start_from:
                                continue
                            trace_path = os.path.join(
                                out_dir, "traces",
                                f"{engine}-{layout}-{qid}-{plan}.json")
                            row = drv.run_cell(
                                srv, workload.name, qid, sql, plan,
                                secondary_indexes=sec_idx,
                                session_vars=cell.get("session_vars"),
                                trace_path=trace_path)
                            row["actual_rows"] = actual
                            row["engine_rows"] = engine_rows
                            row["count_match"] = (None if match is None
                                                  else int(match))
                            if row["bi_est_rows"] and actual:
                                row["q_error"] = round(max(
                                    row["bi_est_rows"] / actual,
                                    actual / row["bi_est_rows"]), 4)
                            row.update({
                                "ts": time.strftime("%H:%M:%S"),
                                "workload": workload.name,
                                "engine": engine,
                                "index_layout": layout,
                                "lsm_state": marker.get("lsm_state"),
                                "server_args_hash": srv.args_hash(),
                                "mysql_commit": binfo["mysql_commit"][:12],
                                "bitlsm_commit": binfo["bitlsm_commit"][:12],
                            })
                            writer.writerow(row)
                            cf.flush()
                            print(f"[{idx}/{total}] {engine}/{layout} "
                                  f"{qid} {plan}: "
                                  f"access={row['chosen_access']} "
                                  f"key={row['chosen_key']} "
                                  f"est={row['bi_est_rows']} act={actual} "
                                  f"qerr={row['q_error']}")
                    truth_conn.close()
        print(f"crosscheck: {checked} engine-count checks, "
              f"{len(mismatches)} mismatches")
        if mismatches:
            for m in mismatches:
                print(f"  MISMATCH {m[0]}/{m[1]} {m[2]}: "
                      f"engine={m[3]} truth={m[4]}")
        print(f"done -> {csv_path}")
        return 1 if mismatches else 0
    finally:
        teardown_logging(log_file, log_path)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="MyRocks integration test sweep")
    add_common_args(parser)
    args = parser.parse_args()
    maybe_run_as_daemon(args)
    sys.exit(run(args.config, dry_run=args.dry_run,
                 start_from=args.start_from))


if __name__ == "__main__":
    main()
