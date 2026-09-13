#pragma once
// Oracle binning policy for the global-bins ablation: one set of bin
// boundaries computed once over the whole workload and shared by every SST.
//
// The bin counts come from the same greedy the SABI builder runs per SST
// (attr_num / rho bitmaps, capped by cardinality), just fed the full dataset,
// so the only thing the ablation changes is what the boundaries were computed
// from. See global_sabi_builder.h for how an SST applies it.
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <rocksdb/status.h>

#include "bit_lsm_encoding.h"
#include "bytes_list.h"

namespace experiment::global_bins {

struct BinPolicy {
  double rho = 0;
  // Rows the policy was computed over, and the workload they came from, so a
  // policy cannot be silently applied to a different dataset.
  uint64_t rows = 0;
  std::string source;
  uint64_t source_bytes = 0;

  std::vector<bit_lsm::IndexType> index_types;
  std::vector<uint32_t> bitmap_nums;
  // kRange: bin_count + 1 boundaries in SABI's memcmp domain.
  // kEquality: (value, bin) sorted by value.
  std::vector<std::variant<bit_lsm::BytesList,
                           std::vector<std::pair<std::string, uint32_t>>>>
      binning_policy;
};

rocksdb::Status SaveBinPolicy(const BinPolicy& policy, const std::string& path);
rocksdb::Status LoadBinPolicy(const std::string& path, BinPolicy* out);

// The policy must have been built for this schema: same rho, same attribute
// count, same index type per attribute.
rocksdb::Status CheckBinPolicy(const BinPolicy& policy,
                               const bit_lsm::SABISchema& schema);

}  // namespace experiment::global_bins
