#!/usr/bin/env python3
"""
Sequential write experiment.

Measures write time and DB size for each method × parameter combination.

Usage:
    python3 experiments/seq_write/run.py exp_set/<params>.json [options]

Options:
    --dry-run             Print commands without executing.
    --methods m1,m2       Run only the specified methods (comma-separated).
    --cooldown SECONDS    Wait between runs (default: 0).
    --hw-reset            Reset hardware state between runs (sudo required).
    --clean-db            Delete DB directories before each write run.
    --start-from N        Start from the N-th experiment (1-indexed).
    --cpu-list LIST       Run each binary under 'taskset -c LIST' (off by
                          default; machine-specific, see main()).
    --daemon / --no-daemon  Background mode (default: daemon).

Runs go schema-major with the method order rotated per schema, so a sweep's
hours of drift cannot line up with one method; see order_runs().
"""

import argparse
import csv
import json
import os
import subprocess
import sys

# Add src/ to path so we can import run_common
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (
    add_common_args,
    cartesian_combinations,
    clean_db,
    cooldown_sleep,
    fmt,
    make_result_dir,
    maybe_run_as_daemon,
    parse_method_filter,
    reset_hardware,
    run_process,
    setup_logging,
    teardown_logging,
)

EXP_DIR = os.path.dirname(os.path.abspath(__file__))

# Parameters that define DB identity.
DB_PARAMS = ["n", "schema", "rho"]

MASTER_COLUMNS = [
    "method", "attr_count", "time_elapsed_ms", "records_written",
    "db_size_bytes", "drain_ms",
]


def _path_safe(key: str, val) -> str:
    if key == "schema":
        return f"{key}_{os.path.splitext(os.path.basename(str(val)))[0]}"
    return f"{key}{fmt(val)}"


def encode_params(params: dict) -> str:
    return "_".join(_path_safe(k, v) for k, v in params.items())


def _schema_stem(schema_path: str) -> str:
    return os.path.splitext(os.path.basename(schema_path))[0]


def attr_count(schema_path: str) -> int:
    """Number of indexed attributes in a schema, read from the file itself.

    The sweep's x axis. Counted rather than parsed out of the filename so a
    schema that does not follow the default_aN_cC naming still lands right.
    """
    with open(schema_path) as f:
        return len(json.load(f)["attrs"])


def get_db_size_bytes(db_path: str) -> int:
    """Get total size of a DB directory in bytes using du."""
    result = subprocess.run(
        ["du", "-sb", db_path], capture_output=True, text=True)
    if result.returncode == 0:
        return int(result.stdout.split()[0])
    return -1


def parse_checkpoints(output: str) -> list[tuple[int, int]]:
    """Parse checkpoint lines from benchmark binary stdout.

    Looks for lines like: 'putted: 1000000 kvps, elapsed: 1234ms'
    and final line: 'created 100000000 kvps. (total:12345ms elapsed)'

    Returns list of (time_elapsed_ms, records_written).
    """
    checkpoints = []
    for line in output.splitlines():
        if "putted:" in line and "elapsed:" in line:
            parts = line.split()
            records = int(parts[parts.index("putted:") + 1])
            elapsed_str = parts[-1]  # e.g. "1234ms"
            elapsed = int(elapsed_str.rstrip("ms"))
            checkpoints.append((elapsed, records))
        elif "total:" in line and "ms elapsed" in line:
            start = line.index("total:") + len("total:")
            end = line.index("ms elapsed")
            total_ms = int(line[start:end])
            # Extract n from "created N kvps."
            parts = line.split()
            n = int(parts[parts.index("created") + 1])
            checkpoints.append((total_ms, n))
    return checkpoints


def parse_drain_ms(output: str):
    """Parse 'DRAIN_MS:<ms>', the post-ingest wait for flush and the compactions
    the ingest itself scheduled (no forced compaction).

    time_elapsed_ms covers the Put loop only; methods that build their index in
    flush/compaction pay much of that cost here, so it is recorded alongside.
    Returns None when the binary printed no such line.
    """
    for line in output.splitlines():
        if line.startswith("DRAIN_MS:"):
            return int(line[len("DRAIN_MS:"):])
    return None


def method_label(name: str, combo: dict) -> str:
    """Build method label, appending rho for bitlsm (e.g. 'bitlsm_rho0.01')."""
    if "rho" in combo:
        return f"{name}_rho{fmt(combo['rho'])}"
    return name


def build_command(binary: str, exp_label: str, db_path: str,
                  output_dir: str, combo: dict, cpu_list: str = "") -> list:
    cmd = ["taskset", "-c", cpu_list] if cpu_list else []
    cmd += [binary,
            "--exp_label", exp_label,
            "--exp_type", "write_seq",
            "--db_path", db_path,
            "--output_dir", output_dir]
    for key, val in combo.items():
        prefix = "-" if len(key) == 1 else "--"
        cmd += [f"{prefix}{key}", fmt(val)]
    return cmd


def order_runs(runs: list, schema_order: list) -> list:
    """Order runs schema-major, rotating the method sequence once per schema.

    Both parts answer the same problem: a sweep runs for hours, and whatever
    drifts over those hours (other tenants on the machine, free-space state)
    lands on whichever method happened to be measured then. Method-major order
    gave each method its own hour, so drift was indistinguishable from the
    method; schema-major puts one schema's methods within minutes of each other,
    where the ratio between them is what the figure plots. Rotating the order
    per schema keeps any one method off the first slot, whose run follows the
    longest idle stretch.

    A method's own combos (e.g. bitlsm over several rho) stay together and in
    order. Runs whose schema is not in schema_order keep their relative order at
    the end, so a param set that sweeps something else still runs.
    """
    if not schema_order:
        return runs
    groups = {s: [] for s in schema_order}
    extra = []
    for r in runs:
        schema = r[2].get("schema")
        (groups[schema] if schema in groups else extra).append(r)

    ordered = []
    for i, schema in enumerate(schema_order):
        group = groups[schema]
        names = list(dict.fromkeys(name for name, _, _ in group))
        if not names:
            continue
        shift = i % len(names)
        for name in names[shift:] + names[:shift]:
            ordered += [r for r in group if r[0] == name]
    return ordered + extra


def append_result(master_path: str, row: dict):
    write_header = (not os.path.exists(master_path)
                    or os.path.getsize(master_path) == 0)
    with open(master_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=MASTER_COLUMNS, restval="")
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def run(config_path: str, dry_run: bool, method_filter: list, cooldown: int,
        hw_reset: bool, clean_db_flag: bool, start_from: int = 1,
        cpu_list: str = ""):
    with open(config_path) as f:
        config = json.load(f)

    exp_set_name = os.path.splitext(os.path.basename(config_path))[0]
    exp_name     = os.path.basename(EXP_DIR)
    exp_label    = f"{exp_name}_{exp_set_name}"
    db_path_base = config["db_path_base"]
    common       = config.get("common_params", {})
    methods      = config["methods"]
    schema_dir   = config.get("schema_dir", "src/benchmark/schema")

    # This experiment always runs sequential writes — no exp_type in params.json.

    # Resolve schema filenames to full paths (relative to project root)
    project_root = os.path.join(EXP_DIR, "..", "..")
    if "schema" in common:
        schemas = common["schema"] if isinstance(common["schema"], list) else [common["schema"]]
        common["schema"] = [
            os.path.normpath(os.path.join(project_root, schema_dir, s))
            for s in schemas
        ]

    # Per-method progress CSV goes alongside the benchmark's own CSV
    output_dir = make_result_dir(exp_label)
    master_csv = os.path.join(output_dir, f"{exp_label}.csv")

    log_file, log_path = setup_logging(exp_label)

    try:
        if method_filter:
            methods = [m for m in methods if m["name"] in method_filter]
            if not methods:
                sys.exit(f"No methods matched: {method_filter}")

        runs = []
        for method in methods:
            all_params = {**common, **method.get("params", {})}
            for combo in cartesian_combinations(all_params):
                runs.append((method["name"], method["binary"], combo))
        runs = order_runs(runs, common.get("schema", []))
        total_runs = len(runs)

        print(f"config   : {config_path}")
        print(f"label    : {exp_label}")
        print(f"total    : {total_runs} run(s) across {len(methods)} method(s)")
        print(f"order    : schema-major, method order rotated per schema")
        print(f"cooldown : {cooldown}s between runs")
        print(f"hw-reset : {'on (sudo)' if hw_reset else 'off'}")
        print(f"clean-db : {'on' if clean_db_flag else 'off'}")
        print(f"cpu-list : {cpu_list or 'off (scheduler picks the cores)'}")
        print(f"master   : {master_csv}")
        if dry_run:
            print("mode     : dry-run\n")
        else:
            print()

        global_idx = 0

        if hw_reset and not dry_run:
            print("[pre-run] initial hw-reset ...")
            reset_hardware(db_path_base)
            if cooldown > 0:
                cooldown_sleep(cooldown)

        for name, binary, combo in runs:
            global_idx += 1
            if global_idx < start_from:
                print(f"[{global_idx}/{total_runs}] [{name}] SKIP (--start-from {start_from})")
                continue

            db_combo = {k: v for k, v in combo.items() if k in DB_PARAMS}
            db_path = f"{db_path_base}/{name}/{encode_params(db_combo)}"
            cmd = build_command(binary, exp_label, db_path, output_dir, combo,
                                cpu_list)

            print(f"[{global_idx}/{total_runs}] [{name}] {' '.join(cmd)}")

            if not dry_run:
                os.makedirs(db_path, exist_ok=True)
                os.makedirs(output_dir, exist_ok=True)

                rc, captured = run_process(cmd)
                if rc != 0:
                    sys.exit(f"Run failed (exit {rc}): {' '.join(cmd)}")

                checkpoints = parse_checkpoints(captured)
                drain_ms = parse_drain_ms(captured)
                db_size = get_db_size_bytes(db_path)
                label = method_label(name, combo)

                if checkpoints:
                    total_ms = checkpoints[-1][0]
                    print(f"  [result] total_time={total_ms}ms, drain={drain_ms}ms, db_size={db_size} bytes ({db_size / (1024**3):.2f} GiB)")

                if clean_db_flag:
                    clean_db(db_path)
                print()

                # Pin every row to its attribute count so the CSV survives a
                # resumed or method-filtered sweep; without it the a value
                # is only recoverable from row order, which those break.
                attrs = (attr_count(combo["schema"])
                         if "schema" in combo else "")

                for elapsed_ms, records in checkpoints[:-1]:
                    append_result(master_csv, {
                        "method": label,
                        "attr_count": attrs,
                        "time_elapsed_ms": elapsed_ms,
                        "records_written": records,
                        "db_size_bytes": "",
                    })
                if checkpoints:
                    append_result(master_csv, {
                        "method": label,
                        "attr_count": attrs,
                        "time_elapsed_ms": checkpoints[-1][0],
                        "records_written": checkpoints[-1][1],
                        "db_size_bytes": db_size,
                        "drain_ms": "" if drain_ms is None else drain_ms,
                    })

                if hw_reset:
                    reset_hardware(db_path_base)
                if cooldown > 0 and global_idx < total_runs:
                    cooldown_sleep(cooldown)

    finally:
        teardown_logging(log_file, log_path)


def main():
    parser = argparse.ArgumentParser(
        description="Sequential write performance: measure write time and DB size"
    )
    add_common_args(parser)
    parser.add_argument(
        "--cpu-list", default="",
        help="Run each binary under 'taskset -c LIST' (e.g. 0-15). Off by "
             "default: which cores exist, and whether they are all the same "
             "speed, is a property of the machine, so a sweep reproduces "
             "elsewhere without it. Worth setting on a hybrid CPU, where the "
             "single writer thread lands on a P- or an E-core depending on "
             "what else the machine is doing.")
    args = parser.parse_args()

    maybe_run_as_daemon(args)

    method_filter = parse_method_filter(args)
    run(args.config, dry_run=args.dry_run, method_filter=method_filter,
        cooldown=args.cooldown, hw_reset=args.hw_reset,
        clean_db_flag=args.clean_db, start_from=args.start_from,
        cpu_list=args.cpu_list)


if __name__ == "__main__":
    main()
