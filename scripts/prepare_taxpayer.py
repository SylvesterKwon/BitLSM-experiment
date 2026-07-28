#!/usr/bin/env python3
"""Download (Zenodo, md5-verified) + prepare + pin the Taxpayer data.

Explicit by design: the harness' verify_data() never downloads. Re-running
with an existing MANIFEST verifies against the pin instead of re-pinning
(frozen procurement — see workloads/sql/publicbi_taxpayer/README.md).

Usage: python3 scripts/prepare_taxpayer.py [instance]   (default: 2)
"""
import json
import os
import subprocess
import sys
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from myrocks.workloads.pbi_taxpayer import (  # noqa: E402
    MANIFEST_PATH, PBI_DATA, ZENODO_RECORD, PbiTaxpayerWorkload, prepare)


def zenodo_file(instance):
    """(md5, download_url) for Taxpayer_<N>.csv.gz from the Zenodo record."""
    url = f"https://zenodo.org/api/records/{ZENODO_RECORD}"
    with urllib.request.urlopen(url) as r:
        rec = json.load(r)
    fname = f"Taxpayer_{instance}.csv.gz"
    for f in rec["files"]:
        if f["key"] == fname:
            md5 = f["checksum"].split(":", 1)[1]
            return md5, f["links"]["self"]
    raise SystemExit(f"{fname} not in Zenodo record {ZENODO_RECORD} "
                     "(Taxpayer_10 is known-missing from the mirror)")


def main():
    instance = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    w = PbiTaxpayerWorkload(instance)
    os.makedirs(PBI_DATA, exist_ok=True)

    if not os.path.exists(w.src_gz):
        md5, url = zenodo_file(instance)
        print(f"downloading {url} ...")
        subprocess.run(["curl", "-L", "--fail", "-o", w.src_gz + ".part",
                        url], check=True)
        got = subprocess.run(["md5sum", w.src_gz + ".part"],
                             capture_output=True, text=True,
                             check=True).stdout.split()[0]
        if got != md5:
            raise SystemExit(f"md5 mismatch: got {got} want {md5}")
        os.replace(w.src_gz + ".part", w.src_gz)
        print(f"downloaded + md5-verified: {w.src_gz}")

    print(f"preparing {w.prepared_path} ...")
    man = prepare(w)
    print(json.dumps(man, indent=2))

    if os.path.exists(MANIFEST_PATH):
        with open(MANIFEST_PATH) as f:
            pinned = json.load(f)
        if pinned != man:
            raise SystemExit("MANIFEST mismatch vs existing pin — data or "
                             "prep changed; refusing to silently re-pin")
        print("matches existing MANIFEST — verified")
    else:
        with open(MANIFEST_PATH, "w") as f:
            json.dump(man, f, indent=2)
            f.write("\n")
        print(f"pinned {MANIFEST_PATH} — commit this file")


if __name__ == "__main__":
    main()
