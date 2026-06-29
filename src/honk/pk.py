"""Primary-key generation.

Two modes:
  - "uuid": random UUIDv4. The PK is uncorrelated with any attribute, so after
    LSM compaction (which sorts by PK) every SST block holds a uniform random
    sample of rows -> zone maps on continuous attributes cannot prune.
  - "ulid": ULID whose 48-bit timestamp is the row's tpep_pickup_datetime. The
    Crockford base32 string sorts lexicographically == chronologically, so the
    PK is correlated with pickup time. After compaction, SST blocks become
    time-contiguous -> zone maps prune pickup/dropoff range queries (the
    "time-correlated" regime). The 80-bit randomness is drawn from the seeded
    RNG (not os.urandom) so generation stays reproducible.
"""

from __future__ import annotations

import uuid

import numpy as np
import pandas as pd
import ulid

PICKUP_COL = "tpep_pickup_datetime"
_MAX_ULID_MS = (1 << 48) - 1


def pickup_ms_from_value(v) -> int:
    """Epoch milliseconds for a row's pickup value (Timestamp or epoch seconds).

    Invalid / missing / pre-1970 values clamp to 0 (they sort to the front;
    such rows are data-quality garbage anyway). The result is always a valid
    ULID timestamp in [0, 2**48).
    """
    try:
        if isinstance(v, pd.Timestamp):
            if pd.isna(v):
                return 0
            ms = int(v.value // 1_000_000)  # ns -> ms
        else:
            f = float(v)
            if f != f or f < 0:  # NaN or negative
                return 0
            ms = int(f * 1000)  # epoch seconds -> ms
    except (TypeError, ValueError):
        return 0
    if ms < 0:
        return 0
    return min(ms, _MAX_ULID_MS)


class PKGenerator:
    """Generates string primary keys for one of the two modes above."""

    def __init__(self, mode: str):
        if mode not in ("uuid", "ulid"):
            raise ValueError(
                f"Unknown pk_mode: {mode!r} (expected 'uuid' or 'ulid')"
            )
        self.mode = mode

    def one(self, pickup_ms: int, rng: np.random.Generator) -> str:
        """One PK. `pickup_ms` is ignored in uuid mode."""
        if self.mode == "uuid":
            return str(uuid.UUID(bytes=rng.bytes(16), version=4))
        ts = max(0, min(int(pickup_ms), _MAX_ULID_MS))
        return ulid.from_bytes(ts.to_bytes(6, "big") + rng.bytes(10)).str

    def many(self, pickup_ms: np.ndarray, rng: np.random.Generator) -> list[str]:
        """Batch of PKs, one per element of `pickup_ms` (ignored in uuid mode).

        uuid mode consumes the RNG identically to the legacy code path
        (`rng.bytes(16 * n)`) so existing uuid workloads reproduce byte-for-byte.
        """
        n = len(pickup_ms)
        if self.mode == "uuid":
            raw = rng.bytes(16 * n)
            return [
                str(uuid.UUID(bytes=raw[i * 16 : (i + 1) * 16], version=4))
                for i in range(n)
            ]
        raw = rng.bytes(10 * n)
        out = []
        for i in range(n):
            ts = int(pickup_ms[i])
            ts = 0 if ts < 0 else min(ts, _MAX_ULID_MS)
            out.append(ulid.from_bytes(ts.to_bytes(6, "big") + raw[i * 10 : (i + 1) * 10]).str)
        return out
