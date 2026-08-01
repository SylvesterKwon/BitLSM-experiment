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

    def parse_row(self, line: str) -> tuple:
        """One line of data_file() -> a value tuple for insert_columns().
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
