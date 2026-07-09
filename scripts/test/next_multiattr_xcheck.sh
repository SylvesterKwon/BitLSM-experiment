#!/usr/bin/env bash
# Cross-process MULTI-ATTRIBUTE PF gate (Phase 3 Task 3.4).
#
# next_reopen_xcheck.sh proved cross-process reopen for a SINGLE, CONTINUOUS
# indexed attr. Task 3.3 added real multi-attribute post-filtering and
# categorical dictionary-encoding to next_honk, but only spot-checked the
# cross-process shape manually (not committed). This script is that
# committed gate: it exercises the real nyc_taxi_seq_write -> nyc_taxi_seq_read
# shape (separate write and read PROCESSES) with an --indexed_attrs list that
# covers >=1 CONTINUOUS and >=2 CATEGORICAL columns, so it proves, together:
#   - N (>1) co-resident 1D secondary indexes select correctly (sec_attr_index).
#   - Categorical columns round-trip through CategoricalDict::Save/Load
#     across the process boundary (the write process assigns ids; the
#     SEPARATE read process must decode/encode using the IDENTICAL mapping).
#   - Multi-attribute post-filtering (AND of clauses, OR within a clause)
#     is correct against reopened (not just in-process) state.
#
# Flow:
#   (a) next_honk --mode write  -- writes the workload, saves the dict files,
#       flushes, closes the DB.
#   (b) next_honk --mode read   -- a SEPARATE process; opens the EXISTING DB,
#       LOADS the persisted dict files, issues the read queries against it.
#   (c) honk_player --binding no-index -- ground truth, full-table-scan, on
#       the SAME workload (single process, as usual).
# Assert per-query_id records_matched: (b) == (c). Do NOT rig this to pass.
#
# NEXT (Phase 4, Task 4.1): optional first arg selects next_honk's
# --read_strategy (pf|im, default pf -- unchanged, byte-identical invocation
# for existing callers). `next_multiattr_xcheck.sh im` exercises the SAME
# write->read->no-index shape but through the IM (index merge) engine path
# instead of PF, proving the SST-side block-list intersection is correct
# across a real process boundary (the flushed-to-SST leg the in-process gate
# cannot exercise, since its tiny workload never flushes).
set -euo pipefail

read_strategy="${1:-pf}"
if [[ "${read_strategy}" != "pf" && "${read_strategy}" != "im" ]]; then
  echo "usage: $0 [pf|im]" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
next_honk_bin="${repo_root}/build/bin/next_honk"
honk_player_bin="${repo_root}/build/bin/honk_player"
workload="${repo_root}/src/honk_player/test/test_workload.tsv"
# trip_distance = CONTINUOUS; VendorID, payment_type = CATEGORICAL (both
# dict-encoded columns get exercised, matching Task 3.3's in-process gate).
indexed_attrs="trip_distance,VendorID,payment_type"
categorical_attrs="VendorID payment_type"

echo "[multiattr-xcheck] read_strategy=${read_strategy}"

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

echo "[multiattr-xcheck] (a) next_honk --mode write (process 1: writes + closes) ..."
"${next_honk_bin}" --mode write --workload "${workload}" --db_path "${next_db}" \
    --indexed_attrs "${indexed_attrs}" --output_dir "${next_write_out}" \
    >"${tmp}/next_honk_write.log" 2>&1
tail -n 6 "${tmp}/next_honk_write.log"

# The write process must have persisted a dict file per CATEGORICAL indexed
# column -- without this, the read process below cannot possibly decode
# query literals into the write process's id space, and the gate would not
# be testing what it claims to.
for attr in ${categorical_attrs}; do
  dict_file="${next_db}/dict_${attr}.txt"
  [[ -s "${dict_file}" ]] || {
    echo "FAIL: expected non-empty dict file for '${attr}' not found: ${dict_file}"
    echo "      (see ${tmp}/next_honk_write.log)"
    exit 1
  }
  echo "[multiattr-xcheck] dict file OK: ${dict_file} ($(wc -l < "${dict_file}") entries)"
done

echo "[multiattr-xcheck] (b) next_honk --mode read --read_strategy ${read_strategy} (process 2: SEPARATE invocation, reopens + loads dicts) ..."
"${next_honk_bin}" --mode read --workload "${workload}" --db_path "${next_db}" \
    --indexed_attrs "${indexed_attrs}" --output_dir "${next_read_out}" \
    --read_strategy "${read_strategy}" \
    >"${tmp}/next_honk_read.log" 2>&1
tail -n 6 "${tmp}/next_honk_read.log"

# 3.3-m3 sanity: the read process just loaded real, non-empty dict files
# above, so next_honk's "empty categorical dict" diagnostic (added for
# Task 3.4) must NOT fire on this happy path -- if it does, either the dict
# didn't actually load, or the diagnostic is misfiring.
if grep -q "WARNING: categorical dict for indexed column" "${tmp}/next_honk_read.log"; then
  echo "FAIL: 'empty categorical dict' warning fired on a happy-path reopen (see ${tmp}/next_honk_read.log)"
  exit 1
fi

echo "[multiattr-xcheck] (c) honk_player --binding no-index (ground truth) ..."
"${honk_player_bin}" --binding no-index --workload "${workload}" \
    --db_path "${noindex_db}" --output_dir "${noindex_out}" \
    >"${tmp}/honk_player.log" 2>&1
tail -n 6 "${tmp}/honk_player.log"

next_read_csv="$(ls "${next_read_out}"/*_read_log.csv 2>/dev/null || true)"
noindex_csv="$(ls "${noindex_out}"/*_read_log.csv 2>/dev/null || true)"
[[ -n "${next_read_csv}" ]] || { echo "FAIL: no next_honk --mode read read CSV produced (see ${tmp}/next_honk_read.log)"; exit 1; }
[[ -n "${noindex_csv}" ]] || { echo "FAIL: no no-index read CSV produced (see ${tmp}/honk_player.log)"; exit 1; }

echo "[multiattr-xcheck] next_honk (reopened, process 2) read CSV : ${next_read_csv}"
echo "[multiattr-xcheck] no-index read CSV                        : ${noindex_csv}"
echo

python3 - "${next_read_csv}" "${noindex_csv}" "${indexed_attrs}" <<'PYEOF'
import csv
import sys

next_path, noindex_path, indexed_attrs_csv = sys.argv[1], sys.argv[2], sys.argv[3]
indexed_attrs = indexed_attrs_csv.split(",")

def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))

next_rows = load(next_path)
noindex_rows = load(noindex_path)

if len(next_rows) != len(noindex_rows):
    print(f"FAIL: query count mismatch: next_honk(reopened)={len(next_rows)} "
          f"no-index={len(noindex_rows)}")
    sys.exit(1)

print(f"{'query':<8}{'filter_attrs':<40}{'next_honk(reopen)':>18}{'no-index':>10}  result")
print("-" * 90)

all_ok = True
exercised = {a: False for a in indexed_attrs}
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
    # exactly the bug this gate targets -- it must NOT be reclassified as
    # "vacuous" via all_ok, below.
    if matched_i > 0:
        any_nonzero_match = True

    filters = n["filter_attrs"].split(",")
    for a in indexed_attrs:
        if a in filters:
            exercised[a] = True

    status = "OK" if ok else "MISMATCH"
    print(f"Q{qid_n:<7}{n['filter_attrs']:<40}{matched_n:>18}{matched_i:>10}  {status}")

print()
for a in indexed_attrs:
    print(f"indexed attr '{a}' appears in >=1 query filter: "
          f"{'yes' if exercised[a] else 'no'}")

# Guard against a vacuous pass (mirrors next_reopen_xcheck.sh): every query
# matching 0 in BOTH outputs would mean the workload/query set is degenerate,
# not that reopen + multi-attr PF actually work.
if not any_nonzero_match:
    print("FAIL: every query matched 0 records in BOTH outputs -- gate is "
          "vacuous, cannot confirm reopen + multi-attr PF work. Check the "
          "workload/query set.")
    sys.exit(1)

if not all(exercised.values()):
    unexercised = [a for a, ok in exercised.items() if not ok]
    print(f"FAIL: indexed attr(s) {unexercised} never appear in any query's "
          "filter_attrs -- gate is vacuous for that attr, it is never "
          "exercised (as a driving OR a post-filtered predicate). Check the "
          "workload/query set.")
    sys.exit(1)

if all_ok:
    print("\nPASS: next_honk (reopened in a separate process) matches "
          "no-index ground truth on every multi-attribute query -- "
          "MANIFEST replay + categorical dict persistence + multi-attr "
          "post-filtering all work across a real process boundary.")
    sys.exit(0)
else:
    print("\nFAIL: next_honk (reopened) diverges from no-index ground truth "
          "(see MISMATCH rows above) -- cross-process multi-attribute PF is "
          "broken.")
    sys.exit(1)
PYEOF
