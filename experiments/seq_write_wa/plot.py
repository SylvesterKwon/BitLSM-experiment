#!/usr/bin/env python3
"""Plot Write Amplification (WA) comparison across methods for seq_write_wa."""

import argparse
import csv
import os
import re

import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.size": 6})


SCHEMAS = ["a1", "a2", "a4", "a8", "a16", "a32"]
SCHEMA_TICK_LABELS = ["a=1", "a=2", "a=4", "a=8", "a=16", "a=32"]

METHOD_ORDER = [
    "no-index",
    "si-lu",
    "si-ck",
    "si-eager",
    "bitlsm_rho0.2",
    "bitlsm_rho0.1",
    "bitlsm_rho0.05",
]
METHOD_LABELS = {
    "no-index": "No Index",
    "si-lu": "Lazy",
    "si-ck": "Composite",
    "si-eager": "SI-Eager",
    "bitlsm_rho0.2": r"BitLSM ($\rho$=0.2)",
    "bitlsm_rho0.1": r"BitLSM ($\rho$=0.1)",
    "bitlsm_rho0.05": r"BitLSM ($\rho$=0.05)",
}
METHOD_COLORS = {
    "bitlsm_rho0.2": "#F08C7C",
    "bitlsm_rho0.1": "#E04040",
    "bitlsm_rho0.05": "#9B1B1B",
    "si-lu": "#4CC850",
    "si-ck": "#9888B8",
}

_SCHEMA_RE = re.compile(r"_a(\d+)_")


def schema_axis_key(schema_field: str):
    """Extract 'aN' (e.g. 'a8') from a schema string like 'default_a8_c100'."""
    if not schema_field:
        return None
    m = _SCHEMA_RE.search(schema_field)
    return f"a{m.group(1)}" if m else None


def load_rows(csv_path: str):
    """Return {method: {schema_key: (user_bytes, flush_bytes, compact_bytes, wa)}}."""
    data: dict[str, dict[str, tuple[int, int, int, float]]] = {}
    with open(csv_path, newline="") as f:
        content = f.read().replace("\r\n", "\n").replace("\r", "\n")
    reader = csv.DictReader(content.strip().splitlines())
    for row in reader:
        method = row["method"].strip()
        skey = schema_axis_key(row.get("schema", "").strip())
        if not skey:
            continue
        try:
            user = int(row["user_bytes"])
            flush = int(row["flush_bytes"])
            compact = int(row["compact_bytes"])
            wa = float(row["wa"])
        except (KeyError, ValueError):
            continue
        data.setdefault(method, {})[skey] = (user, flush, compact, wa)
    return data


def get_ordered_methods(data_keys):
    ordered = [m for m in METHOD_ORDER if m in data_keys]
    remaining = sorted(set(data_keys) - set(METHOD_ORDER))
    return ordered + remaining


def label_for(method):
    return METHOD_LABELS.get(method, method)


def color_for(method):
    return METHOD_COLORS.get(method, None)


def values_for(data, method, value_idx):
    """Return list aligned with SCHEMAS, NaN for missing."""
    out = []
    for s in SCHEMAS:
        rec = data.get(method, {}).get(s)
        out.append(rec[value_idx] if rec is not None else float("nan"))
    return out


def plot_wa(data, output_dir):
    """Bar chart: WA per method per schema."""
    methods = get_ordered_methods(data.keys())

    fig, ax = plt.subplots(figsize=(7, 7 * 0.618))
    x = np.arange(len(SCHEMAS))
    n = len(methods)
    width = 0.8 / n

    for i, method in enumerate(methods):
        vals = values_for(data, method, 3)  # wa
        offset = (i - n / 2 + 0.5) * width
        color = color_for(method)
        bars = ax.bar(x + offset, vals, width, label=label_for(method),
                      **({"color": color} if color else {}))
        for bar, v in zip(bars, vals):
            if not np.isnan(v):
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    bar.get_height(),
                    f"{v:.2f}",
                    ha="center",
                    va="bottom",
                )

    ax.set_ylabel("Write Amplification")

    ax.set_xticks(x)
    ax.set_xticklabels(SCHEMA_TICK_LABELS)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", length=2, width=0.3, direction="in")
    ax.legend()
    ax.grid(False)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "wa_comparison.pdf")
    fig.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Plot Write Amplification comparison"
    )
    parser.add_argument("csv", help="Path to master CSV file")
    parser.add_argument(
        "-o", "--output-dir", default=None,
        help="Output directory (default: same as CSV)"
    )
    args = parser.parse_args()

    output_dir = args.output_dir or os.path.dirname(args.csv)
    os.makedirs(output_dir, exist_ok=True)

    data = load_rows(args.csv)
    if not data:
        print("No WA data found.")
        return

    plot_wa(data, output_dir)


if __name__ == "__main__":
    main()
