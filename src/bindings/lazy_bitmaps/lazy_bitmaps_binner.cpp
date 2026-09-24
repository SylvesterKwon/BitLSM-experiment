#include "lazy_bitmaps_binner.h"

#include <cmath>
#include <string_view>
#include <variant>

#include "bit_lsm_encoding.h"
#include "bytes_list.h"

namespace experiment::lazy_bitmaps {

std::unique_ptr<Binner> Binner::FromPolicy(
    std::shared_ptr<const global_bins::BinPolicy> policy,
    const bit_lsm::BitLSMOptions& opts, std::string* error) {
  error->clear();
  rocksdb::Status s = global_bins::CheckBinPolicy(
      *policy, bit_lsm::SABISchema::FromOptions(opts));
  if (!s.ok()) {
    *error = s.ToString();
    return nullptr;
  }
  for (uint32_t i = 0; i < policy->index_types.size(); ++i) {
    if (policy->index_types[i] != bit_lsm::IndexType::kEquality) continue;
    const auto& entries =
        std::get<std::vector<std::pair<std::string, uint32_t>>>(
            policy->binning_policy[i]);
    if (entries.size() > policy->bitmap_nums[i]) {
      *error = "attr " + std::to_string(i) + ": the policy groups " +
               std::to_string(entries.size()) + " values into " +
               std::to_string(policy->bitmap_nums[i]) +
               " bins; Lazy Bitmaps indexes categorical values exactly";
      return nullptr;
    }
  }
  return std::unique_ptr<Binner>(new Binner(std::move(policy)));
}

bool Binner::UniformPolicy(const bit_lsm::BitLSMOptions& opts,
                           const std::vector<double>& range_min,
                           const std::vector<double>& range_max,
                           const std::vector<int>& cardinalities,
                           global_bins::BinPolicy* out, std::string* error) {
  error->clear();
  const uint32_t per_attr =
      static_cast<uint32_t>(std::lround(1.0 / opts.rho));
  if (per_attr == 0) {
    *error = "rho must be at most 1";
    return false;
  }
  global_bins::BinPolicy p;
  p.rho = opts.rho;
  p.source = "uniform";
  for (uint32_t i = 0; i < opts.attr_num; ++i) {
    const bit_lsm::IndexType type = opts.attr_specs[i].index_type;
    p.index_types.push_back(type);
    if (type == bit_lsm::IndexType::kRange) {
      if (!(range_max[i] > range_min[i])) {
        *error = "attr " + std::to_string(i) + ": empty range";
        return false;
      }
      bit_lsm::BytesList bounds;
      for (uint32_t j = 0; j <= per_attr; ++j) {
        const double v =
            range_min[i] + (range_max[i] - range_min[i]) *
                               (static_cast<double>(j) / per_attr);
        bounds.push_back(bit_lsm::OkeyToBytes(bit_lsm::F64ToOkey(v)));
      }
      p.bitmap_nums.push_back(per_attr);
      p.binning_policy.emplace_back(std::move(bounds));
    } else {
      if (cardinalities[i] <= 0 ||
          static_cast<uint32_t>(cardinalities[i]) > per_attr) {
        *error = "attr " + std::to_string(i) + ": cardinality " +
                 std::to_string(cardinalities[i]) +
                 " exceeds 1/rho; the SABI greedy would group its values";
        return false;
      }
      p.bitmap_nums.push_back(static_cast<uint32_t>(cardinalities[i]));
      p.binning_policy.emplace_back(
          std::vector<std::pair<std::string, uint32_t>>{});
    }
  }
  *out = std::move(p);
  return true;
}

uint32_t Binner::Bin(uint32_t attr, double v) const {
  const auto& bounds =
      std::get<bit_lsm::BytesList>(policy_->binning_policy[attr]);
  char okey[bit_lsm::kOkeyBytes];
  bit_lsm::OkeyToBytes(bit_lsm::F64ToOkey(v), okey);
  const std::string_view key(okey, bit_lsm::kOkeyBytes);
  const uint32_t bins = policy_->bitmap_nums[attr];
  const uint32_t passed = bounds.UpperBound(key);  // boundaries <= v
  if (passed == 0) {
    clamped_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  if (key > bounds.back()) clamped_.fetch_add(1, std::memory_order_relaxed);
  const uint32_t bin = passed - 1;
  return bin < bins ? bin : bins - 1;  // the last bin includes its upper bound
}

}  // namespace experiment::lazy_bitmaps
