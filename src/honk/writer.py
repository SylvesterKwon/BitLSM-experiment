"""TSV workload output writer."""

import json
import uuid
from typing import IO, Any

import numpy as np
import pandas as pd


def _json_default(obj: Any) -> Any:
    """JSON serialization helper for pandas/numpy types."""
    if isinstance(obj, pd.Timestamp):
        return int(obj.timestamp())
    if isinstance(obj, (np.integer,)):
        return int(obj)
    if isinstance(obj, (np.floating,)):
        return float(obj)
    if pd.isna(obj):
        return None
    raise TypeError(f"Not serializable: {type(obj)}")


class TSVWriter:
    """Writes workload operations to a TSV file."""

    def __init__(self, path: str):
        self._f: IO[str] = open(path, "w", buffering=8 * 1024 * 1024)
        self.writes = 0
        self.reads = 0
        self.updates = 0
        self.pauses = 0

    def write_row(self, pk: uuid.UUID, record: dict) -> None:
        record_json = json.dumps(record, default=_json_default, ensure_ascii=False)
        self._f.write(f"w\t{pk}\t{record_json}\n")
        self.writes += 1

    def write_update(self, pk: uuid.UUID, record: dict) -> None:
        record_json = json.dumps(record, default=_json_default, ensure_ascii=False)
        self._f.write(f"u\t{pk}\t{record_json}\n")
        self.updates += 1

    def write_read(self, filters: list[dict], most_selective_attr: str | None = None) -> None:
        obj: dict = {"filters": filters}
        if most_selective_attr is not None:
            obj["most_selective_attr"] = most_selective_attr
        filter_json = json.dumps(obj, ensure_ascii=False)
        self._f.write(f"r\t{filter_json}\n")
        self.reads += 1

    def write_block(self, data: str, writes: int = 0, reads: int = 0, updates: int = 0) -> None:
        """Write a pre-formatted block and update counters."""
        self._f.write(data)
        self.writes += writes
        self.reads += reads
        self.updates += updates

    def write_pause(self, seconds: float) -> None:
        self._f.write(f"p\t{seconds}\n")
        self.pauses += 1

    def close(self) -> None:
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
