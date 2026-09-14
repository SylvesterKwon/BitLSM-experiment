"""Interval-window filter strategy: ``start_attr >= x AND end_attr < y``.

A query of this shape asks for the intervals that begin and end inside one
window -- on the taxi data, the trips picked up after x and dropped off
before y. The two attributes are fixed by the query block, so unlike the
other strategies nothing about the attribute choice is random; only the
window moves.

The window is cut from the sorted values of ``start_attr`` the way
GuidedTwoPointFilterGenerator sizes a single range: a span of rows drawn from
the target selectivity band, anchored at a random position. The rows whose
interval also finishes before y are a subset of those, so the joint
selectivity lands at or a little below the span; phases._generate_read checks
the band on the joint filter and redraws when the spill pushes it under.

Both bounds are emitted as ordinary two-sided "range" filters with the open
side carrying a sentinel far outside the data domain. The player, the
candidate-count tool and compute_selectivity_percent therefore need no
one-sided range support, and BitLSM folds the two conditions on one attribute
into a single interval, so the sentinel bound costs nothing at query time.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

# Open-side sentinels, in epoch seconds: 1970-01-01 and 2100-01-01. Both lie
# outside every value the dataset holds after NA filling.
FAR_PAST = 0
FAR_FUTURE = 4_102_444_800


def _epoch_seconds(series: pd.Series) -> np.ndarray:
    """Valid values of a datetime or numeric column as int64 epoch seconds."""
    valid = series[~pd.isna(series)]
    if pd.api.types.is_datetime64_any_dtype(valid):
        return valid.to_numpy().astype("datetime64[s]").astype(np.int64)
    return valid.to_numpy().astype(np.int64)


class IntervalWindowFilterGenerator:
    def __init__(self, df: pd.DataFrame, start_attr: str, end_attr: str):
        self.start_attr = start_attr
        self.end_attr = end_attr
        self._sorted = np.sort(_epoch_seconds(df[start_attr]))

    def generate(self, target_selectivity_percent: dict,
                 rng: np.random.Generator) -> list[dict]:
        n = len(self._sorted)
        lo = target_selectivity_percent["lo"] / 100.0
        hi = target_selectivity_percent["hi"] / 100.0
        lo_span = max(1, int(lo * n))
        hi_span = min(n, max(lo_span + 1, int(hi * n) + 1))
        span = int(rng.integers(lo_span, hi_span))
        anchor = int(rng.integers(0, max(1, n - span)))
        x = int(self._sorted[anchor])
        y = int(self._sorted[min(anchor + span, n - 1)])
        if y <= x:
            y = x + 1
        return [
            {"attr": self.start_attr, "op": "range", "lo": x, "hi": FAR_FUTURE},
            {"attr": self.end_attr, "op": "range", "lo": FAR_PAST, "hi": y},
        ]


# One generator per (dataframe, attribute pair), built on first use: the
# other strategies are constructed up front in cli.py and threaded through
# every phase function, and this one keeps that plumbing untouched by
# building itself from the df that _generate_read already receives. The
# dataframe lives for the whole run, so id(df) identifies it safely.
_generators: dict[tuple[int, str, str], IntervalWindowFilterGenerator] = {}


def interval_window_generator(df: pd.DataFrame,
                              window: dict) -> IntervalWindowFilterGenerator:
    key = (id(df), window["start_attr"], window["end_attr"])
    gen = _generators.get(key)
    if gen is None:
        gen = IntervalWindowFilterGenerator(df, window["start_attr"],
                                            window["end_attr"])
        _generators[key] = gen
    return gen
