#include "blob_source.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "cache/cache_key.h"
#include "file/random_access_file_reader.h"
#include "rocksdb/advanced_cache.h"
#include "table/block_based/block_based_table_reader.h"

namespace experiment {
using namespace rocksdb;

namespace {

std::atomic<uint64_t> g_page_hits{0};
std::atomic<uint64_t> g_page_misses{0};
std::atomic<uint64_t> g_bytes_read{0};
std::atomic<uint64_t> g_span_reads{0};
std::atomic<uint64_t> g_extent_hits{0};
std::atomic<uint64_t> g_extent_misses{0};
std::atomic<uint64_t> g_single_page_reads{0};
std::atomic<uint64_t> g_batch_calls{0};
std::atomic<uint64_t> g_batch_reads{0};

// Read requests carried by one MultiRead. Every request in a batch is
// submitted before any of them is waited on, so the batch IS the queue depth;
// the cap only bounds the transient scratch/aligned buffer RocksDB allocates
// for a call (RandomAccessFileReader::MultiRead allocates one buffer of the
// batch's total length under direct I/O). 128 x a few pages is a few hundred
// KB, and deeper than the 32-64 slots the data-block prefetch queue and
// SABI's span prefetch run at.
constexpr size_t kMaxBatchRequests = 128;

// A read touching at least this many pages takes the bulk span path. Two is
// strictly better than the per-page loop already: the cache lookups are the
// same, and any two adjacent misses become one pread instead of two.
constexpr uint32_t kMinSpanPages = 2;

// One cached blob page: a heap buffer the cache owns.
struct BlobPage {
  std::unique_ptr<char[]> data;
  size_t size = 0;
};

void DeleteBlobPage(Cache::ObjectPtr obj, MemoryAllocator* /*allocator*/) {
  delete static_cast<BlobPage*>(obj);
}

// Charged as an index block: these pages are index bytes, and the budget
// experiment reasons about index versus data residency. Same role BitLSM's
// on-demand bins use (sabi_reader.cpp SABICachedBinHelper).
const Cache::CacheItemHelper* BlobPageHelper() {
  static const Cache::CacheItemHelper helper(CacheEntryRole::kIndexBlock,
                                             &DeleteBlobPage);
  return &helper;
}

void DeleteCachedExtent(Cache::ObjectPtr obj, MemoryAllocator* /*allocator*/) {
  delete static_cast<CachedExtent*>(obj);
}

// Same role and priority as the pages: extents are index bytes competing in
// the same budget (and the same role BitLSM's on-demand bins carry).
const Cache::CacheItemHelper* CachedExtentHelper() {
  static const Cache::CacheItemHelper helper(CacheEntryRole::kIndexBlock,
                                             &DeleteCachedExtent);
  return &helper;
}

// EXP_SAI_LOCAL_EXTENT_MB, else the compile-time default. Function-local
// static so the env var is read once, before any cursor consults the cap.
std::atomic<uint64_t>& LocalExtentCapVar() {
  static std::atomic<uint64_t> cap{[] {
    if (const char* mb = std::getenv("EXP_SAI_LOCAL_EXTENT_MB")) {
      return static_cast<uint64_t>(std::atoll(mb)) << 20;
    }
    return kMaxLocalExtentBytes;
  }()};
  return cap;
}

// EXP_SAI_ASYNC_INDEX, else off. Function-local static so the env var is read
// once, before any cursor consults it.
std::atomic<bool>& AsyncIndexReadsVar() {
  static std::atomic<bool> on{[] {
    const char* e = std::getenv("EXP_SAI_ASYNC_INDEX");
    return e != nullptr && e[0] != '0';
  }()};
  return on;
}

// EXP_SAI_ASYNC_VERIFY: after every batch, re-read each request the ordinary
// synchronous way and compare. Proves the batched bytes are the pread bytes
// on real SST files, where the unit tests (which run over a MemBlobSource and
// never reach MultiRead) cannot. Off by default -- it doubles index I/O.
bool AsyncVerify() {
  static const bool on = [] {
    const char* e = std::getenv("EXP_SAI_ASYNC_VERIFY");
    return e != nullptr && e[0] != '0';
  }();
  return on;
}

}  // namespace

bool AsyncIndexReads() {
  return AsyncIndexReadsVar().load(std::memory_order_relaxed);
}
void SetAsyncIndexReadsForTest(bool on) {
  AsyncIndexReadsVar().store(on, std::memory_order_relaxed);
}

uint64_t LocalExtentCap() {
  return LocalExtentCapVar().load(std::memory_order_relaxed);
}
void SetLocalExtentCapForTest(uint64_t cap) {
  LocalExtentCapVar().store(cap, std::memory_order_relaxed);
}

BlobSourceStats GetBlobSourceStats() {
  return {g_page_hits.load(std::memory_order_relaxed),
          g_page_misses.load(std::memory_order_relaxed),
          g_bytes_read.load(std::memory_order_relaxed),
          g_span_reads.load(std::memory_order_relaxed),
          g_extent_hits.load(std::memory_order_relaxed),
          g_extent_misses.load(std::memory_order_relaxed),
          g_single_page_reads.load(std::memory_order_relaxed),
          g_batch_calls.load(std::memory_order_relaxed),
          g_batch_reads.load(std::memory_order_relaxed)};
}

void ResetBlobSourceStats() {
  g_page_hits.store(0, std::memory_order_relaxed);
  g_page_misses.store(0, std::memory_order_relaxed);
  g_bytes_read.store(0, std::memory_order_relaxed);
  g_span_reads.store(0, std::memory_order_relaxed);
  g_extent_hits.store(0, std::memory_order_relaxed);
  g_extent_misses.store(0, std::memory_order_relaxed);
  g_single_page_reads.store(0, std::memory_order_relaxed);
  g_batch_calls.store(0, std::memory_order_relaxed);
  g_batch_reads.store(0, std::memory_order_relaxed);
}

PinnedExtent BlobSource::FetchExtent(uint32_t rel_off, uint32_t len) {
  if (len == 0 || LocalExtentCap() == 0) return {};
  auto extent = std::make_unique<CachedExtent>();
  extent->bytes = std::make_unique<char[]>(len);
  extent->len = len;
  std::string scratch;
  const char* p = Read(rel_off, len, scratch);
  if (!ok()) return {};
  std::memcpy(extent->bytes.get(), p, len);
  return PinnedExtent(std::move(extent));
}

FileBlobSource::FileBlobSource(const BlockBasedTable* table)
    : table_(table),
      blob_offset_(table->get_rep()->udi_handle.offset()),
      blob_size_(table->get_rep()->udi_handle.size()) {}

bool FileBlobSource::ReadFromPage(uint64_t data_begin, uint32_t page_len,
                                  uint32_t in_page, uint32_t n, char* dst) {
  const auto* rep = table_->get_rep();
  // no_block_cache=true (or a caller-supplied null block_cache) leaves this
  // null; degrade to uncached reads below rather than crash (same guard as
  // BitLSM's SABIReader::Bin).
  Cache* cache = rep->table_options.block_cache.get();
  // Same mint as BitLSM's on-demand bins: the file's own cache-key base plus
  // the page's file offset. Every reader of this file derives the identical
  // key, so pages are shared without any registry of our own. Keyed by
  // data_begin, not the aligned-down page boundary: the first page's
  // aligned-down boundary lies BEFORE the blob and could equal a data
  // block's start offset (same >>2 key space), a type-confusing collision.
  // data_begin is clamped inside the UDI extent, which no other block
  // occupies, and distinct pages' data_begins never share a >>2 bucket, so
  // keys stay unique.
  const CacheKey key = rep->base_cache_key.WithOffset(data_begin >> 2);

  if (cache != nullptr) {
    if (Cache::Handle* h = cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr)) {
      auto* page = static_cast<BlobPage*>(cache->Value(h));
      assert(page->size == page_len);
      std::memcpy(dst, page->data.get() + in_page, n);
      cache->Release(h);
      g_page_hits.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  auto page = std::make_unique<BlobPage>();
  page->data = std::make_unique<char[]>(page_len);
  page->size = page_len;
  Slice result;
  // One read of the page's blob extent. The extent lies inside a single
  // aligned 4 KB window by construction, and RandomAccessFileReader::Read
  // realigns offset and length itself under direct I/O, so this is one
  // device-page read, never a straddle.
  IOStatus s = rep->file->Read(IOOptions(), data_begin, page_len, &result,
                               page->data.get(), /*aligned_buf=*/nullptr);
  if (!s.ok() || result.size() < page_len) {
    ok_ = false;
    return false;
  }
  // A direct-I/O read can hand back its own aligned buffer rather than the
  // scratch we passed, so copy from where the result actually points.
  if (result.data() != page->data.get()) {
    std::memcpy(page->data.get(), result.data(), result.size());
  }
  std::memcpy(dst, page->data.get() + in_page, n);

  g_page_misses.fetch_add(1, std::memory_order_relaxed);
  g_bytes_read.fetch_add(page_len, std::memory_order_relaxed);
  // ReadFromPage is only ever reached from the per-page loop in Read(), which
  // only runs for single-page requests (multi-page requests take ReadSpan
  // instead) -- so every miss here is one device pread issued by the
  // non-bulk path.
  g_single_page_reads.fetch_add(1, std::memory_order_relaxed);

  if (cache != nullptr) {
    // Insert without taking a handle: the bytes are already copied out, and a
    // refused insert (strict capacity) only costs a re-read next time.
    // High priority, same as RocksDB's own convention for index/filter blocks
    // (cache_index_and_filter_blocks_with_high_priority): index pages should
    // not be evicted by streaming data-block traffic. BitLSM's on-demand bin
    // entries use the same priority, keeping the treatment symmetric.
    BlobPage* raw = page.release();
    Status is = cache->Insert(key.AsSlice(), raw, BlobPageHelper(),
                              sizeof(BlobPage) + raw->size,
                              /*handle=*/nullptr, Cache::Priority::HIGH);
    if (!is.ok()) delete raw;
  }
  return true;
}

const char* FileBlobSource::Read(uint32_t rel_off, uint32_t len,
                                 std::string& scratch) {
  scratch.resize(len);
  if (len == 0) return scratch.data();
  if (static_cast<uint64_t>(rel_off) + len > blob_size_) {
    ok_ = false;
    return scratch.data();
  }
  {
    // Multi-page reads go bulk: one pread per run of missing pages instead of
    // one per page. Same coalescing BitLSM's on-demand LoadRun applies to its
    // bins; page-cache granularity and keys are unchanged.
    const uint64_t abs = blob_offset_ + rel_off;
    const uint64_t first_page = abs & ~static_cast<uint64_t>(kBlobPageSize - 1);
    const uint64_t last_page =
        (abs + len - 1) & ~static_cast<uint64_t>(kBlobPageSize - 1);
    if (last_page - first_page >=
        static_cast<uint64_t>(kMinSpanPages - 1) * kBlobPageSize) {
      return ReadSpan(rel_off, len, scratch);
    }
  }
  const uint64_t blob_end = blob_offset_ + blob_size_;
  uint32_t done = 0;
  while (done < len) {
    const uint64_t abs = blob_offset_ + rel_off + done;  // FILE offset
    // File-anchored page grid: the boundary is the enclosing aligned 4 KB
    // window, regardless of where the blob starts. The first and last pages
    // of a blob are short -- bytes outside [blob_offset_, blob_end) belong
    // to other blocks (or the block trailer) and are never cached here.
    const uint64_t page_off = abs & ~static_cast<uint64_t>(kBlobPageSize - 1);
    const uint64_t data_begin = std::max(page_off, blob_offset_);
    const uint64_t data_end = std::min(page_off + kBlobPageSize, blob_end);
    const uint32_t page_len = static_cast<uint32_t>(data_end - data_begin);
    const uint32_t in_page = static_cast<uint32_t>(abs - data_begin);
    const uint32_t n = std::min(len - done, page_len - in_page);
    if (!ReadFromPage(data_begin, page_len, in_page, n,
                      scratch.data() + done)) {
      return scratch.data();
    }
    done += n;
  }
  return scratch.data();
}

const char* FileBlobSource::ReadSpan(uint32_t rel_off, uint32_t len,
                                     std::string& scratch) {
  const auto* rep = table_->get_rep();
  Cache* cache = rep->table_options.block_cache.get();
  const uint64_t blob_end = blob_offset_ + blob_size_;
  const uint64_t abs_begin = blob_offset_ + rel_off;
  const uint64_t abs_end = abs_begin + len;

  // Walk the extent's pages on the same file-anchored grid as the per-page
  // path. Cached pages are copied out immediately (never re-read from disk);
  // runs of consecutive missing pages are accumulated and fetched with one
  // pread per run.
  bool in_stretch = false;
  uint64_t stretch_begin = 0, stretch_end = 0;
  for (uint64_t page_off =
           abs_begin & ~static_cast<uint64_t>(kBlobPageSize - 1);
       page_off < abs_end; page_off += kBlobPageSize) {
    const uint64_t data_begin = std::max(page_off, blob_offset_);
    const uint64_t data_end = std::min(page_off + kBlobPageSize, blob_end);
    bool hit = false;
    if (cache != nullptr) {
      const CacheKey key = rep->base_cache_key.WithOffset(data_begin >> 2);
      if (Cache::Handle* h =
              cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr)) {
        auto* page = static_cast<BlobPage*>(cache->Value(h));
        assert(page->size == data_end - data_begin);
        const uint64_t cb = std::max(data_begin, abs_begin);
        const uint64_t ce = std::min(data_end, abs_end);
        std::memcpy(scratch.data() + (cb - abs_begin),
                    page->data.get() + (cb - data_begin), ce - cb);
        cache->Release(h);
        g_page_hits.fetch_add(1, std::memory_order_relaxed);
        hit = true;
      }
    }
    if (hit) {
      if (in_stretch) {
        in_stretch = false;
        if (!ReadStretch(stretch_begin, stretch_end, abs_begin, abs_end,
                         scratch.data())) {
          return scratch.data();
        }
      }
    } else {
      if (!in_stretch) {
        in_stretch = true;
        stretch_begin = data_begin;
      }
      stretch_end = data_end;
    }
  }
  if (in_stretch) {
    ReadStretch(stretch_begin, stretch_end, abs_begin, abs_end,
                scratch.data());
  }
  return scratch.data();
}

bool FileBlobSource::ReadStretch(uint64_t data_begin, uint64_t data_end,
                                 uint64_t abs_begin, uint64_t abs_end,
                                 char* out) {
  const auto* rep = table_->get_rep();
  Cache* cache = rep->table_options.block_cache.get();
  const size_t n = static_cast<size_t>(data_end - data_begin);
  auto buf = std::make_unique<char[]>(n);
  Slice result;
  // RandomAccessFileReader::Read realigns offset and length itself under
  // direct I/O, so a stretch starting or ending mid-page is still fine.
  IOStatus s = rep->file->Read(IOOptions(), data_begin, n, &result, buf.get(),
                               /*aligned_buf=*/nullptr);
  if (!s.ok() || result.size() < n) {
    ok_ = false;
    return false;
  }
  // A direct-I/O read can hand back its own buffer rather than the scratch
  // we passed, so read from where the result actually points.
  const char* bytes = result.data();
  g_span_reads.fetch_add(1, std::memory_order_relaxed);
  g_bytes_read.fetch_add(n, std::memory_order_relaxed);

  // Chunk the stretch back into the identical data_begin-keyed pages the
  // per-page path would have cached, so cross-query reuse granularity is
  // unchanged. A refused insert (strict capacity) just skips caching that
  // page; the caller already has its bytes.
  const uint64_t blob_end = blob_offset_ + blob_size_;
  for (uint64_t page_off =
           data_begin & ~static_cast<uint64_t>(kBlobPageSize - 1);
       page_off < data_end; page_off += kBlobPageSize) {
    const uint64_t pb = std::max(page_off, blob_offset_);
    const uint64_t pe = std::min(page_off + kBlobPageSize, blob_end);
    const uint32_t page_len = static_cast<uint32_t>(pe - pb);
    const char* page_bytes = bytes + (pb - data_begin);
    const uint64_t cb = std::max(pb, abs_begin);
    const uint64_t ce = std::min(pe, abs_end);
    if (cb < ce) {
      std::memcpy(out + (cb - abs_begin), page_bytes + (cb - pb), ce - cb);
    }
    g_page_misses.fetch_add(1, std::memory_order_relaxed);
    if (cache != nullptr) {
      auto page = std::make_unique<BlobPage>();
      page->data = std::make_unique<char[]>(page_len);
      page->size = page_len;
      std::memcpy(page->data.get(), page_bytes, page_len);
      const CacheKey key = rep->base_cache_key.WithOffset(pb >> 2);
      BlobPage* raw = page.release();
      Status is = cache->Insert(key.AsSlice(), raw, BlobPageHelper(),
                                sizeof(BlobPage) + raw->size,
                                /*handle=*/nullptr, Cache::Priority::HIGH);
      if (!is.ok()) delete raw;
    }
  }
  return true;
}

void FileBlobSource::PrefetchRanges(const BlobRange* ranges, size_t n) {
  if (ranges == nullptr || n == 0 || !AsyncIndexReads()) return;
  const auto* rep = table_->get_rep();
  Cache* cache = rep->table_options.block_cache.get();
  const uint64_t blob_end = blob_offset_ + blob_size_;

  // 1. The caller's ranges, expanded onto the SAME file-anchored 4 KB grid
  // Read()/ReadFromPage() use, then sorted and deduplicated: overlapping
  // ranges (two leaves sharing a page, a leaf's tail slack reaching into the
  // next leaf) must not become two requests for the same bytes.
  std::vector<uint64_t> pages;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t rel = ranges[i].rel_off, len = ranges[i].len;
    if (len == 0 || static_cast<uint64_t>(rel) + len > blob_size_) continue;
    const uint64_t abs_begin = blob_offset_ + rel;
    const uint64_t abs_end = abs_begin + len;
    for (uint64_t p = abs_begin & ~static_cast<uint64_t>(kBlobPageSize - 1);
         p < abs_end; p += kBlobPageSize) {
      pages.push_back(p);
    }
  }
  if (pages.empty()) return;
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

  // 2. Drop the pages the cache already holds -- the point of batching is the
  // MISSING ones, and re-reading a resident page would inflate both the byte
  // count and the budget's eviction pressure. The lookup is deliberately not
  // counted as a hit: the read that follows this prefetch counts it, exactly
  // as it would have without the prefetch.
  std::vector<PageRun> runs;
  PageRun cur{0, 0};
  bool in_run = false;
  for (uint64_t page_off : pages) {
    // data_begin/data_end are the page's identity in the cache, clamped to
    // the blob the same way every other path clamps them: bytes outside
    // [blob_offset_, blob_end) belong to other blocks.
    const uint64_t data_begin = std::max(page_off, blob_offset_);
    const uint64_t data_end = std::min(page_off + kBlobPageSize, blob_end);
    bool cached = false;
    if (cache != nullptr) {
      const CacheKey key = rep->base_cache_key.WithOffset(data_begin >> 2);
      if (Cache::Handle* h =
              cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr)) {
        cache->Release(h);
        cached = true;
      }
    }
    if (cached) {
      in_run = false;
      continue;
    }
    // Consecutive missing pages become ONE request, the same coalescing
    // ReadSpan applies within a single read; a cached page in the middle
    // breaks the run rather than being re-read.
    if (in_run && cur.second == data_begin) {
      cur.second = data_end;
    } else {
      if (in_run) runs.push_back(cur);
      cur = {data_begin, data_end};
      in_run = true;
    }
    if (runs.size() >= kMaxBatchRequests) SubmitBatch(runs);
  }
  if (in_run) runs.push_back(cur);
  SubmitBatch(runs);
}

void FileBlobSource::SubmitBatch(std::vector<PageRun>& runs) {
  if (runs.empty()) return;
  const auto* rep = table_->get_rep();
  size_t total = 0;
  for (const PageRun& r : runs) {
    total += static_cast<size_t>(r.second - r.first);
  }
  // One scratch block for the whole batch. Under direct I/O
  // RandomAccessFileReader::MultiRead ignores it and aligns/merges the
  // requests into an internally allocated aligned buffer instead (results
  // point there); under buffered reads this is where the data lands.
  std::unique_ptr<char[]> scratch(new char[total]);
  std::vector<FSReadRequest> reqs(runs.size());
  size_t at = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    reqs[i].offset = runs[i].first;
    reqs[i].len = static_cast<size_t>(runs[i].second - runs[i].first);
    reqs[i].scratch = scratch.get() + at;
    at += reqs[i].len;
  }
  // MultiRead requires increasing, non-overlapping offsets: guaranteed, the
  // runs came out of a sorted deduplicated page list.
  AlignedBuf aligned;
  IOStatus s = rep->file->MultiRead(IOOptions(), reqs.data(), reqs.size(),
                                    &aligned);
  g_batch_calls.fetch_add(1, std::memory_order_relaxed);
  g_batch_reads.fetch_add(reqs.size(), std::memory_order_relaxed);
  if (s.ok()) {
    for (size_t i = 0; i < reqs.size(); ++i) {
      // A short or failed sub-request just goes uncached; the serial read
      // that wanted those bytes will issue its own pread and surface the
      // error there. ok_ is deliberately NOT set here -- a prefetch is a
      // hint, and failing one must not turn a healthy query into an abort.
      if (!reqs[i].status.ok() || reqs[i].result.size() < reqs[i].len) continue;
      g_bytes_read.fetch_add(reqs[i].len, std::memory_order_relaxed);
      if (AsyncVerify()) VerifyRequest(reqs[i]);
      CacheRunPages(reqs[i].offset, reqs[i].offset + reqs[i].len,
                    reqs[i].result.data());
    }
  }
  runs.clear();
}

void FileBlobSource::VerifyRequest(const rocksdb::FSReadRequest& req) {
  const auto* rep = table_->get_rep();
  std::unique_ptr<char[]> buf(new char[req.len]);
  Slice got;
  IOStatus s = rep->file->Read(IOOptions(), req.offset, req.len, &got,
                               buf.get(), /*aligned_buf=*/nullptr);
  if (!s.ok() || got.size() != req.len ||
      std::memcmp(got.data(), req.result.data(), req.len) != 0) {
    std::fprintf(stderr,
                 "[blob_source] BATCH MISMATCH at offset %llu len %zu\n",
                 static_cast<unsigned long long>(req.offset), req.len);
    std::abort();
  }
}

void FileBlobSource::CacheRunPages(uint64_t run_begin, uint64_t run_end,
                                   const char* bytes) {
  const auto* rep = table_->get_rep();
  Cache* cache = rep->table_options.block_cache.get();
  const uint64_t blob_end = blob_offset_ + blob_size_;
  // Identical chunking, keys, charge and priority to ReadStretch's: a page
  // this batch cached is indistinguishable from one a pread cached, which is
  // what keeps the budget accounting and the cross-query reuse granularity
  // the same in both modes.
  for (uint64_t page_off =
           run_begin & ~static_cast<uint64_t>(kBlobPageSize - 1);
       page_off < run_end; page_off += kBlobPageSize) {
    const uint64_t pb = std::max(page_off, blob_offset_);
    const uint64_t pe = std::min(page_off + kBlobPageSize, blob_end);
    if (pb >= pe) continue;
    const uint32_t page_len = static_cast<uint32_t>(pe - pb);
    g_page_misses.fetch_add(1, std::memory_order_relaxed);
    if (cache == nullptr) continue;
    auto page = std::make_unique<BlobPage>();
    page->data = std::make_unique<char[]>(page_len);
    page->size = page_len;
    std::memcpy(page->data.get(), bytes + (pb - run_begin), page_len);
    const CacheKey key = rep->base_cache_key.WithOffset(pb >> 2);
    BlobPage* raw = page.release();
    Status is = cache->Insert(key.AsSlice(), raw, BlobPageHelper(),
                              sizeof(BlobPage) + raw->size,
                              /*handle=*/nullptr, Cache::Priority::HIGH);
    if (!is.ok()) delete raw;
  }
}

PinnedExtent FileBlobSource::FetchExtent(uint32_t rel_off, uint32_t len) {
  const uint64_t cap = LocalExtentCap();
  if (len == 0 || cap == 0) return {};  // cap 0: extent fetching disabled
  if (static_cast<uint64_t>(rel_off) + len > blob_size_) {
    ok_ = false;
    return {};
  }
  const auto* rep = table_->get_rep();
  Cache* cache = rep->table_options.block_cache.get();
  const uint64_t abs = blob_offset_ + rel_off;  // FILE offset of the extent
  // Key mint -- collision-critical. Extents live in the SAME file-offset
  // space as the page keys above (data_begin >> 2), and an extent's start
  // CAN coincide with a page's data_begin (a posting list opening exactly at
  // a page boundary, or at the blob's clamped first page): a raw
  // WithOffset(abs >> 2) would type-confuse a CachedExtent with a BlobPage.
  // So extent keys are tagged out of every other keyspace with bit 63: real
  // file offsets never reach 2^63 (nor do RocksDB's own block-offset keys or
  // our >>2 page keys), so a tagged key collides with neither. Two distinct
  // extents can't share a key either: extent starts are starts of distinct
  // index structures at least 4 bytes apart (a posting list is >= 21 bytes,
  // a leaf record >= 10, the summary array >= 32), so their >>2 buckets
  // differ.
  const CacheKey key =
      rep->base_cache_key.WithOffset((abs >> 2) | (1ULL << 63));
  // Over-cap extents bypass the cache entirely (uncached read, pin-owned
  // buffer): unreachable at this workload's extent sizes, but a degenerate
  // extent must not wipe the cache.
  // Gate on the cache's own capacity as well as the absolute cap: with no
  // EXP_BLOCK_CACHE_MB override RocksDB materializes a 32 MB default cache,
  // where a multi-MB HIGH-priority extent would wipe or bypass it (and the
  // default HyperClockCache does not guarantee duplicate-key replacement,
  // which the len>=  hit guard relies on). Capacity/16 is 32 MB at the 512 MB
  // budget floor, above every observed extent, so measured cells are
  // unaffected.
  const bool cacheable =
      cache != nullptr &&
      len <= std::min<uint64_t>(cap, cache->GetCapacity() / 16);

  if (cacheable) {
    if (Cache::Handle* h =
            cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr)) {
      auto* extent = static_cast<CachedExtent*>(cache->Value(h));
      // Extents sharing a start can differ in length: a per-range extent
      // starts at its first interior leaf's postings, so two query ranges
      // sharing that leaf but ending differently mint the same key with
      // different lengths. A longer cached extent serves any shorter
      // request (same file bytes); a shorter one cannot -- release it and
      // reload below (the insert replaces the entry; pins on the old one
      // keep it alive until they release).
      if (extent->len >= len) {
        g_extent_hits.fetch_add(1, std::memory_order_relaxed);
        return PinnedExtent(cache, h, extent->bytes.get(), extent->len);
      }
      cache->Release(h);
    }
  }

  // Miss: ONE pread of the whole extent, straight into the entry's buffer.
  // Deliberately NOT the page path -- per-page entries are not populated for
  // these bytes; the extent entry replaces them wholesale, and chunking a
  // copy into pages would only duplicate the bytes in cache.
  // RandomAccessFileReader::Read realigns offset and length itself under
  // direct I/O, so an unaligned extent is still one clean read.
  auto extent = std::make_unique<CachedExtent>();
  extent->bytes = std::make_unique<char[]>(len);
  extent->len = len;
  Slice result;
  IOStatus s = rep->file->Read(IOOptions(), abs, len, &result,
                               extent->bytes.get(), /*aligned_buf=*/nullptr);
  if (!s.ok() || result.size() < len) {
    ok_ = false;
    return {};
  }
  // A direct-I/O read can hand back its own aligned buffer rather than the
  // scratch we passed, so copy from where the result actually points.
  if (result.data() != extent->bytes.get()) {
    std::memcpy(extent->bytes.get(), result.data(), len);
  }
  g_extent_misses.fetch_add(1, std::memory_order_relaxed);
  g_bytes_read.fetch_add(len, std::memory_order_relaxed);

  if (cacheable) {
    // Insert WITH a handle: the caller is about to decode from these bytes,
    // so the entry must stay pinned until its pin releases. HIGH priority,
    // same convention as the pages and BitLSM's bins. A refused insert
    // (strict capacity) falls through to the owned pin -- the extent then
    // lives exactly as long as its pin, and only costs a re-read next time.
    Cache::Handle* h = nullptr;
    CachedExtent* raw = extent.get();
    Status is = cache->Insert(key.AsSlice(), raw, CachedExtentHelper(),
                              sizeof(CachedExtent) + len, &h,
                              Cache::Priority::HIGH);
    if (is.ok()) {
      extent.release();  // cache owns it now; the handle keeps it pinned
      return PinnedExtent(cache, h, raw->bytes.get(), raw->len);
    }
  }
  return PinnedExtent(std::move(extent));
}

}  // namespace experiment
