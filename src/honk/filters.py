"""Filter generation: uniform random, selectivity-targeted, and two-point strategies."""

import numpy as np
import pandas as pd

from .schema import Column, FilterType


def _to_python(val):
    """Convert numpy/pandas types to JSON-serializable Python types."""
    if isinstance(val, (np.integer,)):
        return int(val)
    if isinstance(val, (np.floating,)):
        return float(val)
    if isinstance(val, pd.Timestamp):
        return int(val.timestamp())
    if isinstance(val, np.datetime64):
        return int(pd.Timestamp(val).timestamp())
    return val


class UniformFilterGenerator:
    """Generate random filters by uniformly sampling column values."""

    def __init__(self, df: pd.DataFrame, columns: list[Column], rng: np.random.Generator):
        self.rng = rng
        self.columns = columns
        self.stats: dict[str, dict] = {}

        for col in columns:
            if col.name not in df.columns:
                continue
            series = df[col.name].dropna()
            if series.empty:
                continue

            if col.filter_type == FilterType.EQUALITY:
                self.stats[col.name] = {
                    "type": "equality",
                    "values": series.unique(),
                }
            else:
                if col.dtype == "datetime":
                    self.stats[col.name] = {
                        "type": "range",
                        "dtype": "datetime",
                        "min": series.min(),
                        "max": series.max(),
                    }
                else:
                    self.stats[col.name] = {
                        "type": "range",
                        "dtype": "numeric",
                        "min": float(series.min()),
                        "max": float(series.max()),
                    }

    def generate(self, query_attr_num: int, query_attrs: list[str] | None = None) -> list[dict]:
        """Generate a filter with query_attr_num random attributes."""
        available = [c for c in self.columns if c.name in self.stats]
        if query_attrs is not None:
            attr_set = set(query_attrs)
            available = [c for c in available if c.name in attr_set]
        k = min(query_attr_num, len(available))
        chosen = self.rng.choice(available, size=k, replace=False)

        filters = []
        for col in chosen:
            stat = self.stats[col.name]
            if stat["type"] == "equality":
                value = self.rng.choice(stat["values"])
                filters.append({"attr": col.name, "op": "eq", "value": _to_python(value)})
            elif stat["dtype"] == "datetime":
                min_ts = stat["min"].timestamp()
                max_ts = stat["max"].timestamp()
                a, b = sorted(self.rng.uniform(min_ts, max_ts, size=2))
                filters.append({
                    "attr": col.name, "op": "range",
                    "lo": int(a),
                    "hi": int(b),
                })
            else:
                a, b = sorted(self.rng.uniform(stat["min"], stat["max"], size=2))
                filters.append({
                    "attr": col.name, "op": "range",
                    "lo": round(a, 2), "hi": round(b, 2),
                })

        return filters


class TwoPointFilterGenerator:
    """Generate filters by sampling two actual data points and using their values as range bounds."""

    def __init__(self, df: pd.DataFrame, columns: list[Column], rng: np.random.Generator):
        self.rng = rng
        self.columns = columns
        self.n_rows = len(df)
        self.available_columns = [c for c in columns if c.name in df.columns]

        # Pre-extract column values as numpy arrays for O(1) random access
        # (avoids df.iloc which creates a full Series per call)
        self._values: dict[str, np.ndarray] = {}
        self._isna: dict[str, np.ndarray] = {}
        for c in self.available_columns:
            arr = df[c.name].to_numpy()
            self._values[c.name] = arr
            self._isna[c.name] = pd.isna(arr)

    def generate(self, query_attr_num: int, query_attrs: list[str] | None = None) -> list[dict]:
        """Pick k random attributes, sample 2 records, build filters from their values."""
        pool = self.available_columns
        if query_attrs is not None:
            attr_set = set(query_attrs)
            pool = [c for c in pool if c.name in attr_set]
        k = min(query_attr_num, len(pool))
        chosen_cols = list(self.rng.choice(pool, size=k, replace=False))

        indices = self.rng.choice(self.n_rows, size=2, replace=False)
        i_a, i_b = int(indices[0]), int(indices[1])

        filters = []
        for col in chosen_cols:
            val_a = self._values[col.name][i_a]
            val_b = self._values[col.name][i_b]
            na_a = self._isna[col.name][i_a]
            na_b = self._isna[col.name][i_b]

            if na_a and na_b:
                continue
            if na_a:
                val_a = val_b
            if na_b:
                val_b = val_a

            if col.filter_type == FilterType.EQUALITY:
                chosen_val = val_a if self.rng.random() < 0.5 else val_b
                filters.append({
                    "attr": col.name,
                    "op": "eq",
                    "value": _to_python(chosen_val),
                })
            else:
                lo, hi = (val_a, val_b) if val_a <= val_b else (val_b, val_a)

                if col.dtype == "datetime":
                    lo_ts = int(pd.Timestamp(lo).timestamp())
                    hi_ts = int(pd.Timestamp(hi).timestamp())
                    if lo_ts == hi_ts:
                        hi_ts = lo_ts + 1
                    filters.append({
                        "attr": col.name,
                        "op": "range",
                        "lo": lo_ts,
                        "hi": hi_ts,
                    })
                else:
                    lo_f = round(float(lo), 2)
                    hi_f = round(float(hi), 2)
                    if lo_f == hi_f:
                        hi_f = lo_f + 0.01
                    filters.append({
                        "attr": col.name,
                        "op": "range",
                        "lo": lo_f,
                        "hi": hi_f,
                    })

        return filters


class GuidedTwoPointFilterGenerator:
    """Categorical-first attribute selection with guided range sampling.

    For low-selectivity queries, blind two-point sampling rarely produces
    a valid pair.  This generator:
      1. Picks categorical (equality) attributes first.
      2. Applies them to estimate the categorical selectivity.
      3. Computes the remaining selectivity budget for range attributes.
      4. Distributes the remaining budget across all range attributes
         via geometric mean (each selects ``remaining^(1/n_range)``).
      5. For each range attribute, samples a span from the pre-sorted
         value array whose length is drawn uniformly from [lo_span, hi_span),
         giving a natural spread of selectivities within the target band.
    """

    def __init__(self, df: pd.DataFrame, columns: list[Column], rng: np.random.Generator):
        self.rng = rng
        self.columns = columns
        self.n_rows = len(df)
        self.df = df
        self.available_columns = [c for c in columns if c.name in df.columns]

        self._values: dict[str, np.ndarray] = {}
        self._isna: dict[str, np.ndarray] = {}
        self._sorted: dict[str, np.ndarray] = {}

        for c in self.available_columns:
            arr = df[c.name].to_numpy()
            self._values[c.name] = arr
            self._isna[c.name] = pd.isna(arr)
            if c.filter_type == FilterType.RANGE:
                valid = arr[~pd.isna(arr)]
                self._sorted[c.name] = np.sort(valid)

    def generate(
        self,
        query_attr_num: int,
        query_attrs: list[str] | None,
        target_selectivity_percent: dict,
    ) -> list[dict]:
        """Generate filters targeting a specific selectivity range."""
        pool = self.available_columns
        if query_attrs is not None:
            attr_set = set(query_attrs)
            pool = [c for c in pool if c.name in attr_set]
        k = min(query_attr_num, len(pool))

        max_retries = 10
        hi_pct = target_selectivity_percent["hi"]
        lo_pct = target_selectivity_percent["lo"]

        while True:
            # --- Pick k attributes randomly ---
            chosen = list(self.rng.choice(pool, size=k, replace=False))
            chosen_eq = [c for c in chosen if c.filter_type == FilterType.EQUALITY]
            chosen_range = [c for c in chosen if c.filter_type == FilterType.RANGE]

            # --- Inner loop: same attributes, re-roll values only ---
            for _inner in range(max_retries):
                # Step 1: eq values
                filters = self._sample_eq_filters(chosen_eq) if chosen_eq else []
                if len(filters) != len(chosen_eq):
                    continue

                cat_frac = compute_selectivity_percent(filters, self.df) / 100.0 if filters else 1.0
                if chosen_eq and cat_frac < hi_pct / 100.0:
                    continue

                # Step 2: range filters with random selectivity split
                if chosen_range:
                    remaining_sel = (lo_pct / 100.0) / cat_frac, (hi_pct / 100.0) / cat_frac
                    n_r = len(chosen_range)
                    weights = self.rng.uniform(0.5, 1.5, size=n_r)
                    weights /= weights.sum()

                    for i, col in enumerate(chosen_range):
                        sorted_vals = self._sorted[col.name]
                        n = len(sorted_vals)
                        if n == 0:
                            continue

                        per_lo = max(0.0, min(1.0, remaining_sel[0] ** weights[i]))
                        per_hi = max(per_lo, min(1.0, remaining_sel[1] ** weights[i]))

                        lo_span = max(1, int(per_lo * n))
                        hi_span = max(lo_span + 1, int(per_hi * n) + 1)
                        hi_span = min(hi_span, n)
                        if lo_span >= hi_span:
                            lo_span = max(1, hi_span - 1)

                        span = int(self.rng.integers(lo_span, hi_span))
                        max_anchor = n - span
                        anchor = int(self.rng.integers(0, max(1, max_anchor + 1)))

                        lo_val = sorted_vals[anchor]
                        hi_val = sorted_vals[min(anchor + span, n - 1)]
                        filters.append(_build_range_filter(col, lo_val, hi_val))

                # Step 3: validate filter count AND selectivity
                if len(filters) != k:
                    continue
                final_sel = compute_selectivity_percent(filters, self.df)
                if lo_pct <= final_sel <= hi_pct:
                    return filters

    def _sample_eq_filters(self, chosen_eq: list[Column]) -> list[dict]:
        """Sample two records and build equality filters from chosen columns."""
        indices = self.rng.choice(self.n_rows, size=2, replace=False)
        i_a, i_b = int(indices[0]), int(indices[1])
        filters = []
        for col in chosen_eq:
            val_a = self._values[col.name][i_a]
            val_b = self._values[col.name][i_b]
            na_a = self._isna[col.name][i_a]
            na_b = self._isna[col.name][i_b]
            if na_a and na_b:
                continue
            if na_a:
                val_a = val_b
            if na_b:
                val_b = val_a
            chosen_val = val_a if self.rng.random() < 0.5 else val_b
            filters.append({
                "attr": col.name, "op": "eq", "value": _to_python(chosen_val),
            })
        return filters


def _build_range_filter(col: Column, lo, hi) -> dict:
    """Build a range filter dict for *col* with bounds [lo, hi)."""
    if col.dtype == "datetime":
        lo_ts = int(pd.Timestamp(lo).timestamp())
        hi_ts = int(pd.Timestamp(hi).timestamp())
        if lo_ts == hi_ts:
            hi_ts += 1
        return {"attr": col.name, "op": "range", "lo": lo_ts, "hi": hi_ts}
    else:
        lo_f = round(float(lo), 2)
        hi_f = round(float(hi), 2)
        if lo_f == hi_f:
            hi_f += 0.01
        return {"attr": col.name, "op": "range", "lo": lo_f, "hi": hi_f}


def compute_selectivity_percent(
    filters: list[dict],
    df: pd.DataFrame,
) -> float:
    """Return the percentage of rows matching all filters (AND semantics)."""
    mask = pd.Series(True, index=df.index)
    for f in filters:
        attr = f["attr"]
        op = f["op"]
        if op == "eq":
            mask &= df[attr] == f["value"]
        elif op == "in":
            mask &= df[attr].isin(f["values"])
        elif op == "range":
            lo, hi = f["lo"], f["hi"]
            if pd.api.types.is_datetime64_any_dtype(df[attr]):
                lo = pd.Timestamp(lo, unit="s")
                hi = pd.Timestamp(hi, unit="s")
            mask &= (df[attr] >= lo) & (df[attr] < hi)
    return float(mask.sum()) / len(df) * 100


def compute_most_selective_attr(
    filters: list[dict],
    df: pd.DataFrame,
) -> str | None:
    """Return the attribute name whose filter matches the fewest rows (most selective).

    Returns ``None`` when there are fewer than 2 filters (no choice to make).
    """
    if len(filters) < 2:
        return None

    best_attr: str | None = None
    best_count = len(df) + 1

    for f in filters:
        attr = f["attr"]
        op = f["op"]
        if op == "eq":
            count = int((df[attr] == f["value"]).sum())
        elif op == "in":
            count = int(df[attr].isin(f["values"]).sum())
        elif op == "range":
            lo, hi = f["lo"], f["hi"]
            if pd.api.types.is_datetime64_any_dtype(df[attr]):
                lo = pd.Timestamp(lo, unit="s")
                hi = pd.Timestamp(hi, unit="s")
            count = int(((df[attr] >= lo) & (df[attr] < hi)).sum())
        else:
            continue

        if count < best_count:
            best_count = count
            best_attr = attr

    return best_attr
