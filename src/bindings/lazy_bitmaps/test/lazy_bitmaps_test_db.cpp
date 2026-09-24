// End-to-end test of LazyBitmapsBinding through the Binding interface: the
// spec's acceptance checks 1 (result equality against an oracle, before and
// after overwrites) and 2 (a reopen resumes the rowid counter at max + 1).
// Run: ./build/bin/lazy_bitmaps_test_db
#include "lazy_bitmaps_binding.h"

#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include "schema_loader.h"

using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::IndexType;
using experiment::LazyBitmapsBinding;

namespace {

const char* kSchemaJson = R"({"attrs": [
  {"type": "categorical", "cardinality": 50},
  {"type": "continuous", "min": 0.0, "max": 1000.0},
  {"type": "categorical", "cardinality": 20},
  {"type": "continuous", "min": -100.0, "max": 100.0}]})";

constexpr uint64_t kRows = 20000;
constexpr uint64_t kOverwrites = 5000;
constexpr int kQueries = 200;

struct Fixture {
  std::string dir, schema_path, db_path;
  Schema schema;
  std::vector<std::string> argv_store;
  std::vector<char*> argv;

  Fixture() {
    dir = std::filesystem::temp_directory_path() /
          ("lazy_bitmaps_test_db_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    schema_path = dir + "/schema.json";
    std::ofstream(schema_path) << kSchemaJson;
    db_path = dir + "/db";
    schema = load_schema(schema_path);
    argv_store = {"lazy_bitmaps_test_db", "--schema", schema_path, "--rho",
                  "0.01"};
    for (auto& s : argv_store) argv.push_back(s.data());
  }
  ~Fixture() { std::filesystem::remove_all(dir); }
  int argc() const { return static_cast<int>(argv.size()); }
};

std::vector<Attr> RandomAttrs(const Schema& s, std::mt19937& gen) {
  std::vector<Attr> attrs(s.options.attr_num);
  for (uint32_t j = 0; j < s.options.attr_num; ++j) {
    if (s.options.attr_specs[j].index_type == IndexType::kEquality)
      attrs[j] = std::to_string(
          std::uniform_int_distribution<int>(0, s.cardinalities[j] - 1)(gen));
    else
      attrs[j] = std::uniform_real_distribution<double>(
          s.range_min[j], s.range_max[j])(gen);
  }
  return attrs;
}

// c attributes chosen at random; categorical -> OR of k values, continuous ->
// [lo, lo + width). Mirrors benchmark_experiment.h's build_read_query.
BitLSMQuery RandomQuery(const Schema& s, std::mt19937& gen) {
  const uint32_t c =
      std::uniform_int_distribution<uint32_t>(1, s.options.attr_num)(gen);
  std::vector<uint32_t> idx(s.options.attr_num);
  for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::shuffle(idx.begin(), idx.end(), gen);
  idx.resize(c);
  const double sel = std::vector<double>{0.02, 0.1, 0.3}[gen() % 3];
  BitLSMQuery q;
  for (uint32_t a : idx) {
    if (s.options.attr_specs[a].index_type == IndexType::kRange) {
      const double range = s.range_max[a] - s.range_min[a];
      const double width = sel * range;
      const double lo = std::uniform_real_distribution<double>(
          s.range_min[a], s.range_max[a] - width)(gen);
      q.clause_groups.push_back({{a, CompareOp::GREATER_EQUAL, lo}});
      q.clause_groups.push_back({{a, CompareOp::LESS, lo + width}});
    } else {
      const int card = s.cardinalities[a];
      const int k = std::max(1, static_cast<int>(std::lround(sel * card)));
      std::set<int> chosen;
      bit_lsm::OrClause clause;
      while (static_cast<int>(chosen.size()) < k) {
        int v = std::uniform_int_distribution<int>(0, card - 1)(gen);
        if (chosen.insert(v).second)
          clause.push_back({a, CompareOp::EQUAL, std::to_string(v)});
      }
      q.clause_groups.push_back(std::move(clause));
    }
  }
  return q;
}

struct Oracle {
  bit_lsm::ValueLayout layout;
  std::unordered_map<std::string, std::string> latest;  // pk -> encoded row
  explicit Oracle(const Schema& s) : layout(s.options) {}
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) {
    std::string v;
    bit_lsm::EncodeValue(layout, attrs, payload, v);
    latest[pk] = v;
  }
  uint64_t Count(const BitLSMQuery& q) const {
    uint64_t n = 0;
    for (const auto& [pk, v] : latest)
      if (q.CheckCondition(rocksdb::Slice(v), layout)) ++n;
    return n;
  }
};

std::string Pk(uint64_t i) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "pk%08llu",
                static_cast<unsigned long long>(i));
  return buf;
}

}  // namespace

int main() {
  Fixture f;
  Oracle oracle(f.schema);
  std::mt19937 gen(42);
  const std::string payload(16, 'x');

  LazyBitmapsBinding b;
  b.Open(f.argc(), f.argv.data(), f.db_path, f.schema.options);
  assert(b.NextRowid() == 0);
  for (uint64_t i = 0; i < kRows; ++i) {
    auto attrs = RandomAttrs(f.schema, gen);
    b.Put(Pk(i), attrs, payload);
    oracle.Put(Pk(i), attrs, payload);
  }
  assert(b.NextRowid() == kRows);

  std::vector<BitLSMQuery> queries;
  for (int i = 0; i < kQueries; ++i) queries.push_back(RandomQuery(f.schema, gen));

  // Before any update every rowid maps to a distinct PK: |Q| == |PKs|.
  int nonzero = 0;
  for (auto& q : queries) {
    auto r = b.Scan(q);
    assert(r.ok && r.matched == oracle.Count(q));
    auto c = b.LastScanCounts();
    assert(c.q == c.pks && c.matched == r.matched);
    nonzero += r.matched > 0;
  }
  assert(nonzero > kQueries / 2);

  // Overwrites leave stale rowids behind; dedup and validation hide them.
  for (uint64_t i = 0; i < kOverwrites; ++i) {
    const std::string pk = Pk(gen() % kRows);
    auto attrs = RandomAttrs(f.schema, gen);
    b.Put(pk, attrs, payload);
    oracle.Put(pk, attrs, payload);
  }
  assert(b.NextRowid() == kRows + kOverwrites);
  uint64_t stale = 0;
  for (auto& q : queries) {
    auto r = b.Scan(q);
    assert(r.ok && r.matched == oracle.Count(q));
    auto c = b.LastScanCounts();
    assert(c.q >= c.pks);
    stale += c.q - c.pks;
  }
  assert(stale > 0);

  // Reopen: the counter resumes at max + 1 (from rowid_map) and the sidecar
  // policy is used, so the same --schema flag is accepted and ignored.
  b.Close();
  LazyBitmapsBinding b2;
  b2.Open(f.argc(), f.argv.data(), f.db_path, f.schema.options);
  assert(b2.NextRowid() == kRows + kOverwrites);
  {
    auto attrs = RandomAttrs(f.schema, gen);
    b2.Put(Pk(kRows), attrs, payload);
    oracle.Put(Pk(kRows), attrs, payload);
  }
  assert(b2.NextRowid() == kRows + kOverwrites + 1);
  for (auto& q : queries) {
    auto r = b2.Scan(q);
    assert(r.ok && r.matched == oracle.Count(q));
  }
  // The index I/O counters move on a reopened (cold block cache) DB.
  assert(b2.GetIndexIoStats().bytes > 0);
  b2.Close();

  std::puts("lazy_bitmaps_test_db: OK");
  return 0;
}
