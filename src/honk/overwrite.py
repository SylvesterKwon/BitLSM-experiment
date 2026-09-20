"""Uniform-overwrite update sequence for the query-under-ingestion experiment.

Streams an existing write trace (`w\\t<pk>\\t<json>` lines) once and emits N
update lines `u\\t<target pk>\\t<json of another row>`. The target key is drawn
uniformly from every key in the trace and the new values are one whole row
drawn uniformly from the same trace (its own key discarded); both draws are
with replacement. Rows are copied whole so within-row correlations survive --
attributes are never shuffled individually. The sequence is seeded, so every
method replays the identical updates.

`mode: "insert"` is the appendix variant: the same sampled rows written under
fresh UUIDs as `w` lines. The main experiment does not run it.

No parquet is loaded; the only input is the trace `nyc_taxi_seq_write` used.
"""

import hashlib
import json
import logging
import os
import time
import uuid
from typing import Iterator

import numpy as np

logger = logging.getLogger("honk")

_WRITE_PREFIX = b"w\t"


def count_write_lines(path: str) -> int:
    """Number of `w` lines in the trace; other ops (r/u/p) are not sampled."""
    n = 0
    with open(path, "rb") as f:
        for line in f:
            if line.startswith(_WRITE_PREFIX):
                n += 1
    return n


def _collect(path: str, target_idx: np.ndarray, payload_idx: np.ndarray):
    """One pass over the trace. Returns (pk_by_index, json_by_index) for the
    sorted, de-duplicated index arrays given. Indices count `w` lines only."""
    pks: dict[int, str] = {}
    jsons: dict[int, str] = {}
    ti = pi = 0
    nt, npay = len(target_idx), len(payload_idx)
    i = 0
    with open(path, "rb") as f:
        for line in f:
            if not line.startswith(_WRITE_PREFIX):
                continue
            want_t = ti < nt and target_idx[ti] == i
            want_p = pi < npay and payload_idx[pi] == i
            if want_t or want_p:
                _, pk, js = line.rstrip(b"\n").split(b"\t", 2)
                if want_t:
                    pks[i] = pk.decode()
                    ti += 1
                if want_p:
                    jsons[i] = js.decode()
                    pi += 1
                if ti == nt and pi == npay:
                    break
            i += 1
    if ti != nt or pi != npay:
        raise RuntimeError(
            f"trace shorter than sampled indices: got {i} write lines")
    return pks, jsons


def generate_lines(source_path: str, num_updates: int, seed: int,
                   mode: str = "overwrite") -> Iterator[str]:
    """Yield `num_updates` TSV lines (with trailing newline)."""
    if mode not in ("overwrite", "insert"):
        raise ValueError(f"unknown mode {mode!r}")
    n = count_write_lines(source_path)
    if n == 0:
        raise RuntimeError(f"no write lines in {source_path}")
    rng = np.random.default_rng(seed)
    targets = rng.integers(0, n, size=num_updates)
    payloads = rng.integers(0, n, size=num_updates)
    logger.info("source=%s write_lines=%d num_updates=%d seed=%d mode=%s",
                source_path, n, num_updates, seed, mode)
    pks, jsons = _collect(source_path,
                          np.unique(targets) if mode == "overwrite" else np.empty(0, dtype=np.int64),
                          np.unique(payloads))
    if mode == "overwrite":
        for t, p in zip(targets, payloads):
            yield f"u\t{pks[int(t)]}\t{jsons[int(p)]}\n"
    else:
        # uuid4 from the same generator so the key stream is seeded too.
        for p in payloads:
            pk = uuid.UUID(bytes=rng.bytes(16), version=4)
            yield f"w\t{pk}\t{jsons[int(p)]}\n"


def write_updates(config_path: str, output_dir: str) -> str:
    """Generate `<output_dir>/<config stem>.tsv` (+ `.log`) from a config JSON
    with keys source_tsv, num_updates, seed, mode. Returns the TSV path."""
    with open(config_path) as f:
        cfg = json.load(f)
    source = cfg["source_tsv"]
    num_updates = int(cfg["num_updates"])
    seed = int(cfg.get("seed", 42))
    mode = cfg.get("mode", "overwrite")

    os.makedirs(output_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(config_path))[0]
    out_path = os.path.join(output_dir, stem + ".tsv")
    log_path = os.path.join(output_dir, stem + ".log")

    t0 = time.time()
    digest = hashlib.sha256()
    written = 0
    with open(out_path, "w", buffering=8 * 1024 * 1024) as out:
        for line in generate_lines(source, num_updates, seed, mode):
            out.write(line)
            digest.update(line.encode())
            written += 1
    elapsed = time.time() - t0
    summary = (f"source_tsv={source} num_updates={written} seed={seed} "
               f"mode={mode} elapsed_s={elapsed:.1f} "
               f"sha256={digest.hexdigest()}")
    with open(log_path, "w") as f:
        f.write(summary + "\n")
    logger.info(summary)
    return out_path
