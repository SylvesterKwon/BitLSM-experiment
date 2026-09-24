#pragma once
// Bin assignment for Lazy Bitmaps: BitLSM's binning rule with global (oracle)
// boundaries, so the comparison with BitLSM isolates where the bitmap lives.
//
// kRange attributes take the bin of BitLSM-Global's BinPolicy: boundaries in
// SABI's memcmp domain (the value's okey in 8-byte big-endian), bin = number
// of boundaries <= value, minus one -- sabi_builder.cpp's rule. Boundaries
// ascend and the okey encoding preserves order, so bin ids ascend with the
// value and a range predicate is one contiguous key span in lazy_bitmaps.
// kEquality attributes are exact (one bitmap per distinct value, the key is
// the value itself), which is what the policy holds whenever an attribute's
// cardinality fits its bin budget; a policy that groups values is refused.
//
// Two policy sources, one lookup:
//   - taxi: the file global_bin_policy writes (--bin_policy), the same file
//     bitlsm-global builds with;
//   - synthetic: equal-width boundaries over the schema's [min, max]. The
//     benchmark draws values uniformly, so equal-width is equi-depth, and with
//     equal query weights the SABI greedy hands every attribute 1/rho bins.
// The policy in force is saved next to the DB (kSidecar), so a reopen for
// queries needs neither flag.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_option.h"
#include "global_bins/bin_policy.h"

namespace experiment::lazy_bitmaps {

inline constexpr const char* kSidecar = "lazy_bitmaps.policy";

class Binner {
 public:
  // Refuses a policy built for another rho or schema, or one that groups
  // categorical values.
  static std::unique_ptr<Binner> FromPolicy(
      std::shared_ptr<const global_bins::BinPolicy> policy,
      const bit_lsm::BitLSMOptions& opts, std::string* error);

  // Synthetic policy: round(1/rho) equal-width bins per kRange attribute over
  // [range_min, range_max]; kEquality attributes exact (bitmap_nums =
  // cardinality). Fails on a cardinality above 1/rho, where the SABI greedy
  // would have grouped values.
  static bool UniformPolicy(const bit_lsm::BitLSMOptions& opts,
                            const std::vector<double>& range_min,
                            const std::vector<double>& range_max,
                            const std::vector<int>& cardinalities,
                            global_bins::BinPolicy* out, std::string* error);

  uint32_t Bins(uint32_t attr) const { return policy_->bitmap_nums[attr]; }
  // Bin of v under attr's boundaries (kRange only). A value outside the first
  // and last boundary lands in the edge bin and is counted in clamped(); with
  // an oracle policy that only happens to values an update workload brings in.
  uint32_t Bin(uint32_t attr, double v) const;
  uint64_t clamped() const { return clamped_.load(std::memory_order_relaxed); }
  const global_bins::BinPolicy& policy() const { return *policy_; }

 private:
  explicit Binner(std::shared_ptr<const global_bins::BinPolicy> p)
      : policy_(std::move(p)) {}

  std::shared_ptr<const global_bins::BinPolicy> policy_;
  mutable std::atomic<uint64_t> clamped_{0};
};

}  // namespace experiment::lazy_bitmaps
