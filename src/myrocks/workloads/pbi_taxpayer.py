"""Public BI benchmark — Taxpayer workload (frozen procurement, plan D5).

Source: cwida/public_bi_benchmark "Taxpayer" workbook (real data: the CMS
Medicare Physician & Other Supplier PUF, despite the name). Instance pinned
to Taxpayer_2 (integration plan D-T1: Taxpayer_10 — referenced by 12/22
original queries — is lost from the Zenodo mirror; all 10 instances share a
byte-identical schema, and ground-truth selectivities were measured on _2).
Canonical table name: `taxpayer` — all 22 vendored queries retarget it.

Frozen pin: workloads/sql/publicbi_taxpayer/MANIFEST.json records sha256 of
the source .gz and the prepared file. verify_data() only verifies —
download/prep is explicit via scripts/prepare_taxpayer.py (never silent).

Source dialect (verified against the benchmark's MonetDB loader:
`copy ... delimiters '|','\\n','' null as 'null'`): pipe-delimited, NO quote
character (quotes are literal data; fields contain neither '|' nor
newlines), NULL = literal `null`. A literal string value "null" is
indistinguishable from NULL — ambiguity inherited from the benchmark.

Prep (PREP_VERSION, binary/byte-transparent): stream-gunzip, assert exactly
28 fields per line, prepend rid (1-based line number; surrogate PK —
BITLSM attr columns cannot be PK columns, and the workbook has no natural
key), rewrite `null` -> \\N, escape literal backslashes (LOAD DATA default
ESCAPED BY '\\'). All engines load the byte-identical prepared file.

Charset stays latin1 server-wide (D-T4: COMMON_ARGS untouched so SSB
args_hash comparability holds). UTF-8 source bytes pass through
byte-transparently: VARBINARY stores raw bytes, VARCHAR latin1 is a 1-byte
charset. Vertica VARCHAR widths are octets, so latin1 char == byte keeps
every value exactly within its declared width.

Filter set (D-T3: union of predicate columns across the 22 queries — same
rule as SSB), VARBINARY on every engine (BITLSM_INDEX requires binary
strings; identical schema keeps engines comparable):
  hcpcs_description, nppes_provider_city, nppes_provider_first_name,
  nppes_provider_last_org_name, nppes_provider_state

Index layouts:
  std          — PK (rid) only
  sk_v1        — std + one SK per filter column
  bi_v1        — std + BITLSM_INDEX over the filter columns (bitlsm engine)
  sk_bi_v1     — sk_v1 + bi coexisting (bitlsm engine): the deployment-
                 realistic "add bi on top of conventional SKs" cell; pairs
                 with plan=ignore_bi to isolate bi's marginal read benefit
  composite_v1 — std + per-template optimal composites (COMPOSITE_INDEXES);
                 upper bound on any realistic SK configuration
"""

import gzip
import hashlib
import json
import os

from .base import ENGINE_CLAUSE, Workload

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PBI_DATA = os.path.join(REPO_ROOT, "data", "sql", "public_bi")
WORKLOAD_DIR = os.path.join(REPO_ROOT, "workloads", "sql",
                            "publicbi_taxpayer")
QUERY_DIR = os.path.join(WORKLOAD_DIR, "queries")
MANIFEST_PATH = os.path.join(WORKLOAD_DIR, "MANIFEST.json")

PREP_VERSION = 1
NULL_TOKEN = b"null"
ZENODO_RECORD = "6344717"  # Public BI part 2 (event.cwi.nl is dead)

# (column, mysql_type, not_null) — order == CSV field order == Vertica DDL
# order (benchmark/Taxpayer/tables/*.table.sql; all 10 instances identical).
# "Number of Records" (Tableau's synthetic always-1 column) is renamed
# number_of_records; no query references it.
COLUMNS = [
    ("number_of_records", "SMALLINT", True),
    ("average_Medicare_allowed_amt", "DOUBLE", False),
    ("average_Medicare_payment_amt", "DOUBLE", False),
    ("average_submitted_chrg_amt", "DOUBLE", False),
    ("bene_day_srvc_cnt", "INT", False),
    ("bene_unique_cnt", "INT", False),
    ("hcpcs_code", "INT", False),
    ("hcpcs_description", "VARBINARY(28)", False),
    ("line_srvc_cnt", "INT", False),
    ("medicare_participation_indicator", "VARCHAR(1)", False),
    ("npi", "INT", True),
    ("nppes_credentials", "VARCHAR(20)", False),
    ("nppes_entity_code", "VARCHAR(1)", False),
    ("nppes_provider_city", "VARBINARY(28)", False),
    ("nppes_provider_country", "VARCHAR(2)", False),
    ("nppes_provider_first_name", "VARBINARY(20)", False),
    ("nppes_provider_gender", "VARCHAR(1)", False),
    ("nppes_provider_last_org_name", "VARBINARY(70)", False),
    ("nppes_provider_mi", "VARCHAR(1)", False),
    ("nppes_provider_state", "VARBINARY(2)", False),
    ("nppes_provider_street1", "VARCHAR(55)", False),
    ("nppes_provider_street2", "VARCHAR(55)", False),
    ("nppes_provider_zip", "INT", False),
    ("place_of_service", "VARCHAR(1)", False),
    ("provider_type", "VARCHAR(43)", False),
    ("stdev_Medicare_allowed_amt", "DOUBLE", False),
    ("stdev_Medicare_payment_amt", "DOUBLE", False),
    ("stdev_submitted_chrg_amt", "DOUBLE", False),
]

FILTER_COLUMNS = [
    "hcpcs_description", "nppes_provider_city", "nppes_provider_first_name",
    "nppes_provider_last_org_name", "nppes_provider_state",
]

# composite_v1 composites — every query template gets a composite over
# exactly its sargable predicate columns (all-equality here; column order
# within an index realizes the longest subset chain, ties broken toward
# the chain absorbing more templates). Duplicate column sets collapse and
# an index is omitted when its full sargable prefix is preserved verbatim
# as the leading prefix of another (index-preserving merge): the chain
# {first,last} < {first,last,state} < {+hcpcs} collapses into comp_01, and
# {hcpcs,state} < {hcpcs,city,state} into comp_02. {last,state} and
# {first,state} are not prefix-realizable inside comp_01 (first/state
# intervene) -> dedicated comp_03/comp_04. Query-specific-best-configuration
# candidates [Chaudhuri & Narasayya, VLDB'97] + index-preserving merges
# [ICDE'99]; upper bound on any realistic SK configuration. q02 has no
# WHERE and full-scans under every layout.
# (name, columns, query templates it was designed for). The query list is
# data, not a comment, because plan=force_composite binds against it: a
# stale comment would silently measure the wrong index.
COMPOSITE_INDEXES = [
    ("comp_01", ["nppes_provider_last_org_name",
                "nppes_provider_first_name",
                "nppes_provider_state",
                "hcpcs_description"],
     ["q04", "q05", "q07", "q09", "q10", "q11", "q14", "q17", "q18", "q19"]),
    ("comp_02", ["hcpcs_description", "nppes_provider_state",
                "nppes_provider_city"],
     ["q03", "q12", "q15", "q16", "q20", "q21", "q22"]),
    ("comp_03", ["nppes_provider_last_org_name",
                "nppes_provider_state"],
     ["q01", "q06"]),
    ("comp_04", ["nppes_provider_first_name",
                "nppes_provider_state"],
     ["q08", "q13"]),
]
# q02 has no WHERE — no composite can help it, and it full-scans everywhere.
COMPOSITE_FOR_QUERY = {f"pbitax_{q}": name
                       for name, _, qs in COMPOSITE_INDEXES for q in qs}

# Stream-counted from the Zenodo files (2026-07-28 feasibility study).
EXPECTED_ROWS_BY_INSTANCE = {1: 9_153_273, 2: 9_153_273}


class PbiTaxpayerWorkload(Workload):

    def __init__(self, instance: int = 2):
        self.instance = instance
        self.name = f"pbi_taxpayer{instance}"
        self.src_gz = os.path.join(PBI_DATA, f"Taxpayer_{instance}.csv.gz")
        self.prepared_path = os.path.join(
            PBI_DATA, f"taxpayer{instance}.v{PREP_VERSION}.csv")
        want = EXPECTED_ROWS_BY_INSTANCE.get(instance)
        self.EXPECTED_ROWS = {"taxpayer": want} if want else {}
        self.EXPECTED_TOTAL_ROWS = want

    # ---- Workload interface ------------------------------------------------

    def verify_data(self):
        if not os.path.exists(MANIFEST_PATH):
            raise RuntimeError(
                "no MANIFEST.json — run scripts/prepare_taxpayer.py first")
        with open(MANIFEST_PATH) as f:
            man = json.load(f)
        if man["instance"] != self.instance \
                or man["prep_version"] != PREP_VERSION:
            raise RuntimeError(
                f"MANIFEST pins instance {man['instance']} "
                f"prep v{man['prep_version']}, wanted instance "
                f"{self.instance} prep v{PREP_VERSION}")
        if not os.path.exists(self.prepared_path):
            raise RuntimeError(
                f"{self.prepared_path} missing — run "
                "scripts/prepare_taxpayer.py (data is never auto-downloaded)")
        if file_sha256(self.prepared_path) != man["prepared_sha256"]:
            raise RuntimeError("prepared file sha256 mismatch vs MANIFEST — "
                               "refusing to load unverified data")

    def tables(self):
        return ["taxpayer"]

    def data_file(self, table):
        return self.prepared_path

    def create_table_sql(self, table, engine, index_layout):
        cols = ["    rid BIGINT NOT NULL"]
        cols += [f"    {c} {t}{' NOT NULL' if nn else ''}"
                 for c, t, nn in COLUMNS]
        cols.append("    PRIMARY KEY (rid)")
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
        cols = ", ".join(["rid"] + [c for c, _, _ in COLUMNS])
        return (f"LOAD DATA LOCAL INFILE '{csv_path}' INTO TABLE {table} "
                f"CHARACTER SET latin1 "
                f"FIELDS TERMINATED BY '|' ESCAPED BY '\\\\' "
                f"LINES TERMINATED BY '\\n' "
                f"({cols})")

    def bi_tables(self, index_layout):
        return {"taxpayer"} if index_layout in ("bi_v1", "sk_bi_v1") else set()

    def secondary_indexes(self, index_layout):
        sks = [f"sk_{c.lower()}" for c in FILTER_COLUMNS]
        if index_layout == "sk_v1":
            return {"taxpayer": sks}
        if index_layout == "composite_v1":
            return {"taxpayer": [n for n, _, _ in COMPOSITE_INDEXES]}
        if index_layout == "bi_v1":
            return {"taxpayer": ["bi"]}
        if index_layout == "sk_bi_v1":
            return {"taxpayer": sks + ["bi"]}
        return {}

    def histogram_columns(self):
        return list(FILTER_COLUMNS)

    def composite_index_for(self, query_id):
        return COMPOSITE_FOR_QUERY.get(query_id)

    # ---- write axis (ingest_run.py) ---------------------------------------

    def insert_columns(self):
        return ["rid"] + [c for c, _, _ in COLUMNS]

    def parse_row(self, line):
        # The prepared file carries LOAD DATA's NULL token; the client
        # protocol needs a real None instead.
        return tuple(None if v == "\\N" else v
                     for v in line.rstrip("\n").split("|"))

    def total_rows(self):
        with open(MANIFEST_PATH) as f:
            return json.load(f)["rows"]

    def queries(self):
        out = {}
        for fname in sorted(os.listdir(QUERY_DIR)):
            if fname.endswith(".sql"):
                with open(os.path.join(QUERY_DIR, fname)) as f:
                    out[f"pbitax_{fname[:-4]}"] = f.read().strip()
        return out


# ---- prep (called by scripts/prepare_taxpayer.py) ---------------------------

def prepare(workload, log=print) -> dict:
    """Deterministic source->prepared transform (byte-transparent, binary).
    Returns the manifest fields."""
    ncols = len(COLUMNS)
    tmp = workload.prepared_path + ".tmp"
    rows = 0
    nulls = 0
    with gzip.open(workload.src_gz, "rb") as fin, open(tmp, "wb") as fout:
        for line in fin:
            fields = line.rstrip(b"\n").split(b"|")
            if len(fields) != ncols:
                raise RuntimeError(
                    f"line {rows + 1}: {len(fields)} fields, "
                    f"expected {ncols}")
            rows += 1
            out = [str(rows).encode()]
            for v in fields:
                if v == NULL_TOKEN:
                    nulls += 1
                    out.append(b"\\N")
                else:
                    out.append(v.replace(b"\\", b"\\\\"))
            fout.write(b"|".join(out) + b"\n")
            if rows % 1_000_000 == 0:
                log(f"  {rows} rows ...")
    os.replace(tmp, workload.prepared_path)
    return {
        "instance": workload.instance,
        "prep_version": PREP_VERSION,
        "rows": rows,
        "null_fields": nulls,
        "source_gz_sha256": file_sha256(workload.src_gz),
        "prepared_sha256": file_sha256(workload.prepared_path),
    }


def file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
