#!/usr/bin/env bash
# IM/PF broadened parity gate + I/O-advantage proof (Phase 4 Task 4.2).
#
# Task 4.1 (next_multiattr_xcheck.sh im) proved IM correctness cross-process
# on test_workload.tsv, but that workload exercises only ONE genuine
# multi-indexed-attribute conjunction (Q8: VendorID AND trip_distance) and
# never checks that IM actually reads FEWER SST data blocks than PF (the
# entire point of building index-merge in the first place). This gate:
#
#   1. Broadens correctness coverage on top of test_workload.tsv
#      (test_workload_im.tsv = the same 100 writes + all 10 original read
#      queries + 2 NEW ones):
#        Q10: an N=3 conjunction predicating ALL THREE indexed attrs at once
#             (trip_distance range AND VendorID=1 AND payment_type=2).
#        Q11: an empty-candidate query (a trip_distance range far outside
#             the data, [1000,2000) against data in [0.2,29.91]) -- the
#             per-attribute R-tree search returns ZERO candidate blocks,
#             proving the intersection code path handles an empty set
#             without crashing.
#      Asserts, per query: PF == IM == no-index (ground truth) on ALL 12
#      queries, both strategies against the SAME reopened (cross-process)
#      DB.
#
#   2. Proves the I/O advantage: reads are run with --report_blocks (a
#      driver-side-only RocksDB PerfContext counter -- no engine change),
#      which prints "BLOCKS query_id=<id> strategy=<pf|im>
#      blocks_touched=<n>" per query to stderr. blocks_touched is the
#      number of DATA blocks (cache hit + miss combined) THAT query's
#      iterator visited. Asserts IM <= PF on EVERY query, and STRICTLY
#      IM < PF on the genuinely multi-indexed-attribute conjunctive queries
#      (Q8, Q10) -- if IM does NOT reduce block reads there, that is a real
#      bug (the intersection isn't taking effect), not something to hide.
#
#      The write+read passes use --block_size 1152 (opt-in, next_honk-only
#      override; unrelated to any other binding's config, and unrelated to
#      real experiment runs which never pass it): with the default 4KB
#      block and ~100 small taxi records (~11 records/block), records land
#      in SST data blocks by random uuid-key order, so almost EVERY
#      block's per-attribute min/max already spans nearly the WHOLE
#      domain -- neither PF nor IM can be selective on such a coarse
#      layout, regardless of correctness. A smaller block_size makes each
#      block hold fewer records, so every indexed predicate's R-tree
#      search can actually exclude blocks that provably do not match -- the
#      layout needed to observe the effect this gate exists to prove.
#      1152 was picked empirically: below ~1024 bytes on this exact
#      workload, PF ITSELF (unrelated to IM -- reproduces identically
#      without --read_strategy im) silently drops one record on Q3 vs
#      no-index ground truth -- a pre-existing NEXT engine edge case at
#      very small block sizes, unrelated to Task 4.2's index-merge scope
#      and NOT fixed here (see the Task 4.2 report). 1152 gives a safe
#      margin above that threshold while still forcing ~4x more data
#      blocks than the 4KB default, enough for the R-tree to be genuinely
#      selective.
#
# Flow (mirrors next_multiattr_xcheck.sh's write/read process split):
#   (a) next_honk --mode write                      (process 1)
#   (b) next_honk --mode read --read_strategy pf    (process 2, reopen #1)
#   (c) next_honk --mode read --read_strategy im    (process 3, reopen #2 --
#       SAME db_path as (b); requires DB::OpenForReadOnly for --mode read,
#       see next_honk.cpp -- a plain rocksdb::DB::Open reopen rolls a fresh
#       manifest that does not round-trip this fork's custom per-file
#       secondary-index fields, silently emptying the global R-tree on any
#       SUBSEQUENT reopen)
#   (d) honk_player --binding no-index               (ground truth)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
next_honk_bin="${repo_root}/build/bin/next_honk"
honk_player_bin="${repo_root}/build/bin/honk_player"
workload="${repo_root}/src/honk_player/test/test_workload_im.tsv"
indexed_attrs="trip_distance,VendorID,payment_type"
categorical_attrs="VendorID payment_type"
block_size=1152
# Query ids (0-indexed, matching read_csv's query_id column) that predicate
# >=2 co-resident INDEXED attrs at once -- IM must show a STRICT reduction
# in blocks_touched here, not just <=.
strict_reduction_qids=(8 10)

for bin in "${next_honk_bin}" "${honk_player_bin}"; do
  [[ -x "${bin}" ]] || { echo "FAIL: missing binary: ${bin}"; exit 1; }
done
[[ -f "${workload}" ]] || { echo "FAIL: missing workload: ${workload}"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

next_db="${tmp}/next_db"
next_write_out="${tmp}/next_write_out"
next_pf_out="${tmp}/next_pf_out"
next_im_out="${tmp}/next_im_out"
noindex_db="${tmp}/noindex_db"
noindex_out="${tmp}/noindex_out"

echo "[im-xcheck] (a) next_honk --mode write --block_size ${block_size} (process 1) ..."
"${next_honk_bin}" --mode write --workload "${workload}" --db_path "${next_db}" \
    --indexed_attrs "${indexed_attrs}" --output_dir "${next_write_out}" \
    --block_size "${block_size}" \
    >"${tmp}/next_honk_write.log" 2>&1
tail -n 6 "${tmp}/next_honk_write.log"

for attr in ${categorical_attrs}; do
  dict_file="${next_db}/dict_${attr}.txt"
  [[ -s "${dict_file}" ]] || {
    echo "FAIL: expected non-empty dict file for '${attr}' not found: ${dict_file}"
    exit 1
  }
done

echo "[im-xcheck] (b) next_honk --mode read --read_strategy pf --report_blocks (process 2, reopen #1) ..."
"${next_honk_bin}" --mode read --workload "${workload}" --db_path "${next_db}" \
    --indexed_attrs "${indexed_attrs}" --output_dir "${next_pf_out}" \
    --read_strategy pf --report_blocks --block_size "${block_size}" \
    >"${tmp}/next_honk_pf.log" 2>"${tmp}/next_honk_pf.err"
tail -n 6 "${tmp}/next_honk_pf.log"

echo "[im-xcheck] (c) next_honk --mode read --read_strategy im --report_blocks (process 3, reopen #2, SAME db_path) ..."
"${next_honk_bin}" --mode read --workload "${workload}" --db_path "${next_db}" \
    --indexed_attrs "${indexed_attrs}" --output_dir "${next_im_out}" \
    --read_strategy im --report_blocks --block_size "${block_size}" \
    >"${tmp}/next_honk_im.log" 2>"${tmp}/next_honk_im.err"
tail -n 6 "${tmp}/next_honk_im.log"

for f in "${tmp}/next_honk_pf.log" "${tmp}/next_honk_im.log"; do
  if grep -q "WARNING: categorical dict for indexed column" "${f}"; then
    echo "FAIL: 'empty categorical dict' warning fired on a happy-path reopen (see ${f})"
    exit 1
  fi
done

echo "[im-xcheck] (d) honk_player --binding no-index (ground truth) ..."
"${honk_player_bin}" --binding no-index --workload "${workload}" \
    --db_path "${noindex_db}" --output_dir "${noindex_out}" \
    >"${tmp}/honk_player.log" 2>&1
tail -n 6 "${tmp}/honk_player.log"

pf_csv="$(ls "${next_pf_out}"/*_read_log.csv 2>/dev/null || true)"
im_csv="$(ls "${next_im_out}"/*_read_log.csv 2>/dev/null || true)"
noindex_csv="$(ls "${noindex_out}"/*_read_log.csv 2>/dev/null || true)"
[[ -n "${pf_csv}" ]] || { echo "FAIL: no PF read CSV produced (see ${tmp}/next_honk_pf.log)"; exit 1; }
[[ -n "${im_csv}" ]] || { echo "FAIL: no IM read CSV produced (see ${tmp}/next_honk_im.log)"; exit 1; }
[[ -n "${noindex_csv}" ]] || { echo "FAIL: no no-index read CSV produced (see ${tmp}/honk_player.log)"; exit 1; }

echo "[im-xcheck] PF read CSV      : ${pf_csv}"
echo "[im-xcheck] IM read CSV      : ${im_csv}"
echo "[im-xcheck] no-index read CSV: ${noindex_csv}"
echo

python3 - "${pf_csv}" "${im_csv}" "${noindex_csv}" "${tmp}/next_honk_pf.err" \
    "${tmp}/next_honk_im.err" "${indexed_attrs}" "${strict_reduction_qids[@]}" <<'PYEOF'
import csv
import re
import sys

pf_path, im_path, noindex_path, pf_err_path, im_err_path, indexed_attrs_csv = sys.argv[1:7]
strict_reduction_qids = set(sys.argv[7:])
indexed_attrs = indexed_attrs_csv.split(",")


def load_csv(path):
    with open(path, newline="") as f:
        return {row["query_id"]: row for row in csv.DictReader(f)}


def load_blocks(path):
    """Parse 'BLOCKS query_id=<id> strategy=<pf|im> blocks_touched=<n>' lines."""
    blocks = {}
    pat = re.compile(r"BLOCKS query_id=(\d+) strategy=(\w+) blocks_touched=(\d+)")
    with open(path) as f:
        for line in f:
            m = pat.search(line)
            if m:
                blocks[m.group(1)] = int(m.group(3))
    return blocks


pf_rows = load_csv(pf_path)
im_rows = load_csv(im_path)
noindex_rows = load_csv(noindex_path)
pf_blocks = load_blocks(pf_err_path)
im_blocks = load_blocks(im_err_path)

qids = sorted(pf_rows.keys(), key=int)

if not (set(pf_rows) == set(im_rows) == set(noindex_rows)):
    print(f"FAIL: query_id set mismatch: pf={sorted(pf_rows)} im={sorted(im_rows)} "
          f"no-index={sorted(noindex_rows)}")
    sys.exit(1)

if len(qids) < 12:
    print(f"FAIL: expected >=12 queries (10 original + N>=3 conjunction + "
          f"empty-candidate), got {len(qids)} -- broadened workload not wired up?")
    sys.exit(1)

print("--- Part 1: broadened correctness parity (PF == IM == no-index) ---")
print(f"{'query':<8}{'filter_attrs':<45}{'pf':>8}{'im':>8}{'no-index':>10}  result")
print("-" * 90)

all_ok = True
any_nonzero_match = False
exercised = {a: False for a in indexed_attrs}
for qid in qids:
    pf_row, im_row, ni_row = pf_rows[qid], im_rows[qid], noindex_rows[qid]
    m_pf = int(pf_row["records_matched"])
    m_im = int(im_row["records_matched"])
    m_ni = int(ni_row["records_matched"])
    ok = (m_pf == m_im == m_ni)
    all_ok &= ok
    if m_ni > 0:
        any_nonzero_match = True
    filters = pf_row["filter_attrs"].split(",")
    for a in indexed_attrs:
        if a in filters:
            exercised[a] = True
    status = "OK" if ok else "MISMATCH"
    print(f"Q{qid:<7}{pf_row['filter_attrs']:<45}{m_pf:>8}{m_im:>8}{m_ni:>10}  {status}")

# Q11 (empty-candidate) must be the all-zero row -- confirm it actually IS
# zero (not accidentally selective), so its "PASS" isn't vacuous by itself.
empty_qid = qids[-1]
if not (int(pf_rows[empty_qid]["records_matched"]) == 0 and
        int(im_rows[empty_qid]["records_matched"]) == 0 and
        int(noindex_rows[empty_qid]["records_matched"]) == 0):
    print(f"FAIL: expected the empty-candidate query (Q{empty_qid}) to match 0 "
          f"records everywhere")
    sys.exit(1)

print()
for a in indexed_attrs:
    print(f"indexed attr '{a}' appears in >=1 query filter: "
          f"{'yes' if exercised[a] else 'no'}")

if not any_nonzero_match:
    print("FAIL: every query matched 0 records everywhere -- gate is vacuous.")
    sys.exit(1)
if not all(exercised.values()):
    print(f"FAIL: some indexed attr never exercised: {exercised}")
    sys.exit(1)
if not all_ok:
    print("\nFAIL: PF/IM/no-index diverge on >=1 query (see MISMATCH rows above).")
    sys.exit(1)

print("\nPASS: PF == IM == no-index on every query (10 original + N>=3 "
      "conjunction + empty-candidate).")

print()
print("--- Part 2: I/O advantage (blocks touched, PF vs IM) ---")
print(f"{'query':<8}{'filter_attrs':<45}{'pf blocks':>10}{'im blocks':>10}  result")
print("-" * 90)

io_ok = True
for qid in qids:
    bp = pf_blocks.get(qid)
    bi = im_blocks.get(qid)
    if bp is None or bi is None:
        print(f"FAIL: missing BLOCKS line for query_id={qid} (pf={bp} im={bi})")
        io_ok = False
        continue
    must_be_strict = qid in strict_reduction_qids
    if bi > bp:
        status = "FAIL (IM > PF)"
        io_ok = False
    elif must_be_strict and bi >= bp:
        status = "FAIL (expected IM < PF, strict)"
        io_ok = False
    elif must_be_strict:
        status = "OK (strict <)"
    else:
        status = "OK"
    print(f"Q{qid:<7}{pf_rows[qid]['filter_attrs']:<45}{bp:>10}{bi:>10}  {status}")

print()
if not io_ok:
    print("FAIL: IM did not demonstrate its I/O advantage over PF -- see FAIL "
          "rows above. This means the intersection is not taking effect on a "
          "genuinely conjunctive query; investigate db/version_set.cc's "
          "AddIteratorsForLevelIndexMerge, do not hide this.")
    sys.exit(1)

print("PASS: IM touches <= PF's blocks on every query, and STRICTLY fewer on "
      f"every genuinely multi-indexed-attribute conjunctive query "
      f"(Q{', Q'.join(sorted(strict_reduction_qids))}).")
sys.exit(0)
PYEOF
