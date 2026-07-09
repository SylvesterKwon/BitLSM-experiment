#pragma once
// NEXT Phase 5 (Task 5.2) -- data-driven per-attribute gap threshold delta_i.
//
// Task 5.1 made the engine split each per-file secondary index BY GAP: while
// walking the value-sorted data-block entries it starts a new global-index
// entry whenever `cur - prev > delta_i` (strict `>`, index_builder.cc:995).
// An empty `BlockBasedTableOptions::sec_index_deltas` makes the engine fall
// back to 0.005 for every index. This header derives those delta_i values
// from the DATA distribution (never from the query workload), controlled by a
// single knob `m`.
//
// Target: for attribute i with C_i distinct values, aim for
//   E_i = clamp( lround( N / (m * B) ), 1, C_i )
// global-index entries. Cutting at the E_i - 1 LARGEST distinct-value gaps
// yields E_i buckets, so we position delta_i strictly between two adjacent
// DISTINCT gap magnitudes and rely on the engine's strict `>` to fire exactly
// the intended number of cuts.
//
//   * E_i <= 1        -> delta_i = 2 * max_gap  (no gap exceeds it -> 1 entry).
//   * E_i >= C_i      -> delta_i = 0.0          (every positive gap > 0 cuts ->
//                                                C_i entries; the low-cardinality
//                                                cap: passenger_count, VendorID,
//                                                payment_type, and every dense
//                                                categorical land here).
//   * else            -> pick the achievable cut count (a cumulative count over
//                        DISTINCT gap magnitudes) closest to k = E_i - 1, then
//                        set delta_i midway between that magnitude and the next
//                        smaller distinct magnitude (or half of it, for the
//                        smallest). Ties in distance prefer the FINER side
//                        (more cuts). A FULLY-TIED distribution (all gaps equal,
//                        e.g. a dense integer categorical) has a single distinct
//                        magnitude, so the only interior boundary sits just
//                        below it => C_i entries (the cap) -- documented in the
//                        Task 5.2 report.
//
// `B` (values per data block) is not stored; the driver estimates it as
//   B = block_size / avg_serialized_value_bytes
// (avg_serialized_value_bytes ~= 8*a + avg_payload_bytes). B only sets the
// target scale -- `m` is the real knob.
//
// ISOLATION: header-only, no RocksDB / nlohmann dependency (matches the other
// next_driver helpers: next_dict.h, next_query.h). Pure <algorithm>/<vector>.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace nextd {

// Per-attribute derivation result. `deltas` is what the engine consumes
// (`bbto.sec_index_deltas`); `cardinalities` (C_i) and `targets` (E_i) are
// carried back purely so the driver can log the target vs. realized
// granularity for the crossover analysis (Task 5.2 Step 4).
inline std::vector<double> ComputeSecIndexDeltas(
    const std::vector<std::vector<double>>& per_attr_values, uint64_t N,
    double m, double B, std::vector<uint64_t>* out_cardinalities = nullptr,
    std::vector<uint64_t>* out_targets = nullptr) {
  std::vector<double> deltas;
  deltas.reserve(per_attr_values.size());
  if (out_cardinalities) out_cardinalities->clear();
  if (out_targets) out_targets->clear();

  const double denom = m * B;

  for (const auto& raw_values : per_attr_values) {
    // 1) sorted distinct value list -> C_i and the C_i - 1 positive gaps.
    std::vector<double> vals(raw_values);
    std::sort(vals.begin(), vals.end());
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    const uint64_t C = vals.size();

    // 2) E_i = clamp( lround(N / (m*B)), 1, C_i ). A non-positive denominator
    //    (degenerate B/m) is treated as "target exceeds cardinality" -> cap.
    uint64_t E = 0;
    if (C > 0) {
      double raw = denom > 0.0 ? static_cast<double>(N) / denom
                               : static_cast<double>(C);
      long e = std::lround(raw);
      if (e < 1) e = 1;
      if (static_cast<uint64_t>(e) > C) e = static_cast<long>(C);
      E = static_cast<uint64_t>(e);
    }

    if (out_cardinalities) out_cardinalities->push_back(C);
    if (out_targets) out_targets->push_back(E);

    // Degenerate: no values (empty attribute) or a single distinct value ->
    // there is nothing to cut. delta_i = 0 is harmless (no gap exists).
    if (C <= 1) {
      deltas.push_back(0.0);
      continue;
    }

    // Consecutive gaps of the sorted distinct list (all strictly > 0).
    std::vector<double> gaps;
    gaps.reserve(C - 1);
    for (size_t j = 1; j < vals.size(); ++j) gaps.push_back(vals[j] - vals[j - 1]);

    // Cap: target >= cardinality -> cut every positive gap (delta_i = 0, strict
    // `>` fires on every gap > 0) -> C_i entries. This is the low-cardinality
    // cap AND the "every categorical" case.
    if (E >= C) {
      deltas.push_back(0.0);
      continue;
    }

    // No cut wanted (1 entry): delta_i strictly above the largest gap.
    if (E <= 1) {
      double max_gap = *std::max_element(gaps.begin(), gaps.end());
      deltas.push_back(2.0 * max_gap);
      continue;
    }

    // 3) 1 < E_i < C_i: choose delta_i between adjacent DISTINCT gap magnitudes.
    // Sort gaps descending, collapse to distinct magnitudes with a cumulative
    // count cum[j] = #{ gaps >= mag[j] } (strictly increasing in j). cum[j] is
    // exactly the number of cuts a threshold placed just below mag[j] fires.
    std::sort(gaps.begin(), gaps.end(), std::greater<double>());
    std::vector<double> mag;
    std::vector<uint64_t> cum;
    for (size_t idx = 0; idx < gaps.size(); ++idx) {
      if (mag.empty() || gaps[idx] != mag.back()) {
        mag.push_back(gaps[idx]);
        cum.push_back(idx + 1);
      } else {
        cum.back() = idx + 1;
      }
    }

    // Desired cut count k = E_i - 1 (that many cuts -> E_i buckets). Pick the
    // achievable cum[j] closest to k; on a distance tie prefer the larger
    // cum[j] (the finer side, more cuts) -- this is what collapses a
    // fully-tied distribution (single magnitude, only cum = C_i - 1 available)
    // to the C_i cap.
    const long k = static_cast<long>(E) - 1;
    size_t best = 0;
    for (size_t j = 1; j < mag.size(); ++j) {
      long dj = std::labs(static_cast<long>(cum[j]) - k);
      long db = std::labs(static_cast<long>(cum[best]) - k);
      if (dj < db || (dj == db && cum[j] > cum[best])) best = j;
    }

    // delta_i strictly between mag[best] and the next smaller distinct
    // magnitude (or half of mag[best] for the smallest) -> exactly cum[best]
    // gaps satisfy gap > delta_i under the engine's strict `>`.
    double lower = (best + 1 < mag.size()) ? mag[best + 1] : 0.0;
    deltas.push_back((mag[best] + lower) / 2.0);
  }

  return deltas;
}

}  // namespace nextd
