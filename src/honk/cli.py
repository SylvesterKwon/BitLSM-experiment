"""CLI entry point for Honk workload generator."""

import argparse
import logging
import os
import sys
import time

import numpy as np

import glob as globmod

from .config import ExpandedQueryBlock, HonkConfigError, WriteOnlyPhase, load_config, resolve_rows, validate_row_budget
from .dataset import DatasetCursor
from .filters import GuidedTwoPointFilterGenerator, TwoPointFilterGenerator, UniformFilterGenerator
from .phases import PKReservoir, execute_phases, _run_write_only, run_write_with_periodic_queries
from .pk import PKGenerator
from .schema import ALL_COLUMNS
from .writer import TSVWriter

logger = logging.getLogger("honk")


def _setup_logging(log_path: str) -> None:
    """Configure logging to stderr and a file."""
    formatter = logging.Formatter(
        "%(asctime)s %(levelname)-8s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    file_handler = logging.FileHandler(log_path, mode="w")
    file_handler.setFormatter(formatter)

    stderr_handler = logging.StreamHandler(sys.stderr)
    stderr_handler.setFormatter(formatter)

    root = logging.getLogger("honk")
    root.setLevel(logging.INFO)
    root.addHandler(file_handler)
    root.addHandler(stderr_handler)


def cmd_run(args: argparse.Namespace) -> None:
    """Execute the 'run' subcommand."""
    config_path = args.config
    output_dir = args.output_dir
    data_dir = args.data_dir

    # Load config
    try:
        config = load_config(config_path)
    except HonkConfigError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    # Derive output basename from config filename
    config_stem = os.path.splitext(os.path.basename(config_path))[0]

    # Create output directory
    os.makedirs(output_dir, exist_ok=True)

    # Setup logging
    log_path = os.path.join(output_dir, f"{config_stem}.log")
    _setup_logging(log_path)

    # Resolve dataset file paths
    file_paths = []
    for fname in config.dataset:
        path = os.path.join(data_dir, fname)
        if not os.path.exists(path):
            logger.error("Dataset file not found: %s", path)
            sys.exit(1)
        file_paths.append(path)

    # Build filter generators
    columns = ALL_COLUMNS

    # Load dataset
    logger.info("Loading %d dataset file(s)... (pk_mode=%s, shuffle=%s)",
                len(file_paths), config.pk_mode, config.shuffle)
    t0 = time.time()
    cursor = DatasetCursor(file_paths, columns=columns,
                           shuffle=config.shuffle, seed=config.seed)
    logger.info("Loaded %d rows in %.1fs", cursor.total_rows, time.time() - t0)

    # Resolve unspecified rows to full dataset size
    resolve_rows(config, cursor.total_rows)

    # Validate row budget
    try:
        validate_row_budget(config, cursor.total_rows)
    except HonkConfigError as e:
        logger.error(str(e))
        sys.exit(1)

    rng = np.random.default_rng(config.seed)

    logger.info("Learning data distributions...")
    df = cursor.dataframe
    uniform_gen = UniformFilterGenerator(df, columns, rng)
    two_point_gen = TwoPointFilterGenerator(df, columns, rng)
    guided_gen = GuidedTwoPointFilterGenerator(df, columns, rng)

    # Execute phases
    tsv_path = os.path.join(output_dir, f"{config_stem}.tsv")
    logger.info("Generating workload → %s", tsv_path)
    t0 = time.time()

    with TSVWriter(tsv_path) as writer:
        execute_phases(config, cursor, uniform_gen, two_point_gen, guided_gen, writer, rng, df)

    elapsed = time.time() - t0
    logger.info("Done in %.1fs", elapsed)


def cmd_ingestion(args: argparse.Namespace) -> None:
    """Execute the 'ingestion' subcommand (6.2.2 Query Performance Under Active Ingestion)."""
    output_dir = args.output_dir
    data_dir = args.data_dir
    seed = args.seed
    preload_rows = args.preload_rows
    interleave_rows = args.interleave_rows
    query_interval = args.query_interval
    query_k = args.query_k
    selectivity_hi = args.target_selectivity * 100.0  # ratio → percent
    selectivity_lo = selectivity_hi / 10.0

    os.makedirs(output_dir, exist_ok=True)

    log_path = os.path.join(output_dir, "ingestion.log")
    _setup_logging(log_path)

    # Discover dataset files
    file_paths = sorted(globmod.glob(os.path.join(data_dir, "*.parquet")))
    if not file_paths:
        logger.error("No .parquet files found in %s", data_dir)
        sys.exit(1)
    logger.info("Found %d parquet file(s) in %s", len(file_paths), data_dir)

    # Load dataset
    columns = ALL_COLUMNS
    pk_gen = PKGenerator(args.pk_mode)
    logger.info("Loading dataset... (pk_mode=%s, shuffle=%s)", args.pk_mode, args.shuffle)
    t0 = time.time()
    cursor = DatasetCursor(file_paths, columns=columns,
                           shuffle=args.shuffle, seed=seed)
    logger.info("Loaded %d rows in %.1fs", cursor.total_rows, time.time() - t0)

    total_needed = preload_rows + interleave_rows
    if total_needed > cursor.total_rows:
        logger.error(
            "Need %d rows (preload %d + interleave %d) but dataset has only %d",
            total_needed, preload_rows, interleave_rows, cursor.total_rows,
        )
        sys.exit(1)

    rng = np.random.default_rng(seed)

    logger.info("Learning data distributions...")
    df = cursor.dataframe
    uniform_gen = UniformFilterGenerator(df, columns, rng)
    two_point_gen = TwoPointFilterGenerator(df, columns, rng)
    guided_gen = GuidedTwoPointFilterGenerator(df, columns, rng)

    query_block = ExpandedQueryBlock(
        label=f"ingestion_k{query_k}",
        strategy="guided_two_point",
        weight=1.0,
        query_attr_num=query_k,
        query_attrs=["payment_type", "PULocationID", "fare_amount", "tpep_pickup_datetime"],
        target_selectivity_percent={"lo": selectivity_lo, "hi": selectivity_hi},
    )

    tsv_path = os.path.join(output_dir, "ingestion.tsv")
    logger.info("Generating workload → %s", tsv_path)
    t0 = time.time()

    pk_buffer = PKReservoir()

    with TSVWriter(tsv_path) as writer:
        # Phase 1: Pre-load (write only)
        preload_phase = WriteOnlyPhase(label="preload", rows=preload_rows)
        _run_write_only(preload_phase, cursor, writer, rng, pk_buffer, pk_gen)

        # Phase 2: Interleave (writes + periodic queries)
        run_write_with_periodic_queries(
            rows=interleave_rows,
            query_interval=query_interval,
            query_block=query_block,
            cursor=cursor,
            uniform_gen=uniform_gen,
            two_point_gen=two_point_gen,
            guided_gen=guided_gen,
            writer=writer,
            rng=rng,
            pk_buffer=pk_buffer,
            df=df,
            rows_already_written=preload_rows,
            pk_gen=pk_gen,
        )

    elapsed = time.time() - t0
    logger.info(
        "Done in %.1fs: %d writes, %d reads",
        elapsed, writer.writes, writer.reads,
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="honk",
        description="Honk — KV storage benchmark workload generator",
    )
    subparsers = parser.add_subparsers(dest="command")

    run_parser = subparsers.add_parser("run", help="Generate a workload from a JSON config")
    run_parser.add_argument("config", help="Path to workload JSON config file")
    run_parser.add_argument("--output_dir", required=True, help="Output directory")
    run_parser.add_argument("--data_dir", default="./data", help="Base directory for dataset files (default: ./data)")

    ing_parser = subparsers.add_parser("ingestion", help="Generate 6.2.2 query-under-ingestion workload")
    ing_parser.add_argument("--output_dir", required=True, help="Output directory")
    ing_parser.add_argument("--data_dir", default="./data", help="Base directory for parquet files")
    ing_parser.add_argument("--seed", type=int, default=42, help="Random seed (default: 42)")
    ing_parser.add_argument("--preload-rows", type=int, default=20_000_000, help="Rows for pre-load phase (default: 20M)")
    ing_parser.add_argument("--interleave-rows", type=int, default=10_000_000, help="Rows for interleave phase (default: 10M)")
    ing_parser.add_argument("--query-interval", type=int, default=10_000, help="Insert 1 query every N writes (default: 10000)")
    ing_parser.add_argument("--query-k", type=int, default=2, help="Number of query attributes (default: 2)")
    ing_parser.add_argument("--target-selectivity", type=float, default=0.0001, help="Selectivity upper bound as ratio (default: 0.0001)")
    ing_parser.add_argument("--pk_mode", choices=["uuid", "ulid"], default="uuid", help="Primary-key scheme (default: uuid)")
    ing_parser.add_argument("--shuffle", action="store_true", help="Seeded shuffle of dataset before consuming (random write order)")

    args = parser.parse_args()
    if args.command is None:
        parser.print_help()
        sys.exit(1)

    if args.command == "run":
        cmd_run(args)
    elif args.command == "ingestion":
        cmd_ingestion(args)
