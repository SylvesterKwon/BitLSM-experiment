"""Interval-window filter strategy: ``start_attr >= x AND end_attr < y``.

A query of this shape asks for the intervals that begin and end inside one
window -- on the taxi data, the trips picked up after x and dropped off
before y. The two attributes are fixed by the query block, so unlike the
other strategies nothing about the attribute choice is random; only the
window moves.

The window is built to hit a target selectivity exactly. A target is drawn
log-uniformly from the block's band (a tight band pins it to one level), x is
the start value of a random row, and y is the k-th smallest end value among
the rows starting at or after x, k being the target row count: the window
then holds exactly k rows (ties on y aside). phases._generate_read still
checks the band on the joint filter over the full dataset, so a tie or a row
with a garbage end value that lands the query outside the band is redrawn.

Both bounds are emitted as ordinary two-sided "range" filters with the open
side carrying a sentinel far outside the data domain. The player, the
candidate-count tool and compute_selectivity_percent therefore need no
one-sided range support, and BitLSM folds the two conditions on one attribute
into a single interval, so the sentinel bound costs nothing at query time.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd

# Open-side sentinels, in epoch seconds: 1970-01-01 and 2100-01-01. Both lie
# outside every value the dataset holds after NA filling.
FAR_PAST = 0
FAR_FUTURE = 4_102_444_800

# Rows examined past the anchor when placing y: k rows must start at or after
# x and end before y, and trips that run past y are skipped over, so the slice
# is a multiple of k. Doubled when even that is not enough.
SLICE_FACTOR = 4
SLICE_MIN = 1000


def _epoch_seconds(series: pd.Series) -> np.ndarray:
    """A datetime or numeric column as int64 epoch seconds (NA -> -1, which
    the dataset loader already substitutes for numeric NA)."""
    if pd.api.types.is_datetime64_any_dtype(series):
        out = series.to_numpy().astype("datetime64[s]").astype(np.int64)
        out[pd.isna(series).to_numpy()] = -1
        return out
    return series.to_numpy().astype(np.int64)


class IntervalWindowFilterGenerator:
    def __init__(self, df: pd.DataFrame, start_attr: str, end_attr: str):
        self.start_attr = start_attr
        self.end_attr = end_attr
        start = _epoch_seconds(df[start_attr])
        end = _epoch_seconds(df[end_attr])
        order = np.argsort(start, kind="stable")
        self._start = start[order]   # start values, ascending
        self._end = end[order]       # end value of the row at each position
        self._n = len(order)

    def generate(self, target_selectivity_percent: dict,
                 rng: np.random.Generator) -> list[dict]:
        lo = target_selectivity_percent["lo"] / 100.0
        hi = target_selectivity_percent["hi"] / 100.0
        target = math.exp(rng.uniform(math.log(lo), math.log(hi)))
        k = max(1, int(round(target * self._n)))
        if k >= self._n:
            raise ValueError("target selectivity selects the whole dataset")

        # Anchor: a random row, far enough from the end that k rows follow.
        anchor = int(rng.integers(0, self._n - k))
        x = int(self._start[anchor])
        # Rows starting before x but sharing its start value must count too:
        # the predicate is start >= x. Move the anchor to the run's first row.
        anchor = int(np.searchsorted(self._start, x, side="left"))

        m = max(SLICE_MIN, SLICE_FACTOR * k)
        while True:
            stop = min(self._n, anchor + m)
            ends = np.sort(self._end[anchor:stop])
            if k <= len(ends):
                y = int(ends[k - 1]) + 1     # end < y admits exactly k rows
                # Rows past the slice start no earlier than its last start; if
                # y lies at or below that, none of them can end before y, so
                # the count is exact. Otherwise widen the slice.
                if y <= int(self._start[stop - 1]) or stop == self._n:
                    break
            if stop == self._n:
                raise ValueError("not enough rows after the anchor")
            m *= 2
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
