// Assert-based unit test for Binner. Run: ./build/bin/lazy_bitmaps_test_binner
#include "lazy_bitmaps_binner.h"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "bit_lsm_encoding.h"

using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::IndexType;
using bit_lsm::PhysicalType;
using experiment::global_bins::BinPolicy;
using experiment::lazy_bitmaps::Binner;

static BitLSMOptions Opts(double rho) {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary, 0),
                  AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                  AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)};
  o.rho = rho;
  return o;
}

int main() {
  std::string err;

  // Uniform policy: 1/rho equal-width bins per kRange attr, categorical exact.
  BinPolicy p;
  assert(Binner::UniformPolicy(Opts(0.01), {0, 0.0, -100.0},
                               {0, 1000.0, 100.0}, {50, 0, 0}, &p, &err));
  assert(p.bitmap_nums == std::vector<uint32_t>({50, 100, 100}));
  auto binner =
      Binner::FromPolicy(std::make_shared<BinPolicy>(p), Opts(0.01), &err);
  assert(binner && err.empty());
  assert(binner->Bins(1) == 100 && binner->Bins(2) == 100);
  assert(binner->Bin(1, 0.0) == 0);
  assert(binner->Bin(1, 5.0) == 0);
  assert(binner->Bin(1, 10.0) == 1);
  assert(binner->Bin(1, 500.0) == 50);
  assert(binner->Bin(1, 999.99) == 99);
  assert(binner->Bin(1, 1000.0) == 99);  // the last bin includes its upper bound
  assert(binner->Bin(2, -100.0) == 0 && binner->Bin(2, 0.0) == 50 &&
         binner->Bin(2, 99.0) == 99);
  assert(binner->clamped() == 0);
  // Monotone in the value.
  uint32_t prev = 0;
  for (double v = 0; v <= 1000; v += 0.37) {
    uint32_t b = binner->Bin(1, v);
    assert(b >= prev && b < 100);
    prev = b;
  }
  // Out-of-range values land in an edge bin and are counted.
  assert(binner->Bin(1, -1.0) == 0 && binner->clamped() == 1);
  assert(binner->Bin(1, 2000.0) == 99 && binner->clamped() == 2);

  // Sidecar roundtrip through the global_bins file format.
  const std::string dir =
      std::filesystem::temp_directory_path() /
      ("lazy_bitmaps_test_binner_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  const std::string path = dir + "/" + experiment::lazy_bitmaps::kSidecar;
  assert(experiment::global_bins::SaveBinPolicy(p, path).ok());
  BinPolicy back;
  assert(experiment::global_bins::LoadBinPolicy(path, &back).ok());
  assert(back.rho == p.rho && back.bitmap_nums == p.bitmap_nums &&
         back.index_types == p.index_types);
  for (uint32_t i = 1; i < 3; ++i) {
    const auto& a = std::get<bit_lsm::BytesList>(p.binning_policy[i]);
    const auto& b = std::get<bit_lsm::BytesList>(back.binning_policy[i]);
    assert(a.arena == b.arena && a.ends == b.ends);
  }
  std::filesystem::remove_all(dir);

  // Refusals: another rho, another schema, a policy that groups categorical
  // values.
  assert(!Binner::FromPolicy(std::make_shared<BinPolicy>(p), Opts(0.001),
                             &err) &&
         !err.empty());
  BitLSMOptions other = Opts(0.01);
  other.attr_specs[1] =
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary, 0);
  assert(!Binner::FromPolicy(std::make_shared<BinPolicy>(p), other, &err));
  BinPolicy grouped = p;
  grouped.bitmap_nums[0] = 2;
  std::get<std::vector<std::pair<std::string, uint32_t>>>(
      grouped.binning_policy[0]) = {{"a", 0}, {"b", 1}, {"c", 1}};
  assert(!Binner::FromPolicy(std::make_shared<BinPolicy>(grouped), Opts(0.01),
                             &err));
  // A cardinality above 1/rho would have been grouped by the SABI greedy.
  BinPolicy too_many;
  assert(!Binner::UniformPolicy(Opts(0.01), {0, 0.0, 0.0}, {0, 1.0, 1.0},
                                {101, 0, 0}, &too_many, &err));

  // A loaded (equi-depth style) policy bins by "boundaries <= value, minus
  // one".
  BinPolicy depth = p;
  bit_lsm::BytesList bounds;
  for (double b : {0.0, 1.0, 2.0, 10.0, 100.0})  // 4 uneven bins
    bounds.push_back(bit_lsm::OkeyToBytes(bit_lsm::F64ToOkey(b)));
  depth.bitmap_nums[1] = 4;
  depth.binning_policy[1] = bounds;
  auto dbin = Binner::FromPolicy(std::make_shared<BinPolicy>(depth),
                                 Opts(0.01), &err);
  assert(dbin);
  assert(dbin->Bin(1, 0.0) == 0 && dbin->Bin(1, 0.5) == 0 &&
         dbin->Bin(1, 1.0) == 1 && dbin->Bin(1, 1.5) == 1 &&
         dbin->Bin(1, 2.0) == 2 && dbin->Bin(1, 9.99) == 2 &&
         dbin->Bin(1, 10.0) == 3 && dbin->Bin(1, 100.0) == 3);

  std::puts("lazy_bitmaps_test_binner: OK");
  return 0;
}
