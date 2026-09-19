// Assert-based unit test for sai_index builder/reader driven WITHOUT RocksDB
// tables: feed encoded rows through the UDI callbacks directly, then query the
// parsed blob against a brute-force oracle. Run: ./build/bin/sai_test_index
#include "sai_index.h"
#include "sai_value_codec.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace experiment::sai;

static std::vector<uint32_t> Drain(RowCursor* c) {
  std::vector<uint32_t> got;
  if (c) while (c->Valid()) { got.push_back(c->Row()); c->Next(); }
  return got;
}

int main() {
  const double inf = std::numeric_limits<double>::infinity();
  bit_lsm::BitLSMOptions opts;
  opts.attr_num = 2;
  opts.attr_specs = {bit_lsm::AttrSpec(bit_lsm::IndexType::kEquality, bit_lsm::PhysicalType::kVarBinary, 0),
                     bit_lsm::AttrSpec(bit_lsm::IndexType::kRange, bit_lsm::PhysicalType::kFloat, 8)};

  const char* vocab[4] = {"aa", "bb", "cc", "dd"};
  const uint32_t N = 5000, PER_BLOCK = 100;
  std::mt19937 gen(42);
  std::vector<std::string> cats;
  std::vector<double> conts;

  SAIIndexBuilder builder(opts);
  const bit_lsm::ValueLayout layout(opts);
  std::string vbuf;
  for (uint32_t i = 0; i < N; ++i) {
    cats.push_back(vocab[gen() % 4]);
    conts.push_back(static_cast<double>(gen() % 10000) / 10.0);
    std::vector<Attr> attrs = {std::string(cats.back()), conts.back()};
    SAICodec::Encode(layout, attrs, "payload", vbuf);
    builder.OnKeyAdded(rocksdb::Slice("k"),
                       rocksdb::UserDefinedIndexBuilder::kValue,
                       rocksdb::Slice(vbuf));
    if ((i + 1) % PER_BLOCK == 0) {
      rocksdb::UserDefinedIndexBuilder::BlockHandle bh{(i / PER_BLOCK) * 4096, 4096};
      std::string scratch;
      builder.AddIndexEntry(rocksdb::Slice("k"), nullptr, bh, &scratch);
    }
  }
  rocksdb::Slice blob;
  assert(builder.Finish(&blob).ok());

  SAIIndexReader reader(blob, opts);
  // 1. Section A: 50 blocks, psum cumulative-through-block.
  assert(reader.entry_count_psum.size() == N / PER_BLOCK);
  assert(reader.entry_count_psum[0] == PER_BLOCK);
  assert(reader.entry_count_psum.back() == N);
  // 2. Locate: rowId -> (block, ordinal).
  uint32_t b, o;
  reader.Locate(0, b, o);    assert(b == 0 && o == 0);
  reader.Locate(99, b, o);   assert(b == 0 && o == 99);
  reader.Locate(100, b, o);  assert(b == 1 && o == 0);
  reader.Locate(4999, b, o); assert(b == 49 && o == 99);
  // 3. Categorical: exact counts + exact posting sets, absent term -> 0/nullptr.
  for (const char* t : vocab) {
    SAIFact f; f.is_cat = true; f.attr_idx = 0; f.cat_value = t;
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < N; ++i) if (cats[i] == t) want.push_back(i);
    assert(reader.Estimate(f) == want.size());
    assert(Drain(reader.OpenCursor(f).get()) == want);
  }
  {
    SAIFact f; f.is_cat = true; f.attr_idx = 0; f.cat_value = "zz";
    assert(reader.Estimate(f) == 0 && reader.OpenCursor(f) == nullptr);
  }
  // 4. Continuous: fuzz ranges vs oracle; estimate is an upper bound.
  for (int t = 0; t < 100; ++t) {
    SAIFact f; f.attr_idx = 1;
    f.lo = static_cast<double>(gen() % 10000) / 10.0;
    f.hi = f.lo + static_cast<double>(gen() % 3000) / 10.0;
    f.lo_inc = gen() % 2; f.hi_inc = gen() % 2;
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < N; ++i) {
      const bool lo_ok = f.lo_inc ? conts[i] >= f.lo : conts[i] > f.lo;
      const bool hi_ok = f.hi_inc ? conts[i] <= f.hi : conts[i] < f.hi;
      if (lo_ok && hi_ok) want.push_back(i);
    }
    assert(Drain(reader.OpenCursor(f).get()) == want);
    assert(reader.Estimate(f) >= want.size());
  }
  {  // full-range estimate exact
    SAIFact f; f.attr_idx = 1; f.lo = -inf; f.hi = inf;
    assert(reader.Estimate(f) == N);
  }
  std::printf("sai_test_index OK\n");
  return 0;
}
