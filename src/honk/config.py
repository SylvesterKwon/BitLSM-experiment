"""Workload configuration loading, validation, and Cartesian expansion."""

import itertools
import json
import math
from dataclasses import dataclass, field


class HonkConfigError(Exception):
    """Raised for invalid workload configuration."""
    pass


# --- Dataclasses ---

@dataclass
class ExpandedQueryBlock:
    label: str
    strategy: str  # "uniform" | "two_point"
    weight: float
    query_attr_num: int | None = None
    query_attrs: list[str] | None = None
    target_selectivity_percent: dict | None = None  # {"lo": float, "hi": float}


@dataclass
class WriteOnlyPhase:
    label: str
    rows: int | None = None
    update_ratio: float = 0.0


@dataclass
class PausePhase:
    label: str
    duration_seconds: float


@dataclass
class ReadOnlyPhase:
    label: str
    num_queries: int | None = None
    queries: list[ExpandedQueryBlock] = field(default_factory=list)


@dataclass
class MixedPhase:
    label: str
    rows: int | None = None
    read_ratio: float = 0.0
    update_ratio: float = 0.0
    queries: list[ExpandedQueryBlock] = field(default_factory=list)


Phase = WriteOnlyPhase | PausePhase | ReadOnlyPhase | MixedPhase


@dataclass
class WorkloadConfig:
    dataset: list[str]
    seed: int
    phases: list[Phase]

    @property
    def total_write_rows(self) -> int:
        """Total rows consumed across all phases (writes + updates)."""
        total = 0
        for p in self.phases:
            if isinstance(p, (WriteOnlyPhase, MixedPhase)):
                total += p.rows + math.ceil(p.rows * p.update_ratio)
        return total


# --- Cartesian expansion ---

EXPAND_FIELDS = ("query_attr_num",)


def _expand_queries(raw_queries: list[dict]) -> list[ExpandedQueryBlock]:
    """Expand query blocks via Cartesian product of list-valued params."""
    result = []
    for q in raw_queries:
        _validate_query_block(q)
        label = q["label"]
        strategy = q["strategy"]
        weight = q.get("weight", 1)
        query_attrs = q.get("query_attrs")
        target_selectivity_percent = q.get("target_selectivity_percent")

        # Identify list-valued expansion params
        expand_params: dict[str, list] = {}
        for key in EXPAND_FIELDS:
            val = q.get(key)
            if val is None:
                continue
            if isinstance(val, list):
                expand_params[key] = val
            else:
                expand_params[key] = [val]

        if not expand_params:
            result.append(ExpandedQueryBlock(
                label=label,
                strategy=strategy,
                weight=weight,
                query_attr_num=q.get("query_attr_num"),
                query_attrs=query_attrs,
                target_selectivity_percent=target_selectivity_percent,
            ))
            continue

        keys = list(expand_params.keys())
        for combo in itertools.product(*[expand_params[k] for k in keys]):
            overrides = dict(zip(keys, combo))
            result.append(ExpandedQueryBlock(
                label=label,
                strategy=strategy,
                weight=weight,
                query_attr_num=overrides.get("query_attr_num"),
                query_attrs=query_attrs,
                target_selectivity_percent=target_selectivity_percent,
            ))

    return result


# --- Validation ---

def _validate_query_block(q: dict) -> None:
    """Validate a single query block definition."""
    label = q.get("label", "<unnamed>")
    strategy = q.get("strategy")

    if strategy not in ("uniform", "two_point", "guided_two_point"):
        raise HonkConfigError(
            f"Query '{label}': unknown strategy '{strategy}', expected 'uniform', 'two_point', or 'guided_two_point'"
        )

    if "query_attr_num" not in q:
        raise HonkConfigError(
            f"Query '{label}': 'query_attr_num' is required"
        )

    tsp = q.get("target_selectivity_percent")
    if tsp is not None:
        if not isinstance(tsp, dict) or "lo" not in tsp or "hi" not in tsp:
            raise HonkConfigError(
                f"Query '{label}': 'target_selectivity_percent' must have 'lo' and 'hi' keys"
            )
        lo, hi = tsp["lo"], tsp["hi"]
        if not (0 <= lo <= hi <= 100):
            raise HonkConfigError(
                f"Query '{label}': 'target_selectivity_percent' requires 0 <= lo <= hi <= 100, got lo={lo}, hi={hi}"
            )

    query_attrs = q.get("query_attrs")
    if query_attrs is not None:
        if not isinstance(query_attrs, list) or not all(isinstance(c, str) for c in query_attrs):
            raise HonkConfigError(
                f"Query '{label}': 'query_attrs' must be a list of column name strings"
            )
        from .schema import ALL_COLUMN_NAMES
        unknown = set(query_attrs) - set(ALL_COLUMN_NAMES)
        if unknown:
            raise HonkConfigError(
                f"Query '{label}': unknown query_attrs: {unknown}"
            )


def _parse_phase(raw: dict) -> Phase:
    """Parse a single phase from raw JSON dict."""
    label = raw.get("label", "<unnamed>")
    phase_type = raw.get("type")

    if phase_type == "write_only":
        return WriteOnlyPhase(
            label=label,
            rows=raw.get("rows"),
            update_ratio=raw.get("update_ratio", 0.0),
        )

    elif phase_type == "pause":
        return PausePhase(label=label, duration_seconds=raw["duration_seconds"])

    elif phase_type == "read_only":
        queries = _expand_queries(raw.get("queries", []))
        return ReadOnlyPhase(
            label=label,
            num_queries=raw.get("num_queries"),
            queries=queries,
        )

    elif phase_type == "mixed":
        queries = _expand_queries(raw.get("queries", []))
        return MixedPhase(
            label=label,
            rows=raw.get("rows"),
            read_ratio=raw.get("read_ratio", 0.0),
            update_ratio=raw.get("update_ratio", 0.0),
            queries=queries,
        )

    else:
        raise HonkConfigError(f"Phase '{label}': unknown type '{phase_type}'")


def load_config(path: str) -> WorkloadConfig:
    """Load and validate a workload configuration from JSON."""
    with open(path) as f:
        raw = json.load(f)

    dataset = raw.get("dataset", [])
    if not dataset:
        raise HonkConfigError("'dataset' must be a non-empty list of file paths")

    seed = raw.get("seed", 42)
    phases = [_parse_phase(p) for p in raw.get("phases", [])]

    return WorkloadConfig(dataset=dataset, seed=seed, phases=phases)


def resolve_rows(config: WorkloadConfig, available_rows: int) -> None:
    """Fill in None rows with available_rows (entire dataset)."""
    for p in config.phases:
        if isinstance(p, (WriteOnlyPhase, MixedPhase)) and p.rows is None:
            p.rows = available_rows
        if isinstance(p, ReadOnlyPhase) and p.num_queries is None:
            p.num_queries = available_rows


def validate_row_budget(config: WorkloadConfig, available_rows: int) -> None:
    """Check that phases don't consume more rows than available."""
    needed = config.total_write_rows
    if needed > available_rows:
        raise HonkConfigError(
            f"Phases require {needed:,} rows but dataset has only {available_rows:,}"
        )
