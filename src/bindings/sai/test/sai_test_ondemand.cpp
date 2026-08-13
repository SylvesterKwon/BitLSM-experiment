// Pins the on-demand readers to the resident ones: both run over the same
// blob bytes and must agree on every estimate and every posting set. The
// on-demand path duplicates the decode logic (it fetches through a
// SAIBlobSource rather than dereferencing a materialised blob), and this test
// is what keeps the two implementations from drifting.
// Run: ./build/bin/sai_test_ondemand
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "sai_blob_source.h"
#include "sai_index.h"
#include "sai_index_registry.h"
#include "sai_ondemand.h"
#include "sai_value_codec.h"

using namespace experiment::sai;

static std::vector<uint32_t> Drain(RowCursor* c) {
  std::vector<uint32_t> got;
  if (c)
    while (c->Valid()) {
      got.push_back(c->Row());
      c->Next();
    }
  return got;
}

int main() {
  const double inf = std::numeric_limits<double>::infinity();
  bit_lsm::BitLSMOptions opts;
  opts.attr_num = 2;
  opts.attr_specs = {bit_lsm::AttrSpec(bit_lsm::UNORDERED),
                     bit_lsm::AttrSpec(bit_lsm::ORDERED)};

  // Vocabulary of 40 terms so the trie has real depth and fan-out, and 40k
  // rows so posting lists span many 128-posting blocks and the numeric index
  // spans many 1024-value leaves -- the cases where on-demand reads differ
  // most from a single materialised buffer.
  std::vector<std::string> vocab;
  for (int i = 0; i < 40; ++i)
    vocab.push_back("term_" + std::to_string(i) + (i % 3 ? "x" : "yy"));

  const uint32_t N = 40000, PER_BLOCK = 100;
  std::mt19937 gen(7);
  std::vector<std::string> cats;
  std::vector<double> conts;

  SAIIndexBuilder builder(opts);
  std::string vbuf;
  for (uint32_t i = 0; i < N; ++i) {
    cats.push_back(vocab[gen() % vocab.size()]);
    conts.push_back(static_cast<double>(gen() % 100000) / 10.0);
    std::vector<Attr> attrs = {cats.back(), conts.back()};
    SAICodec::Encode(opts, attrs, "payload", vbuf);
    builder.OnKeyAdded(rocksdb::Slice("k"),
                       rocksdb::UserDefinedIndexBuilder::kValue,
                       rocksdb::Slice(vbuf));
    if ((i + 1) % PER_BLOCK == 0) {
      rocksdb::UserDefinedIndexBuilder::BlockHandle bh{(i / PER_BLOCK) * 4096,
                                                       4096};
      std::string scratch;
      builder.AddIndexEntry(rocksdb::Slice("k"), nullptr, bh, &scratch);
    }
  }
  rocksdb::Slice blob;
  assert(builder.Finish(&blob).ok());

  SAIIndexReader resident(blob, opts);

  SAIMemBlobSource src(blob.data(), blob.size());
  SAIFileDirectory dir;
  assert(BuildDirectoryFromSource(src, &dir));
  SAIIndexOnDemand ondemand(&dir, &src);

  // 1. Directory matches what the resident reader parsed.
  assert(dir.entries_total == resident.EntriesTotal());
  assert(dir.entry_count_psum == resident.entry_count_psum);
  assert(dir.block_handles.size() == resident.block_handles.size());
  for (size_t i = 0; i < dir.block_handles.size(); ++i) {
    assert(dir.block_handles[i].offset == resident.block_handles[i].offset);
    assert(dir.block_handles[i].size == resident.block_handles[i].size);
  }
  for (uint32_t row : {0u, 1u, 99u, 100u, N / 2, N - 1}) {
    uint32_t b1, o1, b2, o2;
    resident.Locate(row, b1, o1);
    dir.Locate(row, b2, o2);
    assert(b1 == b2 && o1 == o2);
  }

  // 2. Categorical: same counts and same posting sets, present or absent.
  for (const std::string& t : vocab) {
    SAIFact f;
    f.is_cat = true;
    f.attr_idx = 0;
    f.cat_value = t;
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < N; ++i)
      if (cats[i] == t) want.push_back(i);
    assert(ondemand.Estimate(f) == want.size());
    assert(Drain(ondemand.OpenCursor(f).get()) == want);
    assert(ondemand.Estimate(f) == resident.Estimate(f));
  }
  for (const char* absent : {"zz", "term_", "term_999", ""}) {
    SAIFact f;
    f.is_cat = true;
    f.attr_idx = 0;
    f.cat_value = absent;
    assert(ondemand.Estimate(f) == 0);
    assert(ondemand.OpenCursor(f) == nullptr);
    assert(resident.OpenCursor(f) == nullptr);
  }

  // 3. Numeric: fuzz ranges against the oracle and against the resident reader.
  for (int t = 0; t < 200; ++t) {
    SAIFact f;
    f.attr_idx = 1;
    f.lo = static_cast<double>(gen() % 100000) / 10.0;
    f.hi = f.lo + static_cast<double>(gen() % 20000) / 10.0;
    f.lo_inc = gen() % 2;
    f.hi_inc = gen() % 2;
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < N; ++i) {
      const bool lo_ok = f.lo_inc ? conts[i] >= f.lo : conts[i] > f.lo;
      const bool hi_ok = f.hi_inc ? conts[i] <= f.hi : conts[i] < f.hi;
      if (lo_ok && hi_ok) want.push_back(i);
    }
    assert(Drain(ondemand.OpenCursor(f).get()) == want);
    assert(ondemand.Estimate(f) == resident.Estimate(f));
  }
  {
    SAIFact f;
    f.attr_idx = 1;
    f.lo = -inf;
    f.hi = inf;
    assert(ondemand.Estimate(f) == N);
  }

  // 4. AdvanceTo: skipping through a long posting list must land on the same
  // rows as a linear walk. This is the path that reads the skip table.
  {
    SAIFact f;
    f.is_cat = true;
    f.attr_idx = 0;
    f.cat_value = vocab[0];
    std::vector<uint32_t> all = Drain(ondemand.OpenCursor(f).get());
    assert(!all.empty());
    auto cur = ondemand.OpenCursor(f);
    for (size_t i = 0; i < all.size(); i += 97) {
      cur->AdvanceTo(all[i]);
      assert(cur->Valid() && cur->Row() == all[i]);
    }
    // A target past the last posting exhausts the cursor.
    auto tail = ondemand.OpenCursor(f);
    tail->AdvanceTo(all.back() + 1);
    assert(!tail->Valid());
  }

  std::printf("sai_test_ondemand OK\n");
  return 0;
}
