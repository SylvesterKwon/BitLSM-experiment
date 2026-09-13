#pragma once
#include "bit_lsm.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace honk {

using namespace bit_lsm;

// The core split AttrSpec into (IndexType x PhysicalType). Every taxi column
// keeps the physical shape the pre-split one-arg AttrSpec gave it: a kRange
// attr was a double in a fixed 8-byte slot, a kEquality attr was opaque
// variable-width bytes.
inline AttrSpec TaxiAttrSpec(IndexType t) {
  return t == IndexType::kRange
             ? AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)
             : AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary, 0);
}

struct TaxiColumn {
  std::string name;
  IndexType type;
};

inline std::vector<TaxiColumn> GetTaxiColumns() {
  return {
      /* 0  */ {"VendorID", IndexType::kEquality},
      /* 1  */ {"tpep_pickup_datetime", IndexType::kRange},
      /* 2  */ {"tpep_dropoff_datetime", IndexType::kRange},
      /* 3  */ {"passenger_count", IndexType::kRange},
      /* 4  */ {"trip_distance", IndexType::kRange},
      /* 5  */ {"RatecodeID", IndexType::kEquality},
      /* 6  */ {"store_and_fwd_flag", IndexType::kEquality},
      /* 7  */ {"PULocationID", IndexType::kEquality},
      /* 8  */ {"DOLocationID", IndexType::kEquality},
      /* 9  */ {"payment_type", IndexType::kEquality},
      /* 10 */ {"fare_amount", IndexType::kRange},
      /* 11 */ {"extra", IndexType::kRange},
      /* 12 */ {"mta_tax", IndexType::kRange},
      /* 13 */ {"tip_amount", IndexType::kRange},
      /* 14 */ {"tolls_amount", IndexType::kRange},
      /* 15 */ {"improvement_surcharge", IndexType::kRange},
      /* 16 */ {"total_amount", IndexType::kRange},
      /* 17 */ {"congestion_surcharge", IndexType::kRange},
      /* 18 */ {"Airport_fee", IndexType::kRange},
      /* 19 */ {"cbd_congestion_fee", IndexType::kRange},
  };
}

inline std::unordered_map<std::string, uint32_t> BuildColumnIndexMap() {
  auto cols = GetTaxiColumns();
  std::unordered_map<std::string, uint32_t> m;
  for (uint32_t i = 0; i < cols.size(); i++)
    m[cols[i].name] = i;
  return m;
}

/// Build BitLSMOptions for the given indexed column indices.
/// If indexed_indices is empty, all columns are indexed.
inline BitLSMOptions
BuildTaxiBitLSMOptions(const std::vector<uint32_t>& indexed_indices = {}) {
  auto cols = GetTaxiColumns();
  BitLSMOptions opts;
  if (indexed_indices.empty()) {
    opts.attr_num = cols.size();
    for (auto& c : cols)
      opts.attr_specs.push_back(TaxiAttrSpec(c.type));
  } else {
    opts.attr_num = indexed_indices.size();
    for (auto idx : indexed_indices)
      opts.attr_specs.push_back(TaxiAttrSpec(cols[idx].type));
  }
  return opts;
}

} // namespace honk
