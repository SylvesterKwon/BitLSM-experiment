"""Per-identity datadir loading (harness plan D2: datadir caching).

ensure_loaded() is idempotent: identity -> deterministic datadir under
data/sql/db/; init + CREATE + LOAD + ANALYZE run once, marked by LOADED.json.
Subsequent runs just boot the cached datadir (seconds).
"""

import json
import os
import shutil
import time

from .server import DB_BASE, MysqldServer

# One args set per engine, used for BOTH load and measurement runs so the
# datadir identity stays tied to a single server configuration.
SERVER_ARGS = {
    "innodb": ["--skip-log-bin",
               "--character-set-server=latin1",
               "--collation-server=latin1_swedish_ci",
               "--innodb-buffer-pool-size=2G",
               "--innodb-flush-log-at-trx-commit=0"],
    "myrocks": ["--skip-log-bin",
                "--character-set-server=latin1",
                "--collation-server=latin1_swedish_ci"],
}
SERVER_ARGS["bitlsm"] = SERVER_ARGS["myrocks"]

MARKER = "LOADED.json"


def datadir_for(identity: str) -> str:
    return os.path.join(DB_BASE, identity)


def is_loaded(identity: str) -> bool:
    return os.path.exists(os.path.join(datadir_for(identity), MARKER))


def server_for(workload, engine: str, index_layout: str,
               build_kind: str = "debug", engine_params: dict = None
               ) -> MysqldServer:
    identity = workload.identity(engine, index_layout, engine_params)
    return MysqldServer(datadir_for(identity), build_kind,
                        extra_args=SERVER_ARGS[engine])


def ensure_loaded(workload, engine: str, index_layout: str,
                  build_kind: str = "debug", engine_params: dict = None,
                  log=print) -> str:
    identity = workload.identity(engine, index_layout, engine_params)
    datadir = datadir_for(identity)
    if is_loaded(identity):
        return datadir

    log(f"[load:{identity}] verifying data against MANIFEST ...")
    workload.verify_data()

    if os.path.exists(datadir):
        log(f"[load:{identity}] incomplete previous load — rebuilding")
        shutil.rmtree(datadir)

    t0 = time.time()
    srv = server_for(workload, engine, index_layout, build_kind, engine_params)
    counts = {}
    with srv:
        conn = srv.connect()
        cur = conn.cursor()
        cur.execute(f"CREATE DATABASE {workload.name}")
        cur.execute(f"USE {workload.name}")

        for table in workload.tables():
            cur.execute(workload.create_table_sql(table, engine, index_layout))

        rocks = engine in ("myrocks", "bitlsm")
        for table in workload.tables():
            csv_path = workload.data_file(table)
            # bulk load writes SSTs directly and would bypass the memtable
            # path that builds the SABI bitmap — bi tables load normally.
            bulk = rocks and table not in workload.bi_tables(index_layout)
            if bulk:
                cur.execute("SET SESSION rocksdb_bulk_load_allow_unsorted=1")
                cur.execute("SET SESSION rocksdb_bulk_load_allow_sk=1")
                cur.execute("SET SESSION rocksdb_bulk_load=1")
            elif rocks:
                # Non-bulk (bi) path: a 6M+-row LOAD in one transaction blows
                # rocksdb_max_row_locks; periodic auto-commit releases locks.
                # Partial commits are safe — any failure rebuilds the datadir.
                cur.execute("SET SESSION rocksdb_commit_in_the_middle=1")
            t1 = time.time()
            cur.execute(workload.load_sql(table, csv_path))
            if bulk:
                cur.execute("SET SESSION rocksdb_bulk_load=0")
            cur.execute(f"SELECT COUNT(*) FROM {table}")
            counts[table] = cur.fetchone()[0]
            log(f"[load:{identity}] {table}: {counts[table]} rows "
                f"({time.time() - t1:.1f}s)")
            if counts[table] == 0:
                raise RuntimeError(f"{table} loaded 0 rows — aborting")
            want = getattr(workload, "EXPECTED_ROWS", {}).get(table)
            if want is not None and counts[table] != want:
                raise RuntimeError(
                    f"{table}: loaded {counts[table]} rows, expected {want} "
                    f"(canonical count for the frozen snapshot) — aborting")

        want_total = getattr(workload, "EXPECTED_TOTAL_ROWS", None)
        if want_total is not None and sum(counts.values()) != want_total:
            raise RuntimeError(
                f"total rows {sum(counts.values())} != canonical "
                f"{want_total} — aborting")

        # Deterministic LSM state for read-only measurement (D2:
        # lsm_state=compacted). natural-state runs are a separate H5 axis.
        lsm_state = "n/a"
        if rocks:
            cur.execute("SET GLOBAL rocksdb_force_flush_memtable_now = 1")
            cur.execute("SET GLOBAL rocksdb_compact_cf = 'default'")
            lsm_state = "compacted"

        for table in workload.tables():
            cur.execute(f"ANALYZE TABLE {table}")
            cur.fetchall()

        # engine_params {"hist": <tag>}: InnoDB column histograms (default
        # bucket count) on the workload's filter columns — lets the optimizer
        # see value skew that rec_per_key averages hide. Identity-forked so
        # the histogram-free baseline datadir stays intact.
        hist = "off"
        if engine == "innodb" and (engine_params or {}).get("hist"):
            cols = workload.histogram_columns()
            if not cols:
                raise RuntimeError("hist engine_param set but workload "
                                   "defines no histogram_columns()")
            for table in workload.tables():
                cur.execute(f"ANALYZE TABLE {table} UPDATE HISTOGRAM ON "
                            + ", ".join(cols))
                cur.fetchall()
            hist = f"on:{engine_params['hist']}"

        marker = {
            "identity": identity,
            "workload": workload.name,
            "engine": engine,
            "index_layout": index_layout,
            "engine_params": engine_params or {},
            "row_counts": counts,
            "server_args_hash": srv.args_hash(),
            "build_info": srv.build_info(),
            "innodb_histograms": hist,
            "lsm_state": lsm_state,
            "load_seconds": round(time.time() - t0, 1),
            "loaded_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        }
        cur.close()
        conn.close()
    # written after clean shutdown so a crashed load never looks complete
    with open(os.path.join(datadir, MARKER), "w") as f:
        json.dump(marker, f, indent=2)
    log(f"[load:{identity}] done in {marker['load_seconds']}s")
    return datadir
