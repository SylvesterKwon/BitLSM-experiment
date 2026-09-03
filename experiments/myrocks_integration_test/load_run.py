#!/usr/bin/env python3
"""Build the datadirs an exp_set's cells need, and stop.

Loading is not part of any one measurement. read_plan and read_perf carry the
same cell axis and therefore the same fixtures, and a fixture outlives the
sweeps that read it: at SF10 the InnoDB composite datadir alone takes over a
day to build and is then read by every subsequent run. Folding that into a
measurement runner means a sweep can silently turn into a two-day job, and
means asking one runner to do the other's preparation.

So it gets its own runner. The measurement runners still call ensure_loaded
and still build what is missing -- this does not become a step you can forget
-- but with the fixtures already in place that call returns immediately and
they do nothing but measure.

Identity, not path: a datadir's name is derived from (workload, layout,
engine, engine_params), so a cell cannot be pointed at the wrong fixture and
two scale factors coexist under their own names.

Usage:
    python3 experiments/myrocks_integration_test/load_run.py exp_set/<params>.json
    python3 experiments/myrocks_integration_test/load_run.py exp_set/<params>.json --dry-run
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from run_common import (  # noqa: E402
    add_common_args, maybe_run_as_daemon, setup_logging, teardown_logging,
)
from myrocks.loader import datadir_for, ensure_loaded, is_loaded  # noqa: E402
from myrocks.workloads.registry import make_workload  # noqa: E402


def dir_bytes(path):
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


def fixtures(config):
    """The distinct datadirs an exp_set needs. Cells sharing an identity
    (same engine/layout/params, differing only in plans or session_vars)
    collapse to one fixture."""
    workload = make_workload(config)
    seen = {}
    for cell in config["cells"]:
        identity = workload.identity(cell["engine"], cell["index_layout"],
                                     cell.get("engine_params"))
        seen.setdefault(identity, (cell["engine"], cell["index_layout"],
                                   cell.get("engine_params")))
    return workload, seen


def run(config_path, dry_run, start_from):
    with open(config_path) as f:
        config = json.load(f)
    build_kind = config.get("build_kind", "debug")
    workload, wanted = fixtures(config)

    exp_set = os.path.splitext(os.path.basename(config_path))[0]
    exp_name = os.path.basename(os.path.dirname(os.path.abspath(__file__)))
    log_file, log_path = setup_logging(f"{exp_name}_{exp_set}_load")
    try:
        print(f"config : {config_path}")
        print(f"build  : {build_kind}")
        print(f"fixtures: {len(wanted)}")
        pending = [i for i in wanted if not is_loaded(i)]
        for identity in wanted:
            state = "cached" if is_loaded(identity) else "NEEDS LOAD"
            print(f"  [{state:>10}] {identity}")
            print(f"               {datadir_for(identity)}")
        if dry_run:
            print(f"dry-run: {len(pending)} of {len(wanted)} need building")
            return 0
        if not pending:
            print("nothing to build — every fixture is already loaded")
            return 0

        started = time.time()
        for idx, (identity, (engine, layout, eparams)) in enumerate(
                wanted.items(), 1):
            if idx < start_from:
                continue
            if is_loaded(identity):
                print(f"[{idx}/{len(wanted)}] {identity}: cached", flush=True)
                continue
            print(f"[{idx}/{len(wanted)}] {identity}: building ...",
                  flush=True)
            t0 = time.time()
            ensure_loaded(workload, engine, layout, build_kind, eparams)
            size = dir_bytes(datadir_for(identity))
            print(f"[{idx}/{len(wanted)}] {identity}: "
                  f"{time.time() - t0:.0f}s, {size / (1 << 30):.1f} GiB",
                  flush=True)
        print(f"done: {len(wanted)} fixtures ready "
              f"in {time.time() - started:.0f}s")
        return 0
    finally:
        teardown_logging(log_file, log_path)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="Build the datadirs an exp_set needs")
    add_common_args(parser)
    args = parser.parse_args()
    maybe_run_as_daemon(args)
    sys.exit(run(args.config, dry_run=args.dry_run,
                 start_from=args.start_from))


if __name__ == "__main__":
    main()
