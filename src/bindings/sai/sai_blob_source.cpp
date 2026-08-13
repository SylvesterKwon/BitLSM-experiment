#include "sai_blob_source.h"

#include <atomic>
#include <cstring>

#include "file/random_access_file_reader.h"
#include "rocksdb/advanced_cache.h"

namespace experiment::sai {
using namespace rocksdb;

namespace {

std::atomic<uint64_t> g_page_hits{0};
std::atomic<uint64_t> g_page_misses{0};
std::atomic<uint64_t> g_bytes_read{0};

// One cached blob page: a heap buffer the cache owns.
struct BlobPage {
  std::unique_ptr<char[]> data;
  size_t size = 0;
};

void DeleteBlobPage(Cache::ObjectPtr obj, MemoryAllocator* /*allocator*/) {
  delete static_cast<BlobPage*>(obj);
}

// Charged as an index block: these pages are index bytes, and the budget
// experiment reasons about index versus data residency.
const Cache::CacheItemHelper* BlobPageHelper() {
  static const Cache::CacheItemHelper helper(CacheEntryRole::kIndexBlock,
                                             &DeleteBlobPage);
  return &helper;
}

// 16-byte cache key, the same width as RocksDB's own block cache keys. The
// high word carries a constant tag so these entries cannot alias a key
// RocksDB derives from a db session id.
struct PageCacheKey {
  uint64_t tag;
  uint64_t page;

  PageCacheKey(uint64_t file_number, uint64_t page_idx)
      : tag(0x5341495F50414745ull ^ file_number),  // "SAI_PAGE" ^ file number
        page(page_idx) {}
  Slice AsSlice() const {
    return Slice(reinterpret_cast<const char*>(this), sizeof(*this));
  }
};

}  // namespace

SAIBlobSourceStats GetSAIBlobSourceStats() {
  return {g_page_hits.load(std::memory_order_relaxed),
          g_page_misses.load(std::memory_order_relaxed),
          g_bytes_read.load(std::memory_order_relaxed)};
}

void ResetSAIBlobSourceStats() {
  g_page_hits.store(0, std::memory_order_relaxed);
  g_page_misses.store(0, std::memory_order_relaxed);
  g_bytes_read.store(0, std::memory_order_relaxed);
}

bool SAIFileBlobSource::ReadFromPage(uint64_t page_idx, uint32_t page_len,
                                     uint32_t in_page, uint32_t n, char* dst) {
  const PageCacheKey key(file_number_, page_idx);

  if (cache_) {
    Cache::Handle* h = cache_->BasicLookup(key.AsSlice(), nullptr);
    if (h != nullptr) {
      auto* page = static_cast<BlobPage*>(cache_->Value(h));
      std::memcpy(dst, page->data.get() + in_page, n);
      cache_->Release(h);
      g_page_hits.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  auto page = std::make_unique<BlobPage>();
  page->data = std::make_unique<char[]>(page_len);
  page->size = page_len;
  Slice result;
  // RandomAccessFileReader::Read realigns offset and length itself when the
  // file was opened with direct I/O, so a page boundary inside the blob needs
  // no special handling here.
  IOStatus s = file_->Read(IOOptions(), blob_offset_ + page_idx * kBlobPageSize,
                           page_len, &result, page->data.get(),
                           /*aligned_buf=*/nullptr);
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

  if (cache_) {
    // Insert without taking a handle: the bytes are already copied out, and a
    // failed insert only costs a re-read next time.
    BlobPage* raw = page.release();
    Status is = cache_->Insert(key.AsSlice(), raw, BlobPageHelper(),
                               sizeof(BlobPage) + raw->size);
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
  uint32_t done = 0;
  while (done < len) {
    const uint64_t abs = static_cast<uint64_t>(rel_off) + done;
    const uint64_t page_idx = abs / kBlobPageSize;
    const uint64_t page_start = page_idx * kBlobPageSize;
    // The last page of a blob is short; reading a full page there would pull
    // in the block trailer and whatever follows it in the SST.
    const uint32_t page_len = static_cast<uint32_t>(
        std::min<uint64_t>(kBlobPageSize, blob_size_ - page_start));
    const uint32_t in_page = static_cast<uint32_t>(abs - page_start);
    const uint32_t n = std::min(len - done, page_len - in_page);
    if (!ReadFromPage(page_idx, page_len, in_page, n, scratch.data() + done)) {
      return scratch.data();
    }
    done += n;
  }
  return scratch.data();
}

}  // namespace experiment::sai
