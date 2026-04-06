"""Column definitions loaded from schema.json."""

import json
import os
from dataclasses import dataclass
from enum import Enum


class FilterType(Enum):
    EQUALITY = "equality"
    RANGE = "range"


@dataclass
class Column:
    name: str
    filter_type: FilterType
    dtype: str  # "int", "float", "datetime", "str"


def load_columns(schema_path: str | None = None) -> list[Column]:
    """Load column definitions from schema.json."""
    if schema_path is None:
        schema_path = os.path.join(os.path.dirname(__file__), "..", "schema.json")
    with open(schema_path) as f:
        data = json.load(f)

    columns = []
    for field in data["fields"]:
        if "codes" in field:
            filter_type = FilterType.EQUALITY
        else:
            filter_type = FilterType.RANGE
        columns.append(Column(field["name"], filter_type, field["type"]))
    return columns


ALL_COLUMNS: list[Column] = load_columns()
ALL_COLUMN_NAMES: list[str] = [c.name for c in ALL_COLUMNS]
ALL_COLUMN_MAP: dict[str, Column] = {c.name: c for c in ALL_COLUMNS}
