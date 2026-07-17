# SSB denormalized (lineorder_flat) — vendored query set

13 queries (q1_1 … q4_3) rewritten from the original SSB star queries
(O'Neil et al.) against the single denormalized `lineorder_flat` table,
following ClickHouse's lineorder_flat methodology
(https://clickhouse.com/docs/getting-started/example-datasets/star-schema).
NOT the standard star form — results must be labeled "SSB-flat".

Rewrite rules (semantics-preserving; kept auditable here):
1. Dimension joins eliminated — dim attributes are materialized columns
   (C_*, S_*, P_*, D_YEAR, D_YEARMONTHNUM, D_YEARMONTH, D_WEEKNUMINYEAR),
   so `LO_ORDERDATE = D_DATEKEY AND D_YEAR = 1993` becomes `D_YEAR = 1993`.
2. q3_4: `D_YEARMONTH = 'Dec1997'` → `D_YEARMONTHNUM = 199712` (identical
   predicate; matches ClickHouse's `toYYYYMM() = 199712` form and keeps the
   filter on an integer column).
3. Everything else (aggregates, GROUP BY/ORDER BY, constants) verbatim from
   the SSB spec.

Data: generated, never committed — third_party/ssb-dbgen (pinned submodule)
emits 5 normalized .tbl files; `src/myrocks/workloads/ssb_flat.py` streams
them into `data/sql/ssb/sf<SF>/lineorder_flat.v<N>.tbl` (see FLATTEN_VERSION)
with a .meta.json (dbgen commit, SF, rows, sha256) as the integrity pin.
Canonical SF1 counts: lineorder 6,001,215 / customer 30,000 / part 200,000 /
supplier 2,000 / date 2,557 (verified 2026-07-17).

Schema notes: LO_ORDERDATE stays an integer yyyymmdd datekey. Filter-target
dim strings are VARBINARY on all three engines (BITLSM_INDEX requires binary
strings; identical schema keeps engines comparable). Filter-column union
(sk_v1 SKs / bi_v1 BITLSM_INDEX): D_YEAR, D_YEARMONTHNUM, D_WEEKNUMINYEAR,
LO_DISCOUNT, LO_QUANTITY, P_MFGR, P_CATEGORY, P_BRAND, S_REGION, S_NATION,
S_CITY, C_REGION, C_NATION, C_CITY.
