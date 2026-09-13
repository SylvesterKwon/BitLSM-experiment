#!/usr/bin/env python3
"""
Per-query SABI candidate counts for the BitLSM-format DBs of a read param set.

Runs build/bin/candidate_count (offline, untimed) for every workload x
bitlsm / bitlsm-global combination of the param set, against the same DB paths
run.py reads. Each CSV joins honk_player's read CSV on query_id.

Usage:
    python3 experiments/nyc_taxi_seq_read/count_candidates.py \
        experiments/nyc_taxi_seq_read/exp_set/<params>.json --pk_mode ulid [--verify]
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run import encode_method_params, workload_stem  # noqa: E402
from run_common import (  # noqa: E402
    PROJECT_ROOT,
    cartesian_combinations,
    fmt,
    make_result_dir,
    resolve_workload_paths,
    run_process,
)

BINARY = os.path.join(PROJECT_ROOT, "build", "bin", "candidate_count")
SABI_METHODS = ("bitlsm", "bitlsm-global")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("config")
    parser.add_argument("--pk_mode", choices=["uuid", "ulid"], required=True)
    parser.add_argument("--verify", action="store_true",
                        help="Also scan every row: true matches and misses")
    parser.add_argument("--query_limit", type=int, default=0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    with open(args.config) as f:
        config = json.load(f)
    exp_set = os.path.splitext(os.path.basename(args.config))[0]
    common = config.get("common_params", {})
    output_dir = None if args.dry_run else make_result_dir(
        f"nyc_taxi_seq_read_candidates_{exp_set}")

    for workload in resolve_workload_paths(config["workload"]):
        for method in config["methods"]:
            if method["name"] not in SABI_METHODS:
                continue
            for combo in cartesian_combinations(method.get("params", {})):
                db_path = (f"{config['db_path_base']}/{args.pk_mode}/"
                           f"{method['name']}/{encode_method_params(combo)}")
                name = (f"{workload_stem(workload)}_{method['name']}"
                        f"_rho{fmt(combo['rho'])}_candidates.csv")
                cmd = [BINARY, "--db_path", db_path, "--workload", workload,
                       "--indexed_attrs", common["indexed_attrs"],
                       "--output", os.path.join(output_dir or "<result>", name)]
                if args.query_limit:
                    cmd += ["--query_limit", str(args.query_limit)]
                if args.verify:
                    cmd.append("--verify")
                print(" ".join(cmd))
                if args.dry_run:
                    continue
                if not os.path.exists(db_path):
                    sys.exit(f"DB path does not exist: {db_path}")
                rc, _ = run_process(cmd)
                if rc != 0:
                    sys.exit(f"candidate_count failed (exit {rc})")


if __name__ == "__main__":
    main()
