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
#
# Symmetric by the same rule the measurement profile follows: the same cache
# budget, the same commit-flush relaxation, and neither engine throttled below
# what the device can sustain. This used to give InnoDB a 2 GiB buffer pool and
# a relaxed commit flush while leaving the LSM engines on their defaults -- a
# 512 MiB block cache and an fsync on every commit, which is the exact
# one-sided flush setting server_profile.py's docstring warns turns the LSM
# engines fsync-bound. Load time is not a reported metric, but a fixture should
# not be built under a configuration the measurement would reject.
LOAD_CACHE_BYTES = 2 << 30      # 2 GiB, both engines
LOAD_IO_CAPACITY = 1000         # see server_profile.DEFAULTS for the rationale
LOAD_IO_CAPACITY_MAX = 2500
LOAD_LSM_BACKGROUND_JOBS = 6    # see server_profile.DEFAULTS for the rationale

_COMMON = ["--skip-log-bin",
           "--character-set-server=latin1",
           "--collation-server=latin1_swedish_ci"]
SERVER_ARGS = {
    "innodb": _COMMON + [
        f"--innodb-buffer-pool-size={LOAD_CACHE_BYTES}",
        "--innodb-flush-log-at-trx-commit=0",
        f"--innodb-io-capacity={LOAD_IO_CAPACITY}",
        f"--innodb-io-capacity-max={LOAD_IO_CAPACITY_MAX}"],
    # rocksdb_rate_limiter_bytes_per_sec already defaults to 0 (unlimited),
    # so the LSM engines need no counterpart to io-capacity here.
    "myrocks": _COMMON + [
        f"--rocksdb-block-cache-size={LOAD_CACHE_BYTES}",
        "--rocksdb-flush-log-at-trx-commit=0",
        f"--rocksdb-max-background-jobs={LOAD_LSM_BACKGROUND_JOBS}"],
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


# ROCKSDB_CFSTATS counters that are non-zero while the LSM still has
# outstanding background work. Counters and flags only -- the pending-byte
# estimate is a size, not a count, so it is reported but never summed in.
SETTLE_COUNTERS = ("COMPACTION_PENDING", "NUM_RUNNING_COMPACTIONS",
                   "MEM_TABLE_FLUSH_PENDING", "NUM_RUNNING_FLUSHES")
SETTLE_BYTES = "ESTIMATE_PENDING_COMPACTION_BYTES"
SETTLE_POLL_S = 2.0
# One zero reading can be the gap between two scheduled jobs, so require the
# backlog to stay empty for a while before calling the tree settled.
SETTLE_QUIET_S = 30.0
SETTLE_TIMEOUT_S = 3600.0


def _cfstats_sum(cur, stat_types) -> int:
    ph = ", ".join(["%s"] * len(stat_types))
    cur.execute(
        "SELECT COALESCE(SUM(VALUE), 0) FROM information_schema.ROCKSDB_CFSTATS"
        f" WHERE STAT_TYPE IN ({ph})", tuple(stat_types))
    row = cur.fetchone()
    return int(row[0]) if row and row[0] is not None else 0


def _wait_until_settled(cur, log, tag: str) -> str:
    """Wait for leveled compaction to reach its own fixed point.

    Issues no manual compaction: it only blocks until RocksDB reports no
    pending or running compactions/flushes across every column family, which
    is what DB::WaitForCompact() does natively. Returns the lsm_state marker.
    """
    t0 = time.time()
    quiet_since = None
    while True:
        backlog = _cfstats_sum(cur, SETTLE_COUNTERS)
        now = time.time()
        if backlog:
            quiet_since = None
        else:
            if quiet_since is None:
                quiet_since = now
            elif now - quiet_since >= SETTLE_QUIET_S:
                pending = _cfstats_sum(cur, (SETTLE_BYTES,))
                log(f"[load:{tag}] lsm settled in {now - t0:.1f}s "
                    f"(pending compaction bytes: {pending})")
                return "settled"
        if now - t0 > SETTLE_TIMEOUT_S:
            log(f"[load:{tag}] lsm still busy after {now - t0:.0f}s "
                f"(backlog={backlog}) — recording settled-timeout")
            return "settled-timeout"
        time.sleep(SETTLE_POLL_S)


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

        # LSM state for read-only measurement (D2: lsm_state=settled).
        #
        # This used to issue rocksdb_compact_cf per CF: a full-range manual
        # compaction with bottommost rewriting, which leaves the tree in the
        # best read shape it can ever have -- one no running system is in.
        # Measuring reads there flatters every LSM cell, so instead we flush
        # and let leveled compaction reach its own fixed point: the shape the
        # policy settles into once the level size targets are satisfied.
        #
        # Same choice the C++ bindings already make in WaitForQuiescence()
        # ("flush + WaitForCompact, no manual compaction", 338cb97). MyRocks
        # exposes no sysvar for DB::WaitForCompact(), so we poll the per-CF
        # properties it does publish through ROCKSDB_CFSTATS.
        lsm_state = "n/a"
        if rocks:
            cur.execute("SET GLOBAL rocksdb_force_flush_memtable_now = 1")
            lsm_state = _wait_until_settled(cur, log, identity)

        for table in workload.tables():
            cur.execute(f"ANALYZE TABLE {table}")
            cur.fetchall()

        # engine_params {"hist": <tag>}: server-level column histograms
        # (default bucket count) on the workload's filter columns. Standard
        # for SK-engine cells since 2026-07-29 (stats fairness: bitlsm gets
        # its estimator, SK engines get histograms). Note the optimizer only
        # consults histograms where index dives can't answer (non-indexed
        # predicate columns) — on fully-indexed filter sets this is a
        # deliberate null-op (verified: identical plans on taxpayer).
        # Identity-forked so histogram-free baselines stay intact.
        hist = "off"
        if engine in ("innodb", "myrocks") and (engine_params or {}).get("hist"):
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
