#pragma once
// SYNC: this table MUST match src/honk_player/taxi_schema.h GetTaxiColumns().
// If that file changes, update here (and vice-versa).

#include <string>
#include <variant>
#include <vector>

namespace nextd {

enum class AttrType { CATEGORICAL, CONTINUOUS };

using Attr = std::variant<double, std::string>;

struct TaxiColumn {
  std::string name;
  AttrType type;
};

inline const std::vector<TaxiColumn>& Columns() {
  static const std::vector<TaxiColumn> kColumns = {
      /* 0  */ {"VendorID", AttrType::CATEGORICAL},
      /* 1  */ {"tpep_pickup_datetime", AttrType::CONTINUOUS},
      /* 2  */ {"tpep_dropoff_datetime", AttrType::CONTINUOUS},
      /* 3  */ {"passenger_count", AttrType::CONTINUOUS},
      /* 4  */ {"trip_distance", AttrType::CONTINUOUS},
      /* 5  */ {"RatecodeID", AttrType::CATEGORICAL},
      /* 6  */ {"store_and_fwd_flag", AttrType::CATEGORICAL},
      /* 7  */ {"PULocationID", AttrType::CATEGORICAL},
      /* 8  */ {"DOLocationID", AttrType::CATEGORICAL},
      /* 9  */ {"payment_type", AttrType::CATEGORICAL},
      /* 10 */ {"fare_amount", AttrType::CONTINUOUS},
      /* 11 */ {"extra", AttrType::CONTINUOUS},
      /* 12 */ {"mta_tax", AttrType::CONTINUOUS},
      /* 13 */ {"tip_amount", AttrType::CONTINUOUS},
      /* 14 */ {"tolls_amount", AttrType::CONTINUOUS},
      /* 15 */ {"improvement_surcharge", AttrType::CONTINUOUS},
      /* 16 */ {"total_amount", AttrType::CONTINUOUS},
      /* 17 */ {"congestion_surcharge", AttrType::CONTINUOUS},
      /* 18 */ {"Airport_fee", AttrType::CONTINUOUS},
      /* 19 */ {"cbd_congestion_fee", AttrType::CONTINUOUS},
  };
  return kColumns;
}

inline int ColIndex(const std::string& name) {
  const auto& cols = Columns();
  for (size_t i = 0; i < cols.size(); i++) {
    if (cols[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace nextd
