#include "sai_blob_source.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>

#include "cache/cache_key.h"
#include "file/random_access_file_reader.h"
#include "rocksdb/advanced_cache.h"
#include "table/block_based/block_based_table_reader.h"

namespace experiment::sai {
using namespace rocksdb;

namespace {

std::atomic<uint64_t> g_page_hits{0};
std::atomic<uint64_t> g_page_misses{0};
std::atomic<uint64_t> g_bytes_read{0};

// One cached blob page: a heap buffer the cache owns.
struct SAIBlobPage {
  std::unique_ptr<char[]> data;
  size_t size = 0;
};

void DeleteSAIBlobPage(Cache::ObjectPtr obj, MemoryAllocator* /*allocator*/) {
  delete static_cast<SAIBlobPage*>(obj);
}

// Charged as an index block: these pages are index bytes, and the budget
// experiment reasons about index versus data residency. Same role BitLSM's
// on-demand bins use (sabi_reader.cpp SABICachedBinHelper).
const Cache::CacheItemHelper* SAIBlobPageHelper() {
  static const Cache::CacheItemHelper helper(CacheEntryRole::kIndexBlock,
                                             &DeleteSAIBlobPage);
  return &helper;
}

}  // namespace

SAIOndemandStats GetSAIOndemandStats() {
  return {g_page_hits.load(std::memory_order_relaxed),
          g_page_misses.load(std::memory_order_relaxed),
          g_bytes_read.load(std::memory_order_relaxed)};
}

void ResetSAIOndemandStats() {
  g_page_hits.store(0, std::memory_order_relaxed);
  g_page_misses.store(0, std::memory_order_relaxed);
  g_bytes_read.store(0, std::memory_order_relaxed);
}

SAIFileBlobSource::SAIFileBlobSource(const BlockBasedTable* table)
    : table_(table),
      blob_offset_(table->get_rep()->udi_handle.offset()),
      blob_size_(table->get_rep()->udi_handle.size()) {}

bool SAIFileBlobSource::ReadFromPage(uint64_t page_off, uint64_t data_begin,
                                     uint32_t page_len, uint32_t in_page,
                                     uint32_t n, char* dst) {
  const auto* rep = table_->get_rep();
  // no_block_cache=true (or a caller-supplied null block_cache) leaves this
  // null; degrade to uncached reads below rather than crash (same guard as
  // BitLSM's SABIReader::Bin).
  Cache* cache = rep->table_options.block_cache.get();
  // Same mint as BitLSM's on-demand bins: the file's own cache-key base plus
  // the page's file offset. Every reader of this file derives the identical
  // key, so pages are shared without any registry of our own. Keyed by
  // data_begin, not page_off: the first page's aligned-down page_off lies
  // BEFORE the blob and could equal a data block's start offset (same >>2
  // key space), a type-confusing collision. data_begin is clamped inside
  // the UDI extent, which no other block occupies, and distinct pages'
  // data_begins never share a >>2 bucket, so keys stay unique.
  const CacheKey key = rep->base_cache_key.WithOffset(data_begin >> 2);

  if (cache != nullptr) {
    if (Cache::Handle* h = cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr)) {
      auto* page = static_cast<SAIBlobPage*>(cache->Value(h));
      assert(page->size == page_len);
      std::memcpy(dst, page->data.get() + in_page, n);
      cache->Release(h);
      g_page_hits.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  auto page = std::make_unique<SAIBlobPage>();
  page->data = std::make_unique<char[]>(page_len);
  page->size = page_len;
  Slice result;
  // One read of the page's blob extent. The extent lies inside a single
  // aligned 4 KB window by construction, and RandomAccessFileReader::Read
  // realigns offset and length itself under direct I/O, so this is one
  // device-page read, never a straddle.
  IOStatus s = rep->file->Read(IOOptions(), data_begin, page_len, &result,
                               page->data.get(), /*aligned_buf=*/nullptr);
  if (!s.ok() || result.size() < static_cast<size_t>(in_page) + n) {
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

  if (cache != nullptr) {
    // Insert without taking a handle: the bytes are already copied out, and a
    // refused insert (strict capacity) only costs a re-read next time.
    SAIBlobPage* raw = page.release();
    Status is = cache->Insert(key.AsSlice(), raw, SAIBlobPageHelper(),
                              sizeof(SAIBlobPage) + raw->size);
    if (!is.ok()) delete raw;
  }
  return true;
}

const char* SAIFileBlobSource::Read(uint32_t rel_off, uint32_t len,
                                    std::string& scratch) {
  scratch.resize(len);
  if (len == 0) return scratch.data();
  if (static_cast<uint64_t>(rel_off) + len > blob_size_) {
    ok_ = false;
    return scratch.data();
  }
  const uint64_t blob_end = blob_offset_ + blob_size_;
  uint32_t done = 0;
  while (done < len) {
    const uint64_t abs = blob_offset_ + rel_off + done;  // FILE offset
    // File-anchored page grid: the boundary is the enclosing aligned 4 KB
    // window, regardless of where the blob starts. The first and last pages
    // of a blob are short -- bytes outside [blob_offset_, blob_end) belong
    // to other blocks (or the block trailer) and are never cached here.
    const uint64_t page_off = abs & ~static_cast<uint64_t>(kSAIBlobPageSize - 1);
    const uint64_t data_begin = std::max(page_off, blob_offset_);
    const uint64_t data_end = std::min(page_off + kSAIBlobPageSize, blob_end);
    const uint32_t page_len = static_cast<uint32_t>(data_end - data_begin);
    const uint32_t in_page = static_cast<uint32_t>(abs - data_begin);
    const uint32_t n = std::min(len - done, page_len - in_page);
    if (!ReadFromPage(page_off, data_begin, page_len, in_page, n,
                      scratch.data() + done)) {
      return scratch.data();
    }
    done += n;
  }
  return scratch.data();
}

}  // namespace experiment::sai
