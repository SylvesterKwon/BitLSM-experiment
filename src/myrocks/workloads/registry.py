"""Workload registry — the single place run.py/perf_run.py resolve
config["workload"] strings into Workload instances."""

from .pbi_taxpayer import PbiTaxpayerWorkload
from .ssb_flat import SsbFlatWorkload


def make_workload(config):
    kind = config["workload"]
    if kind == "ssbflat":
        return SsbFlatWorkload(sf=config.get("sf", 1))
    if kind == "pbi_taxpayer":
        return PbiTaxpayerWorkload(instance=config.get("instance", 2))
    raise ValueError(f"unknown workload: {kind}")
