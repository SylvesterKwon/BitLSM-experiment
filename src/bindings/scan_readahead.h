#pragma once
// Scan readahead planning shared by the baseline scans, so every method makes
// the identical I/O-planning decision and comparisons measure the index
// design rather than which scan happened to trip RocksDB's readahead ramp.
//
// The reactive ramp in BlockPrefetcher only engages on strictly adjacent block
// reads, so a scan that skips even one block between targets keeps restarting
// it and pays a separate request per block. Every index here knows its whole
// candidate block set before the first read, so the window is a calculation
// instead of a guess. The calculation itself is BitLSM's — reused verbatim
// rather than reimplemented, so the three scans cannot drift apart.
#include <bit_lsm_iterator.h>

#include <cstddef>
#include <utility>
#include <vector>

#include <rocksdb/table.h>
#include "table/format.h"

namespace experiment {

// Readahead window for a scan over `candidate_blocks`, or 0 to leave RocksDB's
// implicit adaptive readahead in charge. `candidate_blocks` must be in file
// order, which is how every candidate set here is built.
inline size_t PlanScanReadaheadSize(
    const std::vector<rocksdb::BlockHandle>& candidate_blocks,
    size_t max_readahead_size) {
  // bit_lsm::ChooseScanReadaheadSize reads only the handle of each entry; the
  // index half of the pair carries no meaning for it.
  std::vector<std::pair<uint32_t, rocksdb::BlockHandle>> targets;
  targets.reserve(candidate_blocks.size());
  for (const rocksdb::BlockHandle& bh : candidate_blocks)
    targets.emplace_back(0, bh);
  return bit_lsm::ChooseScanReadaheadSize(targets, max_readahead_size);
}

}  // namespace experiment
