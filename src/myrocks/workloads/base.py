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
