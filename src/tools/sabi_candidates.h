#pragma once
// One SST's candidate rows for a query, computed exactly as the BitLSM query
// path computes them, through SABIReader's public API.
//
// Mirrors third_party/BitLSM @ 168d595 src/include/sabi_table_iterator.cpp:
// the constructor's skips (query.unsat, QueryCanMatch) and
// SABITableIterator::BuildQueryBitmap, which is private and is transcribed
// here: OR the bins SelectBins picks within a clause, AND across clauses,
// subtract the tombstone. The rows in the returned bitmap are the rows the
// iterator fetches and verifies. Re-check the transcription after a
// submodule bump.
#include <cstdint>
#include <vector>

#include "roaring.hh"
#include "sabi.h"

namespace experiment {

struct SabiCandidates {
  bool skipped = false;  // the SST was skipped before any bitmap work
  roaring::Roaring rows;
};

inline SabiCandidates ComputeSabiCandidates(bit_lsm::SABIReader& reader,
                                            const bit_lsm::SABIQuery& query) {
  SabiCandidates out;
  if (query.unsat || !reader.QueryCanMatch(query)) {
    out.skipped = true;
    return out;
  }

  std::vector<bit_lsm::SABIPinnedBin> pins;
  const auto bin = [&](uint32_t flat) -> const roaring::Roaring& {
    bit_lsm::SABIPinnedBin pin;
    const roaring::Roaring* view = reader.Bin(flat, &pin);
    if (view == nullptr) {
      fprintf(stderr, "SABI bin read failed\n");
      abort();
    }
    if (pin.view != nullptr) pins.push_back(std::move(pin));
    return *view;
  };

  roaring::Roaring tombstone;
  if (reader.TombstoneCardinality() != 0) tombstone = bin(reader.TotalBins());

  if (query.clause_groups.empty()) {
    const uint32_t total = reader.data_entries_cnt_psum.empty()
                               ? 0
                               : reader.data_entries_cnt_psum.back();
    out.rows.addRange(0, total);
    out.rows -= tombstone;
    return out;
  }

  bool first = true;
  for (const auto& clause : query.clause_groups) {
    roaring::Roaring clause_rows;  // an empty clause matches nothing
    for (const auto& cond : clause) {
      bit_lsm::BinSelection sel;
      if (!reader.SelectBins(cond, &sel)) continue;
      for (uint32_t f = sel.first; f <= sel.last; ++f) clause_rows |= bin(f);
    }
    if (first) {
      out.rows = std::move(clause_rows);
      first = false;
    } else {
      out.rows &= clause_rows;
    }
    if (out.rows.isEmpty()) return out;
  }
  out.rows -= tombstone;
  return out;
}

}  // namespace experiment
