"""Phase execution orchestration."""

import json
import logging
import math
import uuid

import numpy as np
import pandas as pd

from .config import (
    ExpandedQueryBlock,
    MixedPhase,
    PausePhase,
    ReadOnlyPhase,
    WorkloadConfig,
    WriteOnlyPhase,
)
from .dataset import DatasetCursor
from .config import HonkConfigError
from .pk import PKGenerator, pickup_ms_from_value, PICKUP_COL
from .filters import GuidedTwoPointFilterGenerator, TwoPointFilterGenerator, UniformFilterGenerator, compute_most_selective_attr, compute_selectivity_percent
from .writer import TSVWriter

logger = logging.getLogger(__name__)


class PKReservoir:
    """Fixed-size reservoir maintaining uniform random sampling over all appended keys.

    Uses Algorithm R (Vitter, 1985) to keep at most *capacity* keys in memory
    while guaranteeing that every key ever appended has equal probability of
    being selected by :meth:`random_choice`.  This bounds memory usage to
    O(capacity) regardless of total write count.
    """

    _DEFAULT_CAPACITY = 100_000

    def __init__(self, capacity: int = _DEFAULT_CAPACITY) -> None:
        self._buf: list[uuid.UUID] = []
        self._capacity = capacity
        self._total = 0

    def append(self, pk: uuid.UUID, rng: np.random.Generator) -> None:
        self._total += 1
        if len(self._buf) < self._capacity:
            self._buf.append(pk)
        else:
            j = int(rng.integers(self._total))
            if j < self._capacity:
                self._buf[j] = pk

    def random_choice(self, rng: np.random.Generator) -> uuid.UUID:
        return self._buf[int(rng.integers(len(self._buf)))]

    def __len__(self) -> int:
        return self._total

    def __bool__(self) -> bool:
        return self._total > 0


def _row_to_dict(row: pd.Series) -> dict:
    """Convert a DataFrame row to a plain dict."""
    return row.to_dict()


def _make_uuid(rng: np.random.Generator) -> uuid.UUID:
    """Generate a random UUID v4 using the seeded RNG."""
    return uuid.UUID(bytes=rng.bytes(16), version=4)


def _build_query_sampler(
    queries: list[ExpandedQueryBlock],
) -> tuple[np.ndarray, list[ExpandedQueryBlock]]:
    """Build a weight-probability array for query block sampling."""
    weights = np.array([q.weight for q in queries], dtype=float)
    probs = weights / weights.sum()
    return probs, queries


_MAX_SELECTIVITY_RETRIES = 1000


def _generate_read(
    block: ExpandedQueryBlock,
    uniform_gen: UniformFilterGenerator,
    two_point_gen: TwoPointFilterGenerator,
    guided_gen: GuidedTwoPointFilterGenerator,
    rng: np.random.Generator,
    df: pd.DataFrame,
) -> tuple[list[dict], str | None]:
    """Generate a single read filter list and its most-selective attribute hint."""

    def _gen_filters() -> list[dict]:
        if block.strategy == "uniform":
            return uniform_gen.generate(block.query_attr_num, block.query_attrs)
        elif block.strategy == "two_point":
            return two_point_gen.generate(block.query_attr_num, block.query_attrs)
        elif block.strategy == "guided_two_point":
            return guided_gen.generate(
                block.query_attr_num, block.query_attrs,
                block.target_selectivity_percent,
            )
        else:
            raise ValueError(f"Unknown strategy: {block.strategy}")

    tsp = block.target_selectivity_percent
    if tsp is None:
        filters = _gen_filters()
        return filters, compute_most_selective_attr(filters, df)

    lo, hi = tsp["lo"], tsp["hi"]
    for _ in range(_MAX_SELECTIVITY_RETRIES):
        filters = _gen_filters()
        sel = compute_selectivity_percent(filters, df)
        if lo <= sel < hi:
            return filters, compute_most_selective_attr(filters, df)

    raise HonkConfigError(
        f"Query '{block.label}': failed to generate filters with selectivity in "
        f"[{lo}, {hi}]% after {_MAX_SELECTIVITY_RETRIES} retries"
    )


def _emit_updates(
    update_ratio: float,
    pk_buffer: PKReservoir,
    cursor: DatasetCursor,
    writer: TSVWriter,
    rng: np.random.Generator,
) -> None:
    """Generate update operations using probabilistic rounding."""
    if update_ratio <= 0 or not pk_buffer:
        return
    int_part = int(update_ratio)
    frac_part = update_ratio - int_part
    num_updates = int_part
    if frac_part > 0 and rng.random() < frac_part:
        num_updates += 1
    for _ in range(num_updates):
        target_pk = pk_buffer.random_choice(rng)
        new_row = cursor.consume(1).iloc[0]
        writer.write_update(target_pk, _row_to_dict(new_row))


def execute_phases(
    config: WorkloadConfig,
    cursor: DatasetCursor,
    uniform_gen: UniformFilterGenerator,
    two_point_gen: TwoPointFilterGenerator,
    guided_gen: GuidedTwoPointFilterGenerator,
    writer: TSVWriter,
    rng: np.random.Generator,
    df: pd.DataFrame,
) -> None:
    """Execute all phases in sequence."""
    pk_buffer = PKReservoir()
    pk_gen = PKGenerator(config.pk_mode)
    for phase in config.phases:
        if isinstance(phase, WriteOnlyPhase):
            _run_write_only(phase, cursor, writer, rng, pk_buffer, pk_gen)
        elif isinstance(phase, PausePhase):
            _run_pause(phase, writer)
        elif isinstance(phase, ReadOnlyPhase):
            _run_read_only(phase, uniform_gen, two_point_gen, guided_gen, writer, rng, df)
        elif isinstance(phase, MixedPhase):
            _run_mixed(phase, cursor, uniform_gen, two_point_gen, guided_gen, writer, rng, pk_buffer, df, pk_gen)

    logger.info(
        "Generation complete: %d writes, %d updates, %d reads, %d pauses",
        writer.writes, writer.updates, writer.reads, writer.pauses,
    )


_WRITE_CHUNK = 100_000


def _run_write_only(
    phase: WriteOnlyPhase,
    cursor: DatasetCursor,
    writer: TSVWriter,
    rng: np.random.Generator,
    pk_buffer: PKReservoir,
    pk_gen: PKGenerator,
) -> None:
    logger.info("Phase '%s': write_only, %d rows, update_ratio=%.2f", phase.label, phase.rows, phase.update_ratio)
    chunk = cursor.consume(phase.rows)

    if phase.update_ratio > 0:
        # Row-by-row path: interleaved updates require sequential processing
        records = chunk.to_dict("records")
        for record in records:
            pk = pk_gen.one(pickup_ms_from_value(record.get(PICKUP_COL)), rng)
            writer.write_row(pk, record)
            pk_buffer.append(pk, rng)
            _emit_updates(phase.update_ratio, pk_buffer, cursor, writer, rng)
        return

    # Fast path: batch processing (no interleaved updates)
    _write_batch(chunk, writer, rng, pk_buffer, pk_gen)


def _write_batch(
    chunk: pd.DataFrame,
    writer: TSVWriter,
    rng: np.random.Generator,
    pk_buffer: PKReservoir,
    pk_gen: PKGenerator,
) -> None:
    """Write a DataFrame chunk using the fast batch path."""
    n = len(chunk)
    for start in range(0, n, _WRITE_CHUNK):
        end = min(start + _WRITE_CHUNK, n)
        sub = chunk.iloc[start:end]
        batch_n = end - start

        # Pickup epoch-ms for ULID PKs (no effect in uuid mode; pure data, so
        # it does not perturb the RNG stream -> uuid output stays reproducible).
        pickup_ms = (
            sub[PICKUP_COL].astype("datetime64[ms]").astype("int64").to_numpy()
        )
        pks = pk_gen.many(pickup_ms, rng)

        dt_cols = sub.select_dtypes(include=["datetime64"]).columns
        if len(dt_cols) > 0:
            sub = sub.copy()
            for c in dt_cols:
                sub[c] = sub[c].astype("int64") // 1_000_000
        json_lines = sub.to_json(
            orient="records", lines=True, force_ascii=False,
        ).split("\n")
        if json_lines and json_lines[-1] == "":
            json_lines.pop()

        buf = "".join(f"w\t{pk}\t{jl}\n" for pk, jl in zip(pks, json_lines))
        writer.write_block(buf, writes=batch_n)

        for pk in pks:
            pk_buffer.append(pk, rng)


def run_write_with_periodic_queries(
    rows: int,
    query_interval: int,
    query_block: ExpandedQueryBlock,
    cursor: DatasetCursor,
    uniform_gen: UniformFilterGenerator,
    two_point_gen: TwoPointFilterGenerator,
    guided_gen: GuidedTwoPointFilterGenerator,
    writer: TSVWriter,
    rng: np.random.Generator,
    pk_buffer: PKReservoir,
    df: pd.DataFrame,
    rows_already_written: int,
    pk_gen: PKGenerator,
) -> None:
    """Write rows with a deterministic QUERY every *query_interval* PUTs.

    Selectivity of each query is validated against the cumulative data
    (rows_already_written + rows written so far in this call).
    """
    logger.info(
        "Interleave: %d rows, query every %d writes, %d queries expected",
        rows, query_interval, rows // query_interval,
    )

    written = 0
    while written < rows:
        batch_size = min(query_interval, rows - written)
        chunk = cursor.consume(batch_size)
        _write_batch(chunk, writer, rng, pk_buffer, pk_gen)
        written += batch_size

        # Insert query after every full interval
        if batch_size == query_interval:
            cumulative_n = rows_already_written + written
            cumulative_df = df.iloc[:cumulative_n]
            filters, most_selective = _generate_read(
                query_block, uniform_gen, two_point_gen, guided_gen, rng, cumulative_df,
            )
            writer.write_read(filters, most_selective)

    logger.info("Interleave done: %d writes, %d reads", written, writer.reads)


def _run_read_only(
    phase: ReadOnlyPhase,
    uniform_gen: UniformFilterGenerator,
    two_point_gen: TwoPointFilterGenerator,
    guided_gen: GuidedTwoPointFilterGenerator,
    writer: TSVWriter,
    rng: np.random.Generator,
    df: pd.DataFrame,
) -> None:
    logger.info("Phase '%s': read_only, %d reads, %d query blocks", phase.label, phase.num_queries, len(phase.queries))
    probs, blocks = _build_query_sampler(phase.queries)
    block_indices = np.arange(len(blocks))
    # Pre-sample all block selections at once (avoids per-iteration rng.choice overhead)
    sampled = rng.choice(block_indices, p=probs, size=phase.num_queries)
    for idx in sampled:
        block = blocks[idx]
        filters, most_selective = _generate_read(block, uniform_gen, two_point_gen, guided_gen, rng, df)
        writer.write_read(filters, most_selective)


def _run_pause(phase: PausePhase, writer: TSVWriter) -> None:
    logger.info("Phase '%s': pause, %s seconds", phase.label, phase.duration_seconds)
    writer.write_pause(phase.duration_seconds)


def _run_mixed(
    phase: MixedPhase,
    cursor: DatasetCursor,
    uniform_gen: UniformFilterGenerator,
    two_point_gen: TwoPointFilterGenerator,
    guided_gen: GuidedTwoPointFilterGenerator,
    writer: TSVWriter,
    rng: np.random.Generator,
    pk_buffer: PKReservoir,
    df: pd.DataFrame,
    pk_gen: PKGenerator,
) -> None:
    logger.info(
        "Phase '%s': mixed, %d rows, read_ratio=%.2f, update_ratio=%.2f, %d query blocks",
        phase.label, phase.rows, phase.read_ratio, phase.update_ratio, len(phase.queries),
    )

    probs, blocks = _build_query_sampler(phase.queries)
    block_indices = np.arange(len(blocks))

    int_part = int(phase.read_ratio)
    frac_part = phase.read_ratio - int_part

    chunk = cursor.consume(phase.rows)
    records = chunk.to_dict("records")
    for record in records:
        # Write
        pk = pk_gen.one(pickup_ms_from_value(record.get(PICKUP_COL)), rng)
        writer.write_row(pk, record)
        pk_buffer.append(pk, rng)

        # Updates
        _emit_updates(phase.update_ratio, pk_buffer, cursor, writer, rng)

        # Determine read count
        num_reads = int_part
        if frac_part > 0 and rng.random() < frac_part:
            num_reads += 1

        # Generate reads
        for _ in range(num_reads):
            idx = rng.choice(block_indices, p=probs)
            block = blocks[idx]
            filters, most_selective = _generate_read(block, uniform_gen, two_point_gen, guided_gen, rng, df)
            writer.write_read(filters, most_selective)
