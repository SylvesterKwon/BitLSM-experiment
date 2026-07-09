#pragma once
#include "bit_lsm.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace honk {

using namespace bit_lsm;

struct TaxiColumn {
  std::string name;
  AttrRole type;
};

inline std::vector<TaxiColumn> GetTaxiColumns() {
  return {
      /* 0  */ {"VendorID", AttrRole::UNORDERED},
      /* 1  */ {"tpep_pickup_datetime", AttrRole::ORDERED},
      /* 2  */ {"tpep_dropoff_datetime", AttrRole::ORDERED},
      /* 3  */ {"passenger_count", AttrRole::ORDERED},
      /* 4  */ {"trip_distance", AttrRole::ORDERED},
      /* 5  */ {"RatecodeID", AttrRole::UNORDERED},
      /* 6  */ {"store_and_fwd_flag", AttrRole::UNORDERED},
      /* 7  */ {"PULocationID", AttrRole::UNORDERED},
      /* 8  */ {"DOLocationID", AttrRole::UNORDERED},
      /* 9  */ {"payment_type", AttrRole::UNORDERED},
      /* 10 */ {"fare_amount", AttrRole::ORDERED},
      /* 11 */ {"extra", AttrRole::ORDERED},
      /* 12 */ {"mta_tax", AttrRole::ORDERED},
      /* 13 */ {"tip_amount", AttrRole::ORDERED},
      /* 14 */ {"tolls_amount", AttrRole::ORDERED},
      /* 15 */ {"improvement_surcharge", AttrRole::ORDERED},
      /* 16 */ {"total_amount", AttrRole::ORDERED},
      /* 17 */ {"congestion_surcharge", AttrRole::ORDERED},
      /* 18 */ {"Airport_fee", AttrRole::ORDERED},
      /* 19 */ {"cbd_congestion_fee", AttrRole::ORDERED},
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
      opts.attr_specs.push_back(AttrSpec(c.type));
  } else {
    opts.attr_num = indexed_indices.size();
    for (auto idx : indexed_indices)
      opts.attr_specs.push_back(AttrSpec(cols[idx].type));
  }
  return opts;
}

} // namespace honk
