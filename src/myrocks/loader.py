"""Per-identity datadir loading (harness plan D2: datadir caching).

ensure_loaded() is idempotent: identity -> deterministic datadir under
data/sql/db/; init + CREATE + LOAD + ANALYZE run once, marked by LOADED.json.
Subsequent runs just boot the cached datadir (seconds).
"""

import json
import os
import re
import shutil
import time

from . import server as _server
from .server import MysqldServer

# One args set per engine, used for BOTH load and measurement runs so the
# datadir identity stays tied to a single server configuration.
#
# Symmetric by the same rule the measurement profile follows: the same cache
# budget, the same commit-flush relaxation, and neither engine throttled below
# what the device can sustain. Every value here is set explicitly for both
# sides -- relying on a compiled default on either engine is what produces a
# one-sided setting, and a one-sided commit flush in particular turns the LSM
# engines fsync-bound (see server_profile.py's docstring). Load time is not a
# reported metric, but a fixture should not be built under a configuration the
# measurement would reject.
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
    # Read through the module rather than a name bound at import time, so
    # server.DB_BASE is the single source of the fixture root. A copy taken
    # here would keep pointing at the default after a caller relocated it,
    # and the load would land somewhere other than where it is read from.
    return os.path.join(_server.DB_BASE, identity)


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



# Building a B-tree's secondary indexes by inserting into them during the load
# is what makes an InnoDB fixture take days: every row descends every index at
# a random position, and once the change buffer fills at its 25%-of-pool cap
# the absorbing stops. Creating the table with its primary key only, loading,
# and adding the secondary indexes afterwards lets InnoDB build them by sorting
# instead -- no random descent, and the pages come out full.
#
# This is not an advantage handed to one engine. The LSM fixtures already end
# up sorted and densely packed, because compaction rewrites them that way
# before lsm_state=settled is recorded; sorting InnoDB's indexes is how a
# B-tree reaches the same kind of state. Leaving them insert-built is the
# asymmetry, not removing it.
#
# Load time is not a reported metric (the write axis is measured by
# ingest_run.py, which is untouched), so what matters here is the datadir this
# produces, and it is the same table with the same indexes.
_KEY_RE = re.compile(r"^\s*(?:UNIQUE\s+)?KEY\s+`?(\w+)`?\s*\(([^)]*)\)\s*,?\s*$",
                     re.IGNORECASE)


def _split_secondary_indexes(ddl: str):
    """(CREATE TABLE without its KEY clauses, [ADD INDEX fragments]).

    Returns the ddl unchanged and an empty list when there is nothing to
    split, so a caller can always use the result.
    """
    kept, adds = [], []
    for line in ddl.splitlines():
        m = _KEY_RE.match(line)
        if m:
            adds.append(f"ADD INDEX {m.group(1)} ({m.group(2).strip()})")
        else:
            kept.append(line)
    if not adds:
        return ddl, []
    # The line before the closing paren now ends in a stray comma.
    for i in range(len(kept) - 1, -1, -1):
        t = kept[i].rstrip()
        if t.endswith(","):
            kept[i] = t[:-1]
            break
    return "\n".join(kept), adds


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

        rocks = engine in ("myrocks", "bitlsm")
        # Deferred only for the B-tree: the LSM engines reach a sorted, densely
        # packed state through compaction, and BitLSM's bitmap has to be
        # present during the load to be built at all.
        deferred = {}
        for table in workload.tables():
            ddl = workload.create_table_sql(table, engine, index_layout)
            if not rocks:
                ddl, adds = _split_secondary_indexes(ddl)
                if adds:
                    deferred[table] = adds
            cur.execute(ddl)

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

        for table, adds in deferred.items():
            t1 = time.time()
            log(f"[load:{identity}] {table}: building {len(adds)} indexes ...")
            cur.execute(f"ALTER TABLE {table} " + ", ".join(adds))
            log(f"[load:{identity}] {table}: indexes built "
                f"({time.time() - t1:.1f}s)")

        want_total = getattr(workload, "EXPECTED_TOTAL_ROWS", None)
        if want_total is not None and sum(counts.values()) != want_total:
            raise RuntimeError(
                f"total rows {sum(counts.values())} != canonical "
                f"{want_total} — aborting")

        # LSM state for read-only measurement (D2: lsm_state=settled).
        #
        # Flush, then let leveled compaction reach its own fixed point: the
        # shape the policy settles into once the level size targets are
        # satisfied. Deliberately not a manual full-range compaction with
        # bottommost rewriting -- that leaves the tree in the best read shape
        # it can ever have, one no running system is in, and measuring reads
        # there flatters every LSM cell.
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
