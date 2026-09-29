#!/usr/bin/env python3
"""Queries-under-concurrent-updates sweep: qui_player over update rates W, W-major.

Every run works on its own checkpoint copy of the base DB, because the
run overwrites records. The copy is what rocksdb::Checkpoint does for a
closed DB, done at the file level so the base is never opened: the immutable
*.sst files are hard-linked, the small metadata RocksDB rewrites (CURRENT,
IDENTITY, MANIFEST-*, OPTIONS-*, the *.log WAL) is copied, and the per-open
files LOG / LOG.old.* / LOCK are left behind. The base's (name, size)
inventory is taken before and after every run and the sweep aborts if it
changed. The copy is deleted after a successful run unless --keep-run-db, and
kept (with its path printed) when the run fails or the base inventory
changed; --clean-db is accepted for uniformity and ignored.

Order is W-major (every arm at one W before the next W) so that drift over a
long sweep lands evenly across arms.

Usage:
    python3 experiments/queries_under_concurrent_updates/run.py exp_set/<params>.json [options]
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
from datetime import datetime

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "..", "src"))
sys.path.insert(0, EXP_DIR)

import summarize
from run_common import (
    add_common_args,
    cartesian_combinations,
    cooldown_sleep,
    fmt,
    git_commit,
    make_result_dir,
    maybe_run_as_daemon,
    parse_method_filter,
    PROJECT_ROOT,
    reset_hardware,
    run_process,
    setup_logging,
    teardown_logging,
    warn_if_cpu_unpinned,
)

BINARY = os.path.join(PROJECT_ROOT, "build", "bin", "qui_player")

# Same rule as nyc_taxi_seq_write / nyc_taxi_seq_read: only these flags name
# a distinct base DB; read-side flags (intersection_limit) share one.
DB_PARAMS = ["rho", "bloom_bits"]


def encode_method_params(params: dict) -> str:
    db_params = {k: v for k, v in params.items() if k in DB_PARAMS}
    if not db_params:
        return "default"
    return "_".join(f"{k}{fmt(v)}" for k, v in db_params.items())


def run_db_name(method: str, combo: dict, rate) -> str:
    """One run's DB copy. Every flag, not just DB_PARAMS: two arms that share
    a base DB (embedded-postings il2 / il0) still need separate copies."""
    params = "_".join(f"{k}{fmt(v)}" for k, v in sorted(combo.items())) or "default"
    return f"{method}_{params}_w{fmt(rate)}"


def abs_path(p: str) -> str:
    return p if os.path.isabs(p) else os.path.join(PROJECT_ROOT, p)


# ---------------------------------------------------------------------------
# Checkpoint copy of a closed, drained base DB
# ---------------------------------------------------------------------------

def classify(name: str):
    """'link' for immutable SSTs, 'copy' for the metadata RocksDB rewrites
    and for the lazy-bitmaps bin-policy sidecar the binding reads at open,
    'skip' for per-open files, None for anything this runner does not know
    (an unknown file means an assumption about the base DB is wrong)."""
    if name.endswith(".sst"):
        return "link"
    if name == "lazy_bitmaps.policy":
        return "copy"
    if (name in ("CURRENT", "IDENTITY") or name.startswith(("MANIFEST-", "OPTIONS-"))
            or name.endswith(".log")):
        return "copy"
    if name in ("LOCK", "LOG") or name.startswith("LOG.old."):
        return "skip"
    return None


def inventory(db_dir: str) -> list:
    """Sorted [name, size] of every file in db_dir, skipped ones included: a
    change to LOG would mean something opened the base."""
    return sorted([n, os.path.getsize(os.path.join(db_dir, n))]
                  for n in os.listdir(db_dir))


def inventory_sha256(inv: list) -> str:
    return hashlib.sha256(json.dumps(inv).encode()).hexdigest()


def checkpoint_db(base: str, run_db: str) -> dict:
    """Create run_db as a file-level checkpoint of base. Returns the counts."""
    if os.path.exists(run_db):
        raise RuntimeError(f"run DB already exists: {run_db}")
    names = sorted(os.listdir(base))
    unknown = [n for n in names if classify(n) is None]
    if unknown:
        raise RuntimeError(f"unknown files in base DB {base}: {unknown}")
    os.makedirs(run_db)
    counts = {"linked": 0, "copied": 0, "skipped": 0}
    try:
        for n in names:
            kind = classify(n)
            src, dst = os.path.join(base, n), os.path.join(run_db, n)
            if kind == "link":
                try:
                    os.link(src, dst)
                except OSError as e:
                    raise RuntimeError(
                        f"hard link failed ({e}); run_db_base must be on the same "
                        f"filesystem as the base DB") from e
                counts["linked"] += 1
            elif kind == "copy":
                shutil.copy2(src, dst)
                counts["copied"] += 1
            else:
                counts["skipped"] += 1
    except Exception:
        shutil.rmtree(run_db, ignore_errors=True)
        raise
    return counts


# ---------------------------------------------------------------------------
# Provenance
# ---------------------------------------------------------------------------

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(64 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tsv_sha256(path: str) -> tuple:
    """(sha256, source). The generator writes the hash it computed while
    streaming into a sidecar <stem>.log; for the 9 GB update file that saves
    a re-read. Anything else is hashed here."""
    side = os.path.splitext(path)[0] + ".log"
    if os.path.exists(side):
        with open(side) as f:
            for tok in f.read().split():
                if tok.startswith("sha256="):
                    return tok[len("sha256="):], "sidecar"
    return sha256_file(path), "computed"


def write_sweep_meta(output_dir: str, cfg: dict, config_path: str,
                     bases: list) -> None:
    qsha, qsrc = tsv_sha256(cfg["query_workload"])
    usha, usrc = (tsv_sha256(cfg["update_workload"])
                  if cfg.get("update_workload") else ("", ""))
    base_dbs = {}
    for base in bases:
        inv = inventory(base)
        base_dbs[base] = {"files": len(inv), "bytes": sum(s for _, s in inv),
                          "inventory_sha256": inventory_sha256(inv),
                          "inventory": inv}
    st = os.stat(BINARY)
    meta = {
        "started": datetime.now().isoformat(timespec="seconds"),
        "argv": sys.argv,
        "exp_set_path": os.path.abspath(config_path),
        "exp_set": cfg,
        "query_workload_sha256": qsha,
        "query_workload_sha256_source": qsrc,
        "update_workload_sha256": usha,
        "update_workload_sha256_source": usrc,
        "base_dbs": base_dbs,
        "experiment_commit": git_commit(PROJECT_ROOT),
        "bitlsm_commit": git_commit(os.path.join(PROJECT_ROOT, "third_party", "BitLSM")),
        "binary": {"path": BINARY, "size": st.st_size,
                   "mtime": datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds")},
    }
    with open(os.path.join(output_dir, "sweep_meta.json"), "w") as f:
        json.dump(meta, f, indent=2)


# ---------------------------------------------------------------------------
# Driver invocation
# ---------------------------------------------------------------------------

def build_command(method: str, combo: dict, rate, run_db: str, cfg: dict,
                  output_dir: str) -> list:
    cmd = [BINARY,
           "--binding", method,
           "--db_path", run_db,
           "--query_workload", cfg["query_workload"],
           "--update_rate", fmt(rate),
           "--duration_s", fmt(cfg["duration_s"]),
           "--window_s", fmt(cfg["window_s"]),
           "--sample_interval_s", fmt(cfg.get("sample_interval_s", 1)),
           "--seed", str(cfg.get("seed", 42)),
           "--output_dir", output_dir]
    if rate > 0:
        cmd += ["--update_workload", cfg["update_workload"]]
    for k, v in cfg.get("common_params", {}).items():
        cmd += [f"--{k}", fmt(v)]
    for k, v in combo.items():
        cmd += [f"--{k}", fmt(v)]
    return cmd


def run(config_path: str, dry_run: bool, method_filter: list, cooldown: int,
        hw_reset: bool, keep_run_db: bool, clean_db_flag: bool,
        start_from: int = 1):
    with open(config_path) as f:
        cfg = json.load(f)
    cfg["query_workload"] = abs_path(cfg["query_workload"])
    if cfg.get("update_workload"):
        cfg["update_workload"] = abs_path(cfg["update_workload"])

    exp_set_name = os.path.splitext(os.path.basename(config_path))[0]
    exp_label = f"queries_under_concurrent_updates_{exp_set_name}"
    db_path_base = cfg["db_path_base"]
    pk_mode = cfg["pk_mode"]
    rates = cfg["update_rates"]
    methods = cfg["methods"]
    output_dir = make_result_dir(exp_label)
    run_db_root = os.path.join(cfg["run_db_base"], os.path.basename(output_dir))

    log_file, log_path = setup_logging(exp_label)
    try:
        warn_if_cpu_unpinned()
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")
        arms = [(m["name"], c) for m in methods
                for c in cartesian_combinations(m.get("params", {}))]
        total = len(rates) * len(arms)
        if any(r > 0 for r in rates) and not cfg.get("update_workload"):
            sys.exit("update_rates above 0 need update_workload")

        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"rates    : {', '.join(fmt(r) for r in rates)} updates/s")
        print(f"arms     : {', '.join(run_db_name(n, c, 0)[:-3] for n, c in arms)}")
        print(f"total    : {total} run(s), {fmt(cfg['duration_s'])} s each")
        print(f"run-db   : {run_db_root} ({'kept' if keep_run_db else 'deleted after each run'})")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        if clean_db_flag:
            print("clean-db : ignored (run DBs are per-run copies)")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        bases = []
        for name, combo in arms:
            base = f"{db_path_base}/{pk_mode}/{name}/{encode_method_params(combo)}"
            if base not in bases:
                bases.append(base)
        missing = [b for b in bases if not os.path.isdir(b)]
        if missing and not dry_run:
            sys.exit(f"base DB missing (build it first): {missing}")

        if not dry_run:
            os.makedirs(output_dir, exist_ok=True)
            write_sweep_meta(output_dir, cfg, config_path, bases)
            if hw_reset:
                print("[pre-run] initial hw-reset ...")
                reset_hardware(db_path_base)
                if cooldown > 0:
                    cooldown_sleep(cooldown)

        idx = 0
        for rate in rates:
            for name, combo in arms:
                idx += 1
                base = f"{db_path_base}/{pk_mode}/{name}/{encode_method_params(combo)}"
                run_db = os.path.join(run_db_root, run_db_name(name, combo, rate))
                cmd = build_command(name, combo, rate, run_db, cfg, output_dir)
                if idx < start_from:
                    print(f"[{idx}/{total}] [w{fmt(rate)}][{name}] SKIP (--start-from {start_from})")
                    continue
                print(f"[{idx}/{total}] [w{fmt(rate)}][{name}] {' '.join(cmd)}")
                if dry_run:
                    continue

                inv_before = inventory(base)
                t_copy = time.time()
                counts = checkpoint_db(base, run_db)
                print(f"  [checkpoint] {run_db}: {counts['linked']} sst linked, "
                      f"{counts['copied']} copied, {counts['skipped']} skipped "
                      f"({time.time() - t_copy:.1f}s)")
                t_run = time.time()
                rc, _ = run_process(cmd)
                elapsed = time.time() - t_run
                base_ok = inventory(base) == inv_before
                succeeded = rc == 0 and base_ok
                kept = keep_run_db or not succeeded
                if not kept:
                    shutil.rmtree(run_db)
                    print(f"  [run-db] removed {run_db}")
                elif not succeeded:
                    print(f"  [run-db] kept for diagnosis: {run_db}")
                else:
                    print(f"  [run-db] kept: {run_db}")
                record = {"idx": idx, "method": name, "params": combo, "rate": rate,
                          "base": base, "run_db": run_db, "kept": kept,
                          "checkpoint": counts, "rc": rc,
                          "elapsed_s": round(elapsed, 1),
                          "base_inventory_sha256": inventory_sha256(inv_before),
                          "base_unchanged": base_ok,
                          "finished": datetime.now().isoformat(timespec="seconds")}
                with open(os.path.join(output_dir, "sweep_runs.jsonl"), "a") as f:
                    f.write(json.dumps(record) + "\n")
                if not base_ok:
                    sys.exit(f"base DB changed during run {idx}: {base}")
                if rc != 0:
                    sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")
                print()

                if hw_reset:
                    reset_hardware(db_path_base)
                if cooldown > 0 and idx < total:
                    cooldown_sleep(cooldown)

        if os.path.isdir(run_db_root) and not os.listdir(run_db_root):
            os.rmdir(run_db_root)

        if not dry_run:
            runs, windows = summarize.collect(output_dir)
            if runs:
                for path in summarize.write_csvs(output_dir, runs, windows):
                    print(f"[summary] {path}")
    finally:
        teardown_logging(log_file, log_path)


def main():
    parser = argparse.ArgumentParser(
        description="Queries-under-concurrent-updates sweep: qui_player over update rates")
    add_common_args(parser)
    parser.add_argument("--keep-run-db", action="store_true",
                        help="Keep each run's DB copy instead of deleting it")
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    run(args.config, dry_run=args.dry_run,
        method_filter=parse_method_filter(args), cooldown=args.cooldown,
        hw_reset=args.hw_reset, keep_run_db=args.keep_run_db,
        clean_db_flag=args.clean_db, start_from=args.start_from)


if __name__ == "__main__":
    main()
