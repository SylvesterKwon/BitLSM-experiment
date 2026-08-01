"""SSB denormalized (lineorder_flat) workload — the initial-scope benchmark.

Generated workload (harness plan D5): third_party/ssb-dbgen (pinned submodule)
emits the 5 normalized .tbl files for a scale factor; an offline streaming
join materializes lineorder_flat as a single pipe-delimited file that all
three engines load byte-identically. Flat column set follows ClickHouse's
lineorder_flat (incl. materialized date attributes D_YEAR/D_YEARMONTHNUM/
D_YEARMONTH/D_WEEKNUMINYEAR) so all 13 queries are sargable single-table
conjunctive selections. LO_ORDERDATE stays an integer datekey (yyyymmdd).

Index layouts (harness plan D6, flat reinterpretation):
  std          — composite PK only (auto has nothing but scans; lower ref)
  sk_v1        — std + one SK per filter column (index_merge arena)
  bi_v1        — std + BITLSM_INDEX over the same filter columns (bitlsm)
  composite_v1 — std + per-template optimal composites (COMPOSITE_INDEXES);
                 upper bound on any realistic SK configuration

Filter-target dim strings are VARBINARY on ALL engines (BITLSM_INDEX takes
binary strings; identical schema keeps the comparison fair).
"""

import hashlib
import json
import os
import subprocess

from .base import ENGINE_CLAUSE, Workload

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DBGEN_SRC = os.path.join(REPO_ROOT, "third_party", "ssb-dbgen")
SSB_DATA = os.path.join(REPO_ROOT, "data", "sql", "ssb")
QUERY_DIR = os.path.join(REPO_ROOT, "workloads", "sql", "ssb_flat", "queries")

FLATTEN_VERSION = 1  # bump on any change to the flat schema/join logic

# (column, type, not_null) — order == on-disk column order of the flat file.
FLAT_COLUMNS = [
    ("LO_ORDERKEY", "BIGINT", True),
    ("LO_LINENUMBER", "INT", True),
    ("LO_CUSTKEY", "INT", True),
    ("LO_PARTKEY", "INT", True),
    ("LO_SUPPKEY", "INT", True),
    ("LO_ORDERDATE", "INT", True),          # yyyymmdd datekey
    ("LO_ORDERPRIORITY", "VARCHAR(15)", True),
    ("LO_SHIPPRIORITY", "INT", True),
    ("LO_QUANTITY", "INT", True),
    ("LO_EXTENDEDPRICE", "INT", True),
    ("LO_ORDTOTALPRICE", "INT", True),
    ("LO_DISCOUNT", "INT", True),
    ("LO_REVENUE", "INT", True),
    ("LO_SUPPLYCOST", "INT", True),
    ("LO_TAX", "INT", True),
    ("LO_COMMITDATE", "INT", True),
    ("LO_SHIPMODE", "VARCHAR(10)", True),
    ("C_NAME", "VARCHAR(25)", True),
    ("C_ADDRESS", "VARCHAR(25)", True),
    ("C_CITY", "VARBINARY(10)", True),
    ("C_NATION", "VARBINARY(15)", True),
    ("C_REGION", "VARBINARY(12)", True),
    ("C_PHONE", "VARCHAR(15)", True),
    ("C_MKTSEGMENT", "VARCHAR(10)", True),
    ("S_NAME", "VARCHAR(25)", True),
    ("S_ADDRESS", "VARCHAR(25)", True),
    ("S_CITY", "VARBINARY(10)", True),
    ("S_NATION", "VARBINARY(15)", True),
    ("S_REGION", "VARBINARY(12)", True),
    ("S_PHONE", "VARCHAR(15)", True),
    ("P_NAME", "VARCHAR(22)", True),
    ("P_MFGR", "VARBINARY(6)", True),
    ("P_CATEGORY", "VARBINARY(7)", True),
    ("P_BRAND", "VARBINARY(9)", True),
    ("P_COLOR", "VARCHAR(11)", True),
    ("P_TYPE", "VARCHAR(25)", True),
    ("P_SIZE", "INT", True),
    ("P_CONTAINER", "VARCHAR(10)", True),
    ("D_YEAR", "SMALLINT", True),
    ("D_YEARMONTHNUM", "INT", True),
    ("D_YEARMONTH", "VARCHAR(7)", True),
    ("D_WEEKNUMINYEAR", "TINYINT", True),
]

# Union of predicate columns across the 13 flat queries (D6 sk/bi target).
FILTER_COLUMNS = [
    "D_YEAR", "D_YEARMONTHNUM", "D_WEEKNUMINYEAR",
    "LO_DISCOUNT", "LO_QUANTITY",
    "P_MFGR", "P_CATEGORY", "P_BRAND",
    "S_REGION", "S_NATION", "S_CITY",
    "C_REGION", "C_NATION", "C_CITY",
]

# composite_v1 composites — every query template gets a composite over
# exactly its sargable predicate columns (equality columns leading, the
# range column last; later range columns stay in the key as in-index
# filters). Duplicate column sets collapse (q2_2/q2_3 -> comp_05), and an
# index is omitted when its full sargable prefix is preserved verbatim as
# the leading prefix of another (q4_1 rides comp_10's 3-prefix). This is
# the query-specific-best-configuration candidate set [Chaudhuri &
# Narasayya, VLDB'97] with index-preserving prefix merges [ICDE'99] — an
# upper bound on any realistic SK configuration, so read wins against it
# transfer to every weaker real-world layout.
# (name, columns, query templates it was designed for). The query list is
# data, not a comment, because plan=force_composite binds against it: a
# stale comment would silently measure the wrong index.
COMPOSITE_INDEXES = [
    ("comp_01", ["D_YEAR", "LO_DISCOUNT", "LO_QUANTITY"], ["q1_1"]),
    ("comp_02", ["D_YEARMONTHNUM", "LO_DISCOUNT", "LO_QUANTITY"], ["q1_2"]),
    ("comp_03", ["D_WEEKNUMINYEAR", "D_YEAR",
                "LO_DISCOUNT", "LO_QUANTITY"], ["q1_3"]),
    ("comp_04", ["P_CATEGORY", "S_REGION"], ["q2_1"]),
    ("comp_05", ["S_REGION", "P_BRAND"], ["q2_2", "q2_3"]),
    ("comp_06", ["C_REGION", "S_REGION", "D_YEAR"], ["q3_1"]),
    ("comp_07", ["C_NATION", "S_NATION", "D_YEAR"], ["q3_2"]),
    ("comp_08", ["C_CITY", "S_CITY", "D_YEAR"], ["q3_3"]),
    ("comp_09", ["C_CITY", "S_CITY", "D_YEARMONTHNUM"], ["q3_4"]),
    ("comp_10", ["C_REGION", "S_REGION", "P_MFGR", "D_YEAR"],
     ["q4_1", "q4_2"]),
    ("comp_11", ["S_NATION", "P_CATEGORY", "C_REGION", "D_YEAR"], ["q4_3"]),
]
COMPOSITE_FOR_QUERY = {f"ssbflat_{q}": name
                       for name, _, qs in COMPOSITE_INDEXES for q in qs}

# Canonical dbgen row counts, verified 2026-07-17 @ submodule pin (SF1).
EXPECTED_TBL_ROWS_SF1 = {
    "customer": 30_000, "part": 200_000, "supplier": 2_000,
    "date": 2_557, "lineorder": 6_001_215,
}


class SsbFlatWorkload(Workload):

    def __init__(self, sf: int = 1):
        self.sf = sf
        self.name = f"ssbflat_sf{sf}"
        self.gen_dir = os.path.join(SSB_DATA, f"sf{sf}")
        self.flat_path = os.path.join(
            self.gen_dir, f"lineorder_flat.v{FLATTEN_VERSION}.tbl")
        self.EXPECTED_ROWS = {}
        self.EXPECTED_TOTAL_ROWS = None
        if sf == 1:
            self.EXPECTED_ROWS = {
                "lineorder_flat": EXPECTED_TBL_ROWS_SF1["lineorder"]}
            self.EXPECTED_TOTAL_ROWS = EXPECTED_TBL_ROWS_SF1["lineorder"]

    # ---- generation -------------------------------------------------------

    def _dbgen_binary(self) -> str:
        build = os.path.join(SSB_DATA, "dbgen-build")
        binary = os.path.join(build, "dbgen")
        if not os.path.exists(binary):
            subprocess.run(["cmake", "-S", DBGEN_SRC, "-B", build,
                            "-DCMAKE_BUILD_TYPE=Release"],
                           check=True, capture_output=True)
            subprocess.run(["cmake", "--build", build],
                           check=True, capture_output=True)
        return binary

    def _ensure_tbls(self):
        missing = [t for t in EXPECTED_TBL_ROWS_SF1
                   if not os.path.exists(os.path.join(self.gen_dir, f"{t}.tbl"))]
        if not missing:
            return
        binary = self._dbgen_binary()
        os.makedirs(self.gen_dir, exist_ok=True)
        env = dict(os.environ, DSS_CONFIG=os.path.dirname(binary))
        for flag in ("c", "p", "s", "d", "l"):
            subprocess.run([binary, "-f", "-s", str(self.sf), "-T", flag],
                           check=True, cwd=self.gen_dir, env=env,
                           capture_output=True)

    def _ensure_flat(self, log=print):
        """Streaming join: dims into memory (SF-proportional but small),
        lineorder streamed once. Deterministic given (.tbl set, version)."""
        if os.path.exists(self.flat_path):
            return
        self._ensure_tbls()
        g = self.gen_dir

        def read_dim(fname, keycol=0):
            out = {}
            with open(os.path.join(g, fname)) as f:
                for line in f:
                    parts = line.rstrip("\n").rstrip("|").split("|")
                    out[parts[keycol]] = parts
            return out

        log(f"[{self.name}] flattening (v{FLATTEN_VERSION}) ...")
        cust = read_dim("customer.tbl")
        supp = read_dim("supplier.tbl")
        part = read_dim("part.tbl")
        date = read_dim("date.tbl")

        tmp = self.flat_path + ".tmp"
        rows = 0
        with open(os.path.join(g, "lineorder.tbl")) as fin, \
                open(tmp, "w") as fout:
            for line in fin:
                lo = line.rstrip("\n").rstrip("|").split("|")
                c, s, p = cust[lo[2]], supp[lo[4]], part[lo[3]]
                d = date[lo[5]]
                # C_(NAME..MKTSEGMENT)=c[1:8], S_(NAME..PHONE)=s[1:7],
                # P_(NAME..CONTAINER)=p[1:9], D_YEAR=d[4],
                # D_YEARMONTHNUM=d[5], D_YEARMONTH=d[6], D_WEEKNUMINYEAR=d[11]
                fout.write("|".join(
                    lo + c[1:8] + s[1:7] + p[1:9]
                    + [d[4], d[5], d[6], d[11]]) + "\n")
                rows += 1
        os.replace(tmp, self.flat_path)

        meta = {
            "flatten_version": FLATTEN_VERSION,
            "sf": self.sf,
            "rows": rows,
            "dbgen_commit": subprocess.run(
                ["git", "-C", DBGEN_SRC, "rev-parse", "HEAD"],
                capture_output=True, text=True).stdout.strip(),
            "sha256": _file_sha256(self.flat_path),
        }
        with open(self.flat_path + ".meta.json", "w") as f:
            json.dump(meta, f, indent=2)
        log(f"[{self.name}] flat rows: {rows}")

    # ---- Workload interface ------------------------------------------------

    def verify_data(self):
        """Generated workload: ensure the flat file exists (generating it if
        needed) and matches its recorded meta hash."""
        self._ensure_flat()
        with open(self.flat_path + ".meta.json") as f:
            meta = json.load(f)
        if meta["flatten_version"] != FLATTEN_VERSION:
            raise RuntimeError("flat file built by a different flatten "
                               "version — delete it to regenerate")
        if _file_sha256(self.flat_path) != meta["sha256"]:
            raise RuntimeError("lineorder_flat sha256 mismatch vs meta — "
                               "refusing to load corrupted data")

    def tables(self):
        return ["lineorder_flat"]

    def data_file(self, table):
        return self.flat_path

    def create_table_sql(self, table, engine, index_layout):
        cols = [f"    {c} {t}{' NOT NULL' if nn else ''}"
                for c, t, nn in FLAT_COLUMNS]
        cols.append("    PRIMARY KEY (LO_ORDERKEY, LO_LINENUMBER)")
        if index_layout in ("sk_v1", "sk_bi_v1"):
            for c in FILTER_COLUMNS:
                cols.append(f"    KEY sk_{c.lower()} ({c})")
        if index_layout == "composite_v1":
            for name, icols, _ in COMPOSITE_INDEXES:
                cols.append(f"    KEY {name} ({', '.join(icols)})")
        if index_layout in ("bi_v1", "sk_bi_v1"):
            assert engine == "bitlsm", "bi layouts are bitlsm-engine only"
            cols.append(
                f"    INDEX bi ({', '.join(FILTER_COLUMNS)}) BITLSM_INDEX")
        if index_layout not in ("std", "sk_v1", "bi_v1", "sk_bi_v1",
                                "composite_v1"):
            raise ValueError(f"unknown index_layout: {index_layout}")
        body = ",\n".join(cols)
        return (f"CREATE TABLE {table} (\n{body}\n) "
                f"ENGINE={ENGINE_CLAUSE[engine]}")

    def load_sql(self, table, csv_path):
        cols = ", ".join(c for c, _, _ in FLAT_COLUMNS)
        return (f"LOAD DATA LOCAL INFILE '{csv_path}' INTO TABLE {table} "
                f"CHARACTER SET latin1 "
                f"FIELDS TERMINATED BY '|' LINES TERMINATED BY '\\n' "
                f"({cols})")

    def bi_tables(self, index_layout):
        return ({"lineorder_flat"}
                if index_layout in ("bi_v1", "sk_bi_v1") else set())

    def secondary_indexes(self, index_layout):
        sks = [f"sk_{c.lower()}" for c in FILTER_COLUMNS]
        if index_layout == "sk_v1":
            return {"lineorder_flat": sks}
        if index_layout == "composite_v1":
            return {"lineorder_flat": [n for n, _, _ in COMPOSITE_INDEXES]}
        if index_layout == "bi_v1":
            return {"lineorder_flat": ["bi"]}
        if index_layout == "sk_bi_v1":
            return {"lineorder_flat": sks + ["bi"]}
        return {}

    def histogram_columns(self):
        return list(FILTER_COLUMNS)

    def composite_index_for(self, query_id):
        return COMPOSITE_FOR_QUERY.get(query_id)

    # ---- write axis (ingest_run.py) ---------------------------------------

    def insert_columns(self):
        return [c for c, _, _ in FLAT_COLUMNS]

    def parse_row(self, line):
        return tuple(line.rstrip("\n").split("|"))

    def total_rows(self):
        with open(self.flat_path + ".meta.json") as f:
            return json.load(f)["rows"]

    def queries(self):
        out = {}
        for fname in sorted(os.listdir(QUERY_DIR)):
            if fname.endswith(".sql"):
                with open(os.path.join(QUERY_DIR, fname)) as f:
                    out[f"ssbflat_{fname[:-4]}"] = f.read().strip()
        return out


def _file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
