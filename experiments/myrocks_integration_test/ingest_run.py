#!/usr/bin/env python3
"""MyRocks integration test — row-level ingest sweep (H4, harness plan §5.5).

Measures per-row index maintenance cost: N writer processes stream real
flat-file rows as individual INSERT statements (autocommit, c-ext +
prepared — client ceiling verified 2.5x above the fastest engine) into a
fresh throwaway datadir per run. Maintenance cost is isolated by
subtracting the same engine's PK-only (std) run.

Server settings mirror the read benchmark (D7: unified 1G cache, direct
I/O) extended symmetrically to the write path: BOTH engines get
flush_log_at_trx_commit=0 — an asymmetric flush setting turns the LSM
engines fsync-bound (~142 rows/s) and masks everything.

Per run: fresh datadir -> DDL -> N writers over disjoint row ranges ->
aggregate rows/s + per-writer checkpoint timeline -> drop datadir.

Usage:
    python3 experiments/myrocks_integration_test/ingest_run.py exp_set/<params>.json [options]
"""

import csv
import json
import multiprocessing
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (  # noqa: E402
    add_common_args, make_result_dir, maybe_run_as_daemon,
    setup_logging, teardown_logging,
)
from myrocks import server_profile  # noqa: E402
from myrocks.server import DB_BASE, MysqldServer  # noqa: E402
from myrocks.workloads.ssb_flat import FLAT_COLUMNS, SsbFlatWorkload  # noqa: E402

CSV_FIELDS = [
    "ts", "workload", "engine", "index_layout", "writers", "rows",
    "seconds", "rows_per_sec", "shuffle",
    "server_args_hash", "mysql_commit", "bitlsm_commit",
]

TABLE = "lineorder_flat"
CHECKPOINT_ROWS = 10_000


def writer_proc(socket, database, data_path, start, count, report_path):
    """One writer: stream rows [start, start+count) of the data file as
    individual prepared INSERTs, autocommit each. Checkpoints
    (wallclock, rows_done) every CHECKPOINT_ROWS for the timeline."""
    import mysql.connector
    conn = mysql.connector.connect(unix_socket=socket, user="root",
                                   password="", database=database,
                                   use_pure=False)
    conn.autocommit = True
    cur = conn.cursor(prepared=True)
    cols = ", ".join(c for c, _, _ in FLAT_COLUMNS)
    ph = ", ".join(["%s"] * len(FLAT_COLUMNS))
    sql = f"INSERT INTO {TABLE} ({cols}) VALUES ({ph})"
    done = 0
    with open(report_path, "w") as rep, open(data_path) as f:
        for _ in range(start):
            next(f)
        for _ in range(count):
            cur.execute(sql, tuple(next(f).rstrip("\n").split("|")))
            done += 1
            if done % CHECKPOINT_ROWS == 0:
                rep.write(f"{time.time():.3f},{done}\n")
                rep.flush()
        rep.write(f"{time.time():.3f},{done}\n")
    cur.close()
    conn.close()


def shuffled_copy(workload, seed: int) -> str:
    """Materialize (once) a deterministically shuffled copy of the flat
    file next to it; identity = (flat file, seed)."""
    import random
    path = f"{workload.flat_path}.shuffled.s{seed}"
    if os.path.exists(path):
        return path
    with open(workload.flat_path) as f:
        lines = f.readlines()
    random.Random(seed).shuffle(lines)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.writelines(lines)
    os.replace(tmp, path)
    return path


def run_one(workload, engine, layout, n_writers, n_rows, data_path,
            build_kind, out_dir, tag, profile):
    """One ingest run on a throwaway datadir; returns the result row."""
    datadir = os.path.join(DB_BASE, f"ingest-{workload.name}-{tag}")
    if os.path.exists(datadir):
        shutil.rmtree(datadir)
    srv = MysqldServer(datadir, build_kind,
                       extra_args=server_profile.build_args(
                           engine, profile, write_path=True))
    try:
        with srv:
            conn = srv.connect()
            cur = conn.cursor()
            cur.execute("CREATE DATABASE ingest")
            cur.execute("USE ingest")
            cur.execute(workload.create_table_sql(TABLE, engine, layout))
            cur.close()
            conn.close()

            timeline_dir = os.path.join(out_dir, "timeline")
            os.makedirs(timeline_dir, exist_ok=True)
            share, rem = divmod(n_rows, n_writers)
            procs = []
            t0 = time.time()
            pos = 0
            for i in range(n_writers):
                cnt = share + (1 if i < rem else 0)
                rep = os.path.join(timeline_dir, f"{tag}-w{i}.csv")
                p = multiprocessing.Process(
                    target=writer_proc,
                    args=(srv.socket, "ingest", data_path, pos, cnt, rep))
                p.start()
                procs.append(p)
                pos += cnt
            for p in procs:
                p.join()
                if p.exitcode != 0:
                    raise RuntimeError(f"writer exited {p.exitcode} ({tag})")
            seconds = time.time() - t0

            conn = srv.connect(database="ingest")
            cur = conn.cursor()
            cur.execute(f"SELECT COUNT(*) FROM {TABLE}")
            loaded = cur.fetchone()[0]
            if loaded != n_rows:
                raise RuntimeError(
                    f"{tag}: {loaded} rows landed, expected {n_rows}")
            cur.close()
            conn.close()
            binfo = srv.build_info()
            args_hash = srv.args_hash()
    finally:
        if os.path.exists(datadir):
            shutil.rmtree(datadir)

    return {
        "engine": engine, "index_layout": layout, "writers": n_writers,
        "rows": n_rows, "seconds": round(seconds, 1),
        "rows_per_sec": round(n_rows / seconds, 1),
        "server_args_hash": args_hash,
        "mysql_commit": binfo["mysql_commit"][:12],
        "bitlsm_commit": binfo["bitlsm_commit"][:12],
    }


def run(config_path, dry_run, start_from):
    with open(config_path) as f:
        config = json.load(f)
    if config.get("build_kind") != "release":
        raise SystemExit("ingest sweep requires build_kind=release — "
                         f"got {config.get('build_kind')!r}")

    exp_set = os.path.splitext(os.path.basename(config_path))[0]
    exp_name = os.path.basename(os.path.dirname(os.path.abspath(__file__)))
    exp_label = f"{exp_name}_{exp_set}"
    workload = SsbFlatWorkload(sf=config.get("sf", 1))
    workload.verify_data()
    profile = server_profile.resolve(config.get("server_common"))

    shuffle = config.get("shuffle", False)
    seed = config.get("shuffle_seed", 42)
    data_path = (shuffled_copy(workload, seed) if shuffle
                 else workload.flat_path)
    with open(workload.flat_path + ".meta.json") as f:
        total_rows = json.load(f)["rows"]
    n_rows = config["rows"]
    if n_rows == "all":
        n_rows = total_rows
    if n_rows > total_rows:
        raise SystemExit(f"rows={n_rows} exceeds flat file ({total_rows})")

    runs = [(ident["engine"], ident["index_layout"], n)
            for ident in config["identities"]
            for n in config["writers"]]

    out_dir = make_result_dir(exp_label)
    log_file, log_path = setup_logging(exp_label)
    try:
        print(f"config : {config_path}")
        print(f"label  : {exp_label}")
        print(f"runs   : {len(runs)} ({n_rows} rows each, "
              f"shuffle={shuffle})")
        print(f"server : {profile}")
        print(f"output : {out_dir}")
        if dry_run:
            for engine, layout, n in runs:
                print(f"  {engine}/{layout} writers={n} args="
                      f"{server_profile.build_args(engine, profile, True)}")
            return 0

        os.makedirs(out_dir, exist_ok=True)
        csv_path = os.path.join(out_dir, "ingest.csv")
        with open(csv_path, "w", newline="") as cf:
            writer = csv.DictWriter(cf, fieldnames=CSV_FIELDS)
            writer.writeheader()
            for idx, (engine, layout, n) in enumerate(runs, 1):
                if idx < start_from:
                    continue
                tag = f"{engine}-{layout}-N{n}"
                print(f"[{idx}/{len(runs)}] {tag} ...", flush=True)
                row = run_one(workload, engine, layout, n, n_rows,
                              data_path, "release", out_dir, tag, profile)
                row.update({"ts": time.strftime("%H:%M:%S"),
                            "workload": workload.name,
                            "shuffle": int(shuffle)})
                writer.writerow(row)
                cf.flush()
                print(f"[{idx}/{len(runs)}] {tag}: "
                      f"{row['rows_per_sec']} rows/s "
                      f"({row['seconds']}s)", flush=True)
        print(f"done -> {csv_path}")
        return 0
    finally:
        teardown_logging(log_file, log_path)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="MyRocks integration test ingest sweep")
    add_common_args(parser)
    args = parser.parse_args()
    maybe_run_as_daemon(args)
    sys.exit(run(args.config, dry_run=args.dry_run,
                 start_from=args.start_from))


if __name__ == "__main__":
    main()
