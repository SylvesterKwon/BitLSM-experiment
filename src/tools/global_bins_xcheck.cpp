// Gate for the global-bins fork (src/bindings/global_bins). Run it before any
// bitlsm-global measurement and after every BitLSM submodule bump.
//
// 1. Fork fidelity: for the same rows, the core bit_lsm::SABIBuilder, the fork
//    with no policy, and the fork with a policy computed from those very rows
//    must write byte-identical blobs. The first equality proves the copy is
//    the core; the second proves the global path adds nothing but its input
//    (a policy built from an SST's own rows is that SST's local binning, and
//    the clamp is then a no-op).
// 2. Global bins stay correct: SSTs cut from a larger dataset are built with
//    the dataset-wide policy, opened with the core SABIReader, and queried.
//    Every row that satisfies a query must be a candidate (no false
//    negatives), the reader's per-SST skip must never drop a matching SST,
//    and interior boundaries must be the policy's.
// 3. The abort paths fire: a value outside the policy's range, a categorical
//    value the policy never saw, and a policy built for another rho.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "bit_lsm_query.h"
#include "global_bins/bin_policy.h"
#include "global_bins/global_sabi_builder.h"
#include "global_bins/global_sabi_factory.h"
#include "sabi.h"
#include "sabi_candidates.h"

using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::IndexType;
using bit_lsm::PhysicalType;
using bit_lsm::QueryCondition;
using experiment::global_bins::BinPolicy;
using experiment::global_bins::GlobalSABIBuilder;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << what << "\n";
  }
}

struct Row {
  std::string key;
  std::vector<Attr> attrs;
  std::string value;  // v3-encoded; empty for a tombstone
  bool tombstone = false;
};

struct Schema {
  std::string name;
  BitLSMOptions opts;
};

BitLSMOptions MakeOptions(std::vector<AttrSpec> specs, double rho) {
  BitLSMOptions o{};
  o.attr_num = static_cast<uint32_t>(specs.size());
  o.attr_specs = std::move(specs);
  o.rho = rho;
  return o;
}

// Taxi-like: a time attr that tracks row order (so contiguous slices are
// PK-correlated), a heavy-hitter fare, a skewed location, a tiny-domain count,
// and a two-value vendor.
Schema TaxiLike() {
  return {"taxi-like",
          MakeOptions({AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                       AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                       AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary),
                       AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                       AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)},
                      0.01)};
}

// Nullable and non-float physical types.
Schema Mixed() {
  return {"mixed",
          MakeOptions(
              {AttrSpec(IndexType::kRange, PhysicalType::kInt, 8, true),
               AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary, 0, true),
               AttrSpec(IndexType::kRange, PhysicalType::kVarBinary),
               AttrSpec(IndexType::kRange, PhysicalType::kUint, 4)},
              0.05)};
}

Attr RandomAttr(const Schema& s, uint32_t a, uint64_t i, std::mt19937_64& rng) {
  std::uniform_real_distribution<double> u(0, 1);
  if (s.name == "taxi-like") {
    switch (a) {
      case 0:
        return 1.7e9 + static_cast<double>(i) * 10 +
               std::floor(u(rng) * 30);
      case 1:
        return u(rng) < 0.3 ? 52.0 : std::round(std::exp(u(rng) * 5) * 100) / 100;
      case 2:
        return std::to_string(static_cast<int>(std::pow(u(rng), 3) * 50));
      case 3:
        return static_cast<double>(static_cast<int>(u(rng) * 8) - 1);
      default:
        return std::string(u(rng) < 0.7 ? "1" : "2");
    }
  }
  if (a < 2 && u(rng) < 0.1) return std::monostate{};
  switch (a) {
    case 0:
      return static_cast<int64_t>(u(rng) * 2000) - 1000;
    case 1:
      return "v" + std::to_string(static_cast<int>(u(rng) * 20));
    case 2:
      return std::string(static_cast<size_t>(u(rng) * 6), 'a' + (i % 26));
    default:
      return static_cast<uint64_t>(u(rng) * 50);
  }
}

std::vector<Row> MakeRows(const Schema& s, uint64_t n, double tombstone_rate,
                          uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u(0, 1);
  const bit_lsm::ValueLayout layout(s.opts);
  std::vector<Row> rows(n);
  for (uint64_t i = 0; i < n; ++i) {
    Row& r = rows[i];
    r.key = "k" + std::to_string(1000000000 + i);
    r.tombstone = u(rng) < tombstone_rate;
    r.attrs.resize(s.opts.attr_num);
    for (uint32_t a = 0; a < s.opts.attr_num; ++a)
      r.attrs[a] = RandomAttr(s, a, i, rng);
    if (!r.tombstone) bit_lsm::EncodeValue(layout, r.attrs, "payload", r.value);
  }
  return rows;
}

std::unique_ptr<bit_lsm::AttrExtractor> Extractor(const Schema& s) {
  return std::make_unique<bit_lsm::ValueLayoutExtractor>(s.opts);
}

// Rows in, blob out, the way a table builder drives a UDI builder: every key,
// then an index entry per block.
std::string BuildBlob(rocksdb::UserDefinedIndexBuilder& b,
                      const std::vector<const Row*>& rows) {
  constexpr uint32_t kRowsPerBlock = 20;
  uint64_t offset = 0;
  std::string scratch;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = *rows[i];
    b.OnKeyAdded(r.key,
                 r.tombstone ? rocksdb::UserDefinedIndexBuilder::kTypeDeletion
                             : rocksdb::UserDefinedIndexBuilder::kValue,
                 r.value);
    if ((i + 1) % kRowsPerBlock == 0 || i + 1 == rows.size()) {
      b.AddIndexEntry(r.key, nullptr, {offset, 4096}, &scratch);
      offset += 4096;
    }
  }
  rocksdb::Slice out;
  rocksdb::Status s = b.Finish(&out);
  if (!s.ok()) {
    std::cerr << "Finish failed: " << s.ToString() << "\n";
    abort();
  }
  return out.ToString();
}

BinPolicy PolicyOf(const Schema& s, const std::vector<const Row*>& rows) {
  GlobalSABIBuilder b(bit_lsm::SABISchema::FromOptions(s.opts), Extractor(s),
                      nullptr);
  for (const Row* r : rows)
    b.OnKeyAdded(r->key,
                 r->tombstone ? rocksdb::UserDefinedIndexBuilder::kTypeDeletion
                              : rocksdb::UserDefinedIndexBuilder::kValue,
                 r->value);
  return b.ExportLocalPolicy();
}

std::string CoreBlob(const Schema& s, const std::vector<const Row*>& rows) {
  bit_lsm::SABIBuilder b(bit_lsm::SABISchema::FromOptions(s.opts), Extractor(s));
  return BuildBlob(b, rows);
}

std::string ForkBlob(const Schema& s, const std::vector<const Row*>& rows,
                     std::shared_ptr<const BinPolicy> policy) {
  GlobalSABIBuilder b(bit_lsm::SABISchema::FromOptions(s.opts), Extractor(s),
                      std::move(policy));
  return BuildBlob(b, rows);
}

std::vector<const Row*> Ptrs(const std::vector<Row>& rows, size_t from,
                             size_t to) {
  std::vector<const Row*> out;
  for (size_t i = from; i < to; ++i) out.push_back(&rows[i]);
  return out;
}

// ---- 1. fidelity ----

void CheckFidelity(const Schema& s, const std::string& label,
                   const std::vector<const Row*>& rows) {
  const std::string core = CoreBlob(s, rows);
  Check(ForkBlob(s, rows, nullptr) == core,
        s.name + "/" + label + ": fork without policy != core");
  auto policy = std::make_shared<BinPolicy>(PolicyOf(s, rows));
  Check(ForkBlob(s, rows, policy) == core,
        s.name + "/" + label + ": fork with own-rows policy != core");
}

// ---- 2. global correctness ----

// A random query shaped like the experiment's: 1-3 attributes, a range on
// ordered attrs drawn from two data rows, equality on categorical ones.
BitLSMQuery RandomQuery(const Schema& s, const std::vector<Row>& data,
                        std::mt19937_64& rng) {
  std::uniform_int_distribution<size_t> pick(0, data.size() - 1);
  std::vector<uint32_t> attrs(s.opts.attr_num);
  for (uint32_t a = 0; a < attrs.size(); ++a) attrs[a] = a;
  std::shuffle(attrs.begin(), attrs.end(), rng);
  const uint32_t k = 1 + rng() % 3;
  std::vector<QueryCondition> conds;
  for (uint32_t j = 0; j < k && j < attrs.size(); ++j) {
    const uint32_t a = attrs[j];
    const Attr* x = nullptr;
    const Attr* y = nullptr;
    for (int tries = 0; tries < 100 && (!x || !y); ++tries) {
      const Row& r = data[pick(rng)];
      if (r.tombstone || std::holds_alternative<std::monostate>(r.attrs[a]))
        continue;
      (x ? y : x) = &r.attrs[a];
    }
    if (!x || !y) continue;
    const auto as_value = [](const Attr& v) -> QueryCondition::value_type {
      if (auto* d = std::get_if<double>(&v)) return *d;
      if (auto* i = std::get_if<int64_t>(&v)) return *i;
      if (auto* u = std::get_if<uint64_t>(&v)) return *u;
      return std::get<std::string>(v);
    };
    if (s.opts.attr_specs[a].index_type == IndexType::kEquality) {
      conds.push_back({a, CompareOp::EQUAL, as_value(*x)});
    } else {
      auto lo = as_value(*x), hi = as_value(*y);
      if (hi < lo) std::swap(lo, hi);
      conds.push_back({a, CompareOp::GREATER_EQUAL, lo});
      conds.push_back({a, rng() % 2 ? CompareOp::LESS : CompareOp::LESS_EQUAL,
                       hi});
    }
  }
  return BitLSMQuery(std::move(conds));
}

void CheckGlobalCorrectness(const Schema& s, const std::vector<Row>& data,
                            const std::vector<std::vector<const Row*>>& ssts,
                            const std::string& label) {
  std::vector<const Row*> all;
  for (const Row& r : data) all.push_back(&r);
  auto policy = std::make_shared<BinPolicy>(PolicyOf(s, all));
  const bit_lsm::ValueLayout layout(s.opts);

  std::mt19937_64 rng(7);
  uint64_t global_candidates = 0, local_candidates = 0, matches = 0;
  size_t differing_blobs = 0;
  for (size_t f = 0; f < ssts.size(); ++f) {
    const std::string blob = ForkBlob(s, ssts[f], policy);
    const std::string local = CoreBlob(s, ssts[f]);
    if (blob != local) ++differing_blobs;
    rocksdb::Slice gs(blob), ls(local);
    bit_lsm::SABIReader greader(gs), lreader(ls);

    // Interior boundaries are the policy's; the ends are this SST's bounds.
    for (uint32_t a = 0; a < s.opts.attr_num; ++a) {
      if (s.opts.attr_specs[a].index_type != IndexType::kRange) continue;
      Check(greader.bitmap_index.bitmap_nums[a] == policy->bitmap_nums[a],
            label + ": bin count is not the policy's");
      const auto& got =
          std::get<bit_lsm::BytesList>(greader.bitmap_index.binning_policy[a]);
      const auto& want = std::get<bit_lsm::BytesList>(policy->binning_policy[a]);
      if (got.size() != want.size() || got[0] == "") continue;
      for (size_t b = 1; b + 1 < got.size(); ++b)
        Check(got[b] == std::clamp(want[b], got[0], got.back()),
              label + ": boundary " + std::to_string(b) +
                  " is not the clamped policy boundary");
    }

    for (int q = 0; q < 200; ++q) {
      BitLSMQuery query = RandomQuery(s, data, rng);
      if (query.clause_groups.empty()) continue;
      Check(query.Validate(s.opts).ok(), label + ": generated query invalid");
      const bit_lsm::SABIQuery sq = bit_lsm::EncodeQuery(query, s.opts);
      const auto g = experiment::ComputeSabiCandidates(greader, sq);
      const auto l = experiment::ComputeSabiCandidates(lreader, sq);
      for (uint32_t row = 0; row < ssts[f].size(); ++row) {
        const Row& r = *ssts[f][row];
        if (r.tombstone || !query.CheckCondition(r.value, layout)) continue;
        ++matches;
        Check(!g.skipped && g.rows.contains(row),
              label + ": global bins missed a matching row");
        Check(!l.skipped && l.rows.contains(row),
              label + ": local bins missed a matching row");
      }
      global_candidates += g.rows.cardinality();
      local_candidates += l.rows.cardinality();
    }
  }
  // Otherwise the checks above could pass on local bins by accident.
  Check(differing_blobs == ssts.size(),
        label + ": a dataset-wide policy left some SST's blob identical to its "
                "local one");
  std::cout << "  " << label << ": matches=" << matches
            << " candidates local=" << local_candidates
            << " global=" << global_candidates << "\n";
}

// ---- 3. abort paths ----

bool AbortsInChild(const std::function<void()>& fn) {
  std::cout.flush();
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

}  // namespace

int main() {
  for (const Schema& s : {TaxiLike(), Mixed()}) {
    std::cout << "[" << s.name << "] fidelity\n";
    const std::vector<Row> rows = MakeRows(s, 50000, 0.02, 1);
    CheckFidelity(s, "1 row", Ptrs(rows, 0, 1));
    CheckFidelity(s, "2 rows", Ptrs(rows, 0, 2));
    CheckFidelity(s, "137 rows", Ptrs(rows, 100, 237));
    CheckFidelity(s, "5000 rows", Ptrs(rows, 0, 5000));
    CheckFidelity(s, "50000 rows", Ptrs(rows, 0, 50000));
    std::vector<Row> same = MakeRows(s, 3000, 0, 2);
    for (Row& r : same) r = same[0];
    CheckFidelity(s, "all rows equal", Ptrs(same, 0, same.size()));
    std::vector<Row> tomb = MakeRows(s, 500, 1.0, 3);
    CheckFidelity(s, "all tombstones", Ptrs(tomb, 0, tomb.size()));

    std::cout << "[" << s.name << "] global bins\n";
    // PK-correlated SSTs: contiguous slices of the time-ordered rows.
    std::vector<std::vector<const Row*>> sorted_ssts;
    for (size_t f = 0; f < 10; ++f)
      sorted_ssts.push_back(Ptrs(rows, f * 5000, (f + 1) * 5000));
    CheckGlobalCorrectness(s, rows, sorted_ssts, s.name + "/correlated");
    // Uncorrelated SSTs: the same rows shuffled.
    std::vector<const Row*> shuffled = Ptrs(rows, 0, rows.size());
    std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937_64(9));
    std::vector<std::vector<const Row*>> random_ssts;
    for (size_t f = 0; f < 10; ++f)
      random_ssts.emplace_back(shuffled.begin() + f * 5000,
                               shuffled.begin() + (f + 1) * 5000);
    CheckGlobalCorrectness(s, rows, random_ssts, s.name + "/uncorrelated");
  }

  std::cout << "[abort paths]\n";
  const Schema s = TaxiLike();
  const std::vector<Row> rows = MakeRows(s, 2000, 0, 4);
  auto policy = std::make_shared<BinPolicy>(PolicyOf(s, Ptrs(rows, 0, 2000)));
  const bit_lsm::ValueLayout layout(s.opts);

  // Each case is rows the policy covers with exactly one value broken, so an
  // abort can only come from that value.
  std::vector<Row> out_of_range(rows.begin(), rows.begin() + 10);
  out_of_range[3].attrs[0] = 1.0e10;
  bit_lsm::EncodeValue(layout, out_of_range[3].attrs, "payload",
                       out_of_range[3].value);
  Check(AbortsInChild([&] {
          ForkBlob(s, Ptrs(out_of_range, 0, out_of_range.size()), policy);
        }),
        "value outside the policy range did not abort");

  std::vector<Row> unseen(rows.begin(), rows.begin() + 10);
  unseen[5].attrs[2] = std::string("never-seen");
  bit_lsm::EncodeValue(layout, unseen[5].attrs, "payload", unseen[5].value);
  Check(AbortsInChild([&] {
          ForkBlob(s, Ptrs(unseen, 0, unseen.size()), policy);
        }),
        "categorical value absent from the policy did not abort");

  BitLSMOptions other_rho = s.opts;
  other_rho.rho = 0.02;
  Check(AbortsInChild([&] {
          experiment::global_bins::GlobalSABIFactory f(other_rho, policy);
        }),
        "policy for another rho did not abort");

  Check(AbortsInChild([&] {
          ForkBlob(s, Ptrs(rows, 0, rows.size()), policy);
        }) == false,
        "rows inside the policy aborted");

  if (failures) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
