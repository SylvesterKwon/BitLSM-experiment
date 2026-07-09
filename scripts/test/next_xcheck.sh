#!/usr/bin/env bash
# Correctness cross-check: next_honk (NEXT 1D secondary index + post-filter)
# vs honk_player --binding no-index (full-table-scan ground truth).
#
# Runs both drivers on the SAME test workload (which interleaves w/r ops, so
# each driver writes then reads within a single process/DB), extracts
# records_matched per query_id from both *_read_log.csv files, and asserts
# they are equal for EVERY query. no-index does a plain full-table scan, so
# it is unconditionally correct -- if next_honk's post-filtered secondary-
# index scan disagrees with it on any query, that's a real bug (see
# task-5-brief.md's "correctness gate" section: do NOT rig this to pass).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
next_honk_bin="${repo_root}/build/bin/next_honk"
honk_player_bin="${repo_root}/build/bin/honk_player"
workload="${repo_root}/src/honk_player/test/test_workload.tsv"
indexed_attr="trip_distance"  # CONTINUOUS column; Phase 1 indexes one such attr

for bin in "${next_honk_bin}" "${honk_player_bin}"; do
  [[ -x "${bin}" ]] || { echo "FAIL: missing binary: ${bin}"; exit 1; }
done
[[ -f "${workload}" ]] || { echo "FAIL: missing workload: ${workload}"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

next_db="${tmp}/next_db"
next_out="${tmp}/next_out"
noindex_db="${tmp}/noindex_db"
noindex_out="${tmp}/noindex_out"

echo "[xcheck] running next_honk (indexed_attr=${indexed_attr}) ..."
"${next_honk_bin}" --workload "${workload}" --db_path "${next_db}" \
    --indexed_attr "${indexed_attr}" --output_dir "${next_out}" \
    >"${tmp}/next_honk.log" 2>&1
tail -n 6 "${tmp}/next_honk.log"

echo "[xcheck] running honk_player --binding no-index (ground truth) ..."
"${honk_player_bin}" --binding no-index --workload "${workload}" \
    --db_path "${noindex_db}" --output_dir "${noindex_out}" \
    >"${tmp}/honk_player.log" 2>&1
tail -n 6 "${tmp}/honk_player.log"

next_csv="$(ls "${next_out}"/*_read_log.csv 2>/dev/null || true)"
noindex_csv="$(ls "${noindex_out}"/*_read_log.csv 2>/dev/null || true)"
[[ -n "${next_csv}" ]] || { echo "FAIL: no next_honk read CSV produced (see ${tmp}/next_honk.log)"; exit 1; }
[[ -n "${noindex_csv}" ]] || { echo "FAIL: no no-index read CSV produced (see ${tmp}/honk_player.log)"; exit 1; }

echo "[xcheck] next_honk read CSV : ${next_csv}"
echo "[xcheck] no-index read CSV  : ${noindex_csv}"
echo

python3 - "${next_csv}" "${noindex_csv}" "${indexed_attr}" <<'PYEOF'
import csv
import sys

next_path, noindex_path, indexed_attr = sys.argv[1], sys.argv[2], sys.argv[3]

def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))

next_rows = load(next_path)
noindex_rows = load(noindex_path)

if len(next_rows) != len(noindex_rows):
    print(f"FAIL: query count mismatch: next_honk={len(next_rows)} "
          f"no-index={len(noindex_rows)}")
    sys.exit(1)

print(f"{'query':<8}{'filter_attrs':<32}{'next_honk':>10}{'no-index':>10}  result")
print("-" * 70)

all_ok = True
index_exercised = False
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

    if indexed_attr in n["filter_attrs"].split(","):
        index_exercised = True

    status = "OK" if ok else "MISMATCH"
    print(f"Q{qid_n:<7}{n['filter_attrs']:<32}{matched_n:>10}{matched_i:>10}  {status}")

print()
print(f"indexed attr '{indexed_attr}' appears in >=1 query filter: "
      f"{'yes' if index_exercised else 'no'}")

if all_ok:
    print("\nPASS: next_honk matches no-index ground truth on every query")
    sys.exit(0)
else:
    print("\nFAIL: next_honk diverges from no-index ground truth (see MISMATCH rows above)")
    sys.exit(1)
PYEOF
