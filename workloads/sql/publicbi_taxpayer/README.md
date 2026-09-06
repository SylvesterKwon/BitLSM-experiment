# Public BI "Taxpayer" — vendored query set + frozen data pin

22 queries from the **Public BI benchmark** (cwida/public_bi_benchmark, MIT;
Ghita/Manegold/Boncz, CWI — the open re-implementation of Vogelsgesang et
al., *"Get Real: How Benchmarks Fail to Represent the Real World"*, DBTest
'18). Real Tableau-log SQL over real data. Despite the workbook name, the
data is the **CMS Medicare Physician & Other Supplier PUF**.

Results must be labeled "Public BI Taxpayer (instance 2, retargeted)" —
see the instance substitution note below.

## Data (frozen procurement)

- 10 instances `Taxpayer_1..10`, byte-identical 28-column schema,
  9,153,273 rows / ~1.83 GB uncompressed each (instances 1 and 2 verified).
- Mirror: **Zenodo, Public BI part 2, DOI 10.5281/zenodo.6344717** —
  holds instances 1..9 only.
- Acquisition: `scripts/prepare_taxpayer.py` downloads (md5-verified
  against the Zenodo record), prepares, and pins `MANIFEST.json`
  (sha256 of source .gz + prepared file). The harness' `verify_data()`
  only ever verifies against the committed pin.

Source dialect (verified against the benchmark's MonetDB loader,
`copy ... delimiters '|','\n','' null as 'null'`): pipe-delimited, **no
quote character** (quotes are literal data), NULL = literal `null`.
Prep (see `src/myrocks/workloads/pbi_taxpayer.py`) prepends a surrogate
`rid` (1-based line number; the workbook has no natural key and BITLSM
attribute columns cannot be PK columns), rewrites `null` -> `\N`, and
escapes literal backslashes. Inherited ambiguity: a genuine string value
`null` is indistinguishable from NULL (same in the upstream loaders).

## Instance substitution (documented deviation)

The original queries span instances (12/22 reference `Taxpayer_10`, which
is **missing from the Zenodo mirror**). All instances are schema-identical
near-copies, so all 22 queries are retargeted to one canonical table
`taxpayer`, loaded from **Taxpayer_2** (chosen because per-template
selectivities and exact match counts were independently measured on it,
2026-07-28 — see the table below).

## Rewrite rules (Vertica/Tableau -> MySQL 8.0)

`original/` holds the verbatim upstream queries; `rewrite.py` generates
`queries/` deterministically (regenerate with
`python3 workloads/sql/publicbi_taxpayer/rewrite.py`):

- **R1** `"Taxpayer_N"` refs -> bare `taxpayer`; column qualifiers
  stripped (bare FROM keeps harness hint injection working).
- **R2** double-quoted identifiers: known columns -> bare; Tableau aliases
  (`avg:...:ok`, `TEMP(...)`) -> backticks.
- **R3** `CAST(x AS double)` -> `x` (operands already DOUBLE; no-op cast).
- **R4** `CAST(x AS BIGINT)` -> `CAST(x AS SIGNED)`.

Everything else — constants, predicate structure, GROUP BY shapes
(including q20's duplicate GROUP BY term, legal in MySQL) — is verbatim.
**Scope note:** queries are the original 22 only; constant expansion
(selectivity sweeps from the NDV ladder) is deferred unless results show
too few informative cells (user decision 2026-07-28).

## Schema notes

Filter-column union across the 22 queries (sk_v1 SKs / bi_v1
BITLSM_INDEX target): `hcpcs_description, nppes_provider_city,
nppes_provider_first_name, nppes_provider_last_org_name,
nppes_provider_state` — all VARBINARY on every engine (BITLSM_INDEX
requires binary strings; identical schema keeps engines comparable).
Server stays latin1; UTF-8 source bytes pass through byte-transparently
(latin1 char == byte == Vertica octet width). `"Number of Records"`
(Tableau's synthetic always-1 column, never query-referenced) is renamed
`number_of_records`.

## Measured template selectivities (Taxpayer_2, 9,153,273 rows, 2026-07-28)

| predicate | rows | selectivity |
|---|---|---|
| state='WA' | 178,278 | 1.95e-2 |
| first_name='JOHN' | 245,222 | 2.68e-2 |
| hcpcs='Initial hospital care' | 281,570 | 3.08e-2 |
| city IN (BELLEVUE,BELLVUE) | 7,616 | 8.3e-4 |
| last_org_name='HOLDER' | 758 | 8.3e-5 |
| JOHN ∧ WA (q08/q13) | 4,741 | 5.2e-4 |
| hcpcs ∧ WA (q03/q12/q20) | 3,857 | 4.2e-4 |
| HOLDER ∧ WA (q01/q06) | 98 | 1.1e-5 |
| JOHN ∧ HOLDER (q05/q09/q11) | 77 | 8.4e-6 |
| JOHN ∧ HOLDER ∧ WA (q04/q07/q10/q14/q18/q19) | 29 | 3.2e-6 |
| city ∧ WA ∧ hcpcs (q15/q16/q21/q22) | 107 | 1.2e-5 |
| all four (q17) | 1 | 1.1e-7 |

These double as external validation targets for the H2-style COUNT gate:
individually-unselective predicates (2–3%) whose conjunctions collapse to
1e-5…1e-7 — the index-intersection regime, on real skewed data with NULLs.
NDV ladder: state 62, hcpcs_description 4,950, city 12,829, first_name
54,937, last_org_name 228,976, npi 880,645.
