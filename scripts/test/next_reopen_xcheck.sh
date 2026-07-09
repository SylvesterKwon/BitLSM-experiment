#!/usr/bin/env bash
# Cross-process reopen correctness gate (Phase 3 Task 3.1).
#
# Unlike next_xcheck.sh (which writes+reads in a SINGLE next_honk process --
# the in-process regression that Task 2.4 could pass even with the reopen
# bug present), this gate exercises the actual bug: on DB reopen in a
# SEPARATE process, VersionSet::global_rtrees_ (the N in-RAM global
# R-trees) must be repopulated from the MANIFEST via
# VersionEditHandler::OnNonCfOperation's two-arg VersionBuilder::Apply, or
# every secondary-index read against the reopened DB silently returns
# nothing.
#
# Flow:
#   (a) next_honk --mode write  -- writes the workload, closes the DB.
#   (b) next_honk --mode read   -- a SEPARATE process; opens the EXISTING DB
#       (does not recreate it) and issues the read queries against it.
#   (c) honk_player --binding no-index -- ground truth, full-table-scan,
#       on the SAME workload (single process, as usual).
# Assert per-query_id records_matched: (b) == (c). Do NOT rig this to pass.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
next_honk_bin="${repo_root}/build/bin/next_honk"
honk_player_bin="${repo_root}/build/bin/honk_player"
workload="${repo_root}/src/honk_player/test/test_workload.tsv"
indexed_attr="trip_distance"  # CONTINUOUS column; single-attr reopen gate

for bin in "${next_honk_bin}" "${honk_player_bin}"; do
  [[ -x "${bin}" ]] || { echo "FAIL: missing binary: ${bin}"; exit 1; }
done
[[ -f "${workload}" ]] || { echo "FAIL: missing workload: ${workload}"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

next_db="${tmp}/next_db"
next_write_out="${tmp}/next_write_out"
next_read_out="${tmp}/next_read_out"
noindex_db="${tmp}/noindex_db"
noindex_out="${tmp}/noindex_out"

echo "[reopen-xcheck] (a) next_honk --mode write (process 1: writes + closes) ..."
"${next_honk_bin}" --mode write --workload "${workload}" --db_path "${next_db}" \
    --indexed_attr "${indexed_attr}" --output_dir "${next_write_out}" \
    >"${tmp}/next_honk_write.log" 2>&1
tail -n 6 "${tmp}/next_honk_write.log"

echo "[reopen-xcheck] (b) next_honk --mode read (process 2: SEPARATE invocation, reopens) ..."
"${next_honk_bin}" --mode read --workload "${workload}" --db_path "${next_db}" \
    --indexed_attr "${indexed_attr}" --output_dir "${next_read_out}" \
    >"${tmp}/next_honk_read.log" 2>&1
tail -n 6 "${tmp}/next_honk_read.log"

echo "[reopen-xcheck] (c) honk_player --binding no-index (ground truth) ..."
"${honk_player_bin}" --binding no-index --workload "${workload}" \
    --db_path "${noindex_db}" --output_dir "${noindex_out}" \
    >"${tmp}/honk_player.log" 2>&1
tail -n 6 "${tmp}/honk_player.log"

next_read_csv="$(ls "${next_read_out}"/*_read_log.csv 2>/dev/null || true)"
noindex_csv="$(ls "${noindex_out}"/*_read_log.csv 2>/dev/null || true)"
[[ -n "${next_read_csv}" ]] || { echo "FAIL: no next_honk --mode read read CSV produced (see ${tmp}/next_honk_read.log)"; exit 1; }
[[ -n "${noindex_csv}" ]] || { echo "FAIL: no no-index read CSV produced (see ${tmp}/honk_player.log)"; exit 1; }

echo "[reopen-xcheck] next_honk (reopened, process 2) read CSV : ${next_read_csv}"
echo "[reopen-xcheck] no-index read CSV                        : ${noindex_csv}"
echo

python3 - "${next_read_csv}" "${noindex_csv}" "${indexed_attr}" <<'PYEOF'
import csv
import sys

next_path, noindex_path, indexed_attr = sys.argv[1], sys.argv[2], sys.argv[3]

def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))

next_rows = load(next_path)
noindex_rows = load(noindex_path)

if len(next_rows) != len(noindex_rows):
    print(f"FAIL: query count mismatch: next_honk(reopened)={len(next_rows)} "
          f"no-index={len(noindex_rows)}")
    sys.exit(1)

print(f"{'query':<8}{'filter_attrs':<32}{'next_honk(reopen)':>18}{'no-index':>10}  result")
print("-" * 80)

all_ok = True
index_exercised = False
any_nonzero_match = False
for n, i in zip(next_rows, noindex_rows):
    qid_n, qid_i = n["query_id"], i["query_id"]
    if qid_n != qid_i:
        print(f"FAIL: query_id mismatch at row: next_honk={qid_n} no-index={qid_i}")
        all_ok = False
        continue

    matched_n = int(n["records_matched"])
    matched_i = int(i["records_matched"])
    ok = matched_n == matched_i
    all_ok &= ok
    # Base the vacuous-gate check on no-index (ground truth), not next_honk:
    # next_honk reading back 0 across the board while no-index is nonzero is
    # exactly the bug this gate targets (all_ok catches it as MISMATCH,
    # below) -- it must NOT be reclassified as "vacuous".
    if matched_i > 0:
        any_nonzero_match = True

    if indexed_attr in n["filter_attrs"].split(","):
        index_exercised = True

    status = "OK" if ok else "MISMATCH"
    print(f"Q{qid_n:<7}{n['filter_attrs']:<32}{matched_n:>18}{matched_i:>10}  {status}")

print()
print(f"indexed attr '{indexed_attr}' appears in >=1 query filter: "
      f"{'yes' if index_exercised else 'no'}")

# Guard against a vacuous pass: if the reopened process returned 0 matches
# on EVERY query while no-index (ground truth) also returned 0 on every
# query, that would mean the workload/queries are degenerate, not that
# reopen actually works. The bug this gate targets manifests as next_honk
# returning 0 across the board while no-index returns >0 -- catch that
# shape explicitly, not just via the per-row equality check.
if not any_nonzero_match:
    print("FAIL: every query matched 0 records in BOTH outputs -- gate is "
          "vacuous, cannot confirm reopen works. Check the workload/query set.")
    sys.exit(1)

if not index_exercised:
    print(f"FAIL: indexed attr '{indexed_attr}' never appears in any query's "
          "filter_attrs -- gate is vacuous, the secondary index under test "
          "is never exercised. Check the workload/query set.")
    sys.exit(1)

if all_ok:
    print("\nPASS: next_honk (reopened in a separate process) matches "
          "no-index ground truth on every query -- MANIFEST replay "
          "correctly repopulated the global R-tree.")
    sys.exit(0)
else:
    print("\nFAIL: next_honk (reopened) diverges from no-index ground truth "
          "(see MISMATCH rows above) -- reopen repopulation is broken.")
    sys.exit(1)
PYEOF
