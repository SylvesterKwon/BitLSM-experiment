"""Workload interface for the myrocks_optimizer experiment family.

A workload = {schema DDL, data source, query set, index layouts}, versioned
as one unit. Adding a new benchmark set = one subclass + vendored queries
+ an exp_set entry (harness plan D5). Procurement rules: generated sets pin
their generator (submodule + params); frozen sets pin a checksum MANIFEST.
"""

ENGINE_CLAUSE = {
    "innodb": "InnoDB",
    "myrocks": "ROCKSDB",
    "bitlsm": "ROCKSDB",  # same SE; differs by index layout (bi indexes)
}

# Secondary keys go to their own CF on the RocksDB engines; InnoDB has no CFs
# and its DDL is unaffected. The BitLSM estimator normalizes selectivity by the
# key count of the CF hosting the table's rows, while the optimizer multiplies
# that back out by the server's logical row count -- so every SK entry sharing
# that CF diluted the row estimate by exactly (CF keys / table rows). Measured
# 2026-07-31: 0/1/3/7 SKs gave 1.00/2.00/4.00/8.00x under-estimation, and SSB
# sk_bi_v1's q-error median was 1878 against bi_v1's 1.08. Splitting the SKs
# out leaves the data CF holding rows only, which is the condition the
# estimator assumes. Applied to every SK-bearing rocksdb layout, not just the
# bi ones, so index placement stays one less difference between the cells.
# The block cache is a single DB-wide object shared by all CFs, so the D7
# unified cache budget is unaffected; only the extra memtable is new.
SK_CF = "sk_cf"
SK_CF_COMMENT = f" COMMENT 'cfname={SK_CF}'"


def derive_range_columns(queries: dict, filter_columns) -> set:
    """Filter columns some query compares with a non-equality operator.

    A BITLSM key part is kEquality by default on a binary column, which serves
    `=` and cannot express a range: the predicate is dropped and that column
    stops pruning. Declaring the key part ORDERED fixes it, but only the
    workload's own queries say which columns need it -- so the declaration is
    checked against them rather than trusted. A hand-kept list that drifts
    would not fail; it would quietly measure a worse BitLSM.

    Numeric columns are kRange already, so a range on one needs no declaration;
    the caller filters by type.
    """
    import re
    found = set()
    for sql in queries.values():
        m = re.search(r"\bWHERE\b(.*?)(\bGROUP BY\b|\bORDER BY\b|;|$)",
                      " ".join(sql.split()), re.I | re.S)
        if not m:
            continue
        where = m.group(1)
        for c in filter_columns:
            if re.search(rf"\b{c}\s+BETWEEN\b", where, re.I) or \
               re.search(rf"\b{c}\s*(<=|>=|<|>)(?!=)", where, re.I):
                found.add(c)
    return found


def check_range_columns(declared: set, queries: dict, filter_columns,
                        binary_columns, where: str) -> None:
    """Fail loudly when the declared ORDERED set and the queries disagree.

    Both directions matter. A binary column that takes a range but is not
    declared silently loses its pruning; a column declared without needing it
    silently changes that column's bin layout for nothing, which moves numbers
    with no reason a reader could find."""
    needed = {c for c in derive_range_columns(queries, filter_columns)
              if c in binary_columns}
    if declared != needed:
        raise ValueError(
            f"{where}: BITLSM ORDERED declaration is out of date with the "
            f"queries. declared={sorted(declared)} implied={sorted(needed)}. "
            f"Add or remove the column, or change the query that moved.")


class Workload:
    name = None  # e.g. "job"

    def verify_data(self):
        """Verify data files against the pinned MANIFEST. Hard-fail on
        mismatch — never silently re-download or load unverified data."""
        raise NotImplementedError

    def tables(self) -> list:
        """Table names in load order."""
        raise NotImplementedError

    def create_table_sql(self, table: str, engine: str,
                         index_layout: str) -> str:
        """CREATE TABLE with engine clause and ALL indexes of the layout
        declared inline (BITLSM_INDEX only supports CREATE TABLE syntax, and
        one symmetric code path keeps identities comparable)."""
        raise NotImplementedError

    def load_sql(self, table: str, csv_path: str) -> str:
        """LOAD DATA statement for one table."""
        raise NotImplementedError

    def bi_tables(self, index_layout: str) -> set:
        """Tables carrying a BITLSM_INDEX under this layout (these skip
        rocksdb_bulk_load so bitmap build goes through the memtable path)."""
        return set()

    def secondary_indexes(self, index_layout: str) -> dict:
        """{table: [secondary index names]} under this layout, PK excluded.
        Drives plan-hint resolution (fullscan ignores all of them,
        index_merge needs >=2 mergeable ones); {} = PK-only layout, where
        only unhinted plans are valid."""
        return {}

    def data_file(self, table: str) -> str:
        raise NotImplementedError

    def insert_columns(self) -> list:
        """Column names for the row-by-row INSERT of the write axis
        (ingest_run.py). Order must match parse_row()'s tuple."""
        raise NotImplementedError

    def parse_row(self, line: bytes) -> tuple:
        """One line of data_file() (BYTES, read binary) -> a value tuple for
        insert_columns(). Values stay bytes so the client sends them without
        charset conversion, matching load_sql's byte-for-byte passthrough.
        Owns the source dialect (delimiter, NULL token)."""
        raise NotImplementedError

    def total_rows(self) -> int:
        """Row count of the data source (for `rows: "all"`)."""
        raise NotImplementedError

    def histogram_columns(self) -> list:
        """Columns for ANALYZE .. UPDATE HISTOGRAM (engine_params
        {"hist": ...} identities). [] = workload defines no histogram set."""
        return []

    def composite_index_for(self, query_id: str) -> str:
        """The composite_v1 index this query template was designed around,
        or None when the query has no sargable predicate to index.

        Drives plan=force_composite. Without it the composite cell measures
        whatever the optimizer's stats happen to pick, which is not the
        per-template upper bound the layout exists to represent."""
        return None

    def queries(self) -> dict:
        """{stable query_id: SQL text}"""
        raise NotImplementedError

    def identity(self, engine: str, index_layout: str,
                 engine_params: dict = None) -> str:
        """Deterministic DB identity -> datadir name (harness plan D2).
        engine_params (e.g. future BitLSM rho) fork the identity."""
        parts = [self.name, index_layout, engine]
        for k in sorted(engine_params or {}):
            parts.append(f"{k}{engine_params[k]}")
        return "-".join(parts)
