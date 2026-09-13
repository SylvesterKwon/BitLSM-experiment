// Pins the on-demand readers to the resident ones: both run over the same
// blob bytes and must agree on every estimate and every posting set. The
// on-demand path duplicates the decode logic (it fetches through a
// BlobSource rather than dereferencing a materialised blob), and this test
// is what keeps the two implementations from drifting. It also pins the
// metadata-only reader's contract: everything it serves must be an owned
// copy, so the raw blob can be freed (and here: scribbled) after
// construction. Run: ./build/bin/sai_test_ondemand
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "blob_source.h"
#include "sai_index.h"
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
  opts.attr_specs = {bit_lsm::AttrSpec(bit_lsm::IndexType::kEquality, bit_lsm::PhysicalType::kVarBinary, 0),
                     bit_lsm::AttrSpec(bit_lsm::IndexType::kRange, bit_lsm::PhysicalType::kFloat, 8)};

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
  assert(resident.RetainsIndexContents());

  // Metadata-only reader over a DOOMED copy of the blob: constructed, then
  // the copy is scribbled and freed, the way RocksDB frees the raw block
  // right after construction on the metadata path. Everything the reader
  // serves afterwards must come from owned copies.
  std::vector<char> doomed(blob.data(), blob.data() + blob.size());
  SAIIndexReader* meta;
  {
    rocksdb::Slice doomed_slice(doomed.data(), doomed.size());
    meta = new SAIIndexReader(doomed_slice, opts, /*metadata_only=*/true);
  }
  assert(!meta->RetainsIndexContents());
  std::memset(doomed.data(), 0xAB, doomed.size());
  doomed.clear();
  doomed.shrink_to_fit();

  // 1. Directory matches what the resident reader parsed, after the blob the
  // metadata reader was built over is gone.
  assert(meta->EntriesTotal() == resident.EntriesTotal());
  assert(meta->entry_count_psum == resident.entry_count_psum);
  assert(meta->RegionOffsets() == resident.RegionOffsets());
  assert(meta->block_handles.size() == resident.block_handles.size());
  for (size_t i = 0; i < meta->block_handles.size(); ++i) {
    assert(meta->block_handles[i].offset == resident.block_handles[i].offset);
    assert(meta->block_handles[i].size == resident.block_handles[i].size);
  }
  for (uint32_t row : {0u, 1u, 99u, 100u, N / 2, N - 1}) {
    uint32_t b1, o1, b2, o2;
    resident.Locate(row, b1, o1);
    meta->Locate(row, b2, o2);
    assert(b1 == b2 && o1 == o2);
  }
  delete meta;

  // On-demand decode over the live blob, driven through the same fact-level
  // entry points the metadata reader routes to (with a memory source standing
  // in for the SST file).
  // Wraps the memory source so the async pass can check what the readers
  // ANNOUNCE through BlobSource::PrefetchRanges. A hint must never name bytes
  // outside the blob (FileBlobSource would turn such a range into a read past
  // the UDI extent, into another block's bytes), and the pass is only
  // meaningful if the readers actually emit hints -- both are asserted.
  class CheckedMemBlobSource : public experiment::MemBlobSource {
   public:
    using experiment::MemBlobSource::MemBlobSource;
    void PrefetchRanges(const experiment::BlobRange* r, size_t n) override {
      assert(r != nullptr && n > 0);
      for (size_t i = 0; i < n; ++i) {
        assert(r[i].len > 0);
        assert(static_cast<uint64_t>(r[i].rel_off) + r[i].len <= BlobSize());
      }
      announced += n;
    }
    uint64_t announced = 0;
  };
  CheckedMemBlobSource src(blob.data(), blob.size());
  const std::vector<uint32_t>& region_off = resident.RegionOffsets();
  auto od_estimate = [&](const SAIFact& f) {
    return OnDemandEstimate(src, region_off, f);
  };
  auto od_cursor = [&](const SAIFact& f) {
    return OnDemandOpenCursor(&src, region_off, f);
  };

  // Sections 2-4 run twice: once with the default cursor-local extent cap
  // (bulk-fetched posting lists / summary arrays / boundary-leaf windows) and
  // once with the cap forced to 0, which drives every cursor down the
  // per-block fallback paths. The fuzzer is reseeded at the top of each pass
  // so both passes cover literally identical ranges; both must agree with the
  // oracle and the resident reader.
  auto run_suite = [&]() {
  gen.seed(1234);
  // 2. Categorical: same counts and same posting sets, present or absent.
  for (const std::string& t : vocab) {
    SAIFact f;
    f.is_cat = true;
    f.attr_idx = 0;
    f.cat_value = t;
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < N; ++i)
      if (cats[i] == t) want.push_back(i);
    assert(od_estimate(f) == want.size());
    assert(Drain(od_cursor(f).get()) == want);
    assert(od_estimate(f) == resident.Estimate(f));
    assert(src.ok());
  }
  for (const char* absent : {"zz", "term_", "term_999", ""}) {
    SAIFact f;
    f.is_cat = true;
    f.attr_idx = 0;
    f.cat_value = absent;
    assert(od_estimate(f) == 0);
    assert(od_cursor(f) == nullptr);
    assert(resident.OpenCursor(f) == nullptr);
    assert(src.ok());  // absent is a clean miss, not a read error
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
    assert(Drain(od_cursor(f).get()) == want);
    assert(od_estimate(f) == resident.Estimate(f));
    assert(src.ok());
  }
  {
    SAIFact f;
    f.attr_idx = 1;
    f.lo = -inf;
    f.hi = inf;
    assert(od_estimate(f) == N);
  }

  // 4. AdvanceTo: skipping through a long posting list must land on the same
  // rows as a linear walk. This is the path that reads the skip table.
  {
    SAIFact f;
    f.is_cat = true;
    f.attr_idx = 0;
    f.cat_value = vocab[0];
    std::vector<uint32_t> all = Drain(od_cursor(f).get());
    assert(!all.empty());
    auto cur = od_cursor(f);
    for (size_t i = 0; i < all.size(); i += 97) {
      cur->AdvanceTo(all[i]);
      assert(cur->Valid() && cur->Row() == all[i]);
    }
    // A target past the last posting exhausts the cursor.
    auto tail = od_cursor(f);
    tail->AdvanceTo(all.back() + 1);
    assert(!tail->Valid());
    assert(src.ok());
  }
  };  // run_suite

  run_suite();  // cursor-local buffers (default cap)
  experiment::SetLocalExtentCapForTest(0);
  run_suite();  // per-block fallback everywhere
  // Cap 0 + batched submission (EXP_SAI_ASYNC_INDEX): the readers announce
  // their ranges up front, then read exactly as the pass above did. Announcing
  // is a hint, so the rows must not move by one -- that is what this pass
  // pins. The batching itself lives in FileBlobSource and needs a real SST;
  // it is cross-checked on the workload with EXP_SAI_ASYNC_VERIFY=1.
  experiment::SetAsyncIndexReadsForTest(true);
  const uint64_t announced_before = src.announced;
  run_suite();
  assert(src.announced > announced_before);  // the new path really ran
  experiment::SetAsyncIndexReadsForTest(false);
  experiment::SetLocalExtentCapForTest(experiment::kMaxLocalExtentBytes);

  std::printf("sai_test_ondemand OK\n");
  return 0;
}
