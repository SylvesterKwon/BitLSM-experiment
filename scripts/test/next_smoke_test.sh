#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bin="${repo_root}/build/bin/next_smoke"
tmp="$(mktemp -d)"
data="${tmp}/buildings_tiny"
db="${tmp}/db"

# Buildings format expected by the example: id perimeter low0 low1 high0 high1
{
  echo -e "0\t1.5\t0.0\t0.0\t1.0\t1.0"
  echo -e "1\t2.5\t1.0\t1.0\t2.0\t2.0"
  echo -e "2\t3.5\t2.0\t2.0\t3.0\t3.0"
} > "${data}"

echo "[smoke] running next_smoke on 3 rows..."
out="$(timeout 90 "${bin}" "${db}" 3 "${data}" 2>&1 || true)"
echo "${out}"

echo "${out}" | grep -q "end writing data" || { echo "FAIL: no 'end writing data'"; exit 1; }
[[ -d "${db}" ]] || { echo "FAIL: DB dir not created"; exit 1; }
ls "${db}"/*.sst >/dev/null 2>&1 || [[ -f "${db}/CURRENT" ]] || { echo "FAIL: no DB files"; exit 1; }

echo "PASS: next_smoke wrote a DB in isolation"
rm -rf "${tmp}"
