#pragma once
// Byte-range access to one SSTable's user-defined-index (UDI) blob.
//
// Shared by every on-demand UDI reader (SAI, Embedded): none of this depends
// on a specific blob format, only on RocksDB's BlockBasedTable plumbing
// (rep->file, rep->udi_handle, rep->table_options.block_cache,
// rep->base_cache_key), so it lives here instead of being copy-pasted per
// binding.
//
// Corresponds to Cassandra's per-column index file access: a query reads the
// ranges it actually touches (disk/v1/postings/PostingsReader.java: "Holds
// exactly one posting block in memory at a time"), instead of materialising a
// whole SSTable's index.
//
// RocksDB hands a user-defined index to its reader as one block, so a
// resident reader keeps the entire blob live for as long as its cache entry
// survives. A source is the on-demand alternative: FileBlobSource reads blob
// ranges straight from the SST file and caches them page by page in the same
// block cache that every method's data blocks compete for, so index
// residency answers to the same budget.
//
// Reading a range out of the middle of the blob is only possible because
// BlockBasedTableBuilder writes BlockType::kUserDefinedIndex with
// kNoCompression, leaving the bytes on disk exactly as the builder emitted
// them.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/advanced_cache.h"  // rocksdb::Cache / Cache::Handle, held
                                     // by PinnedExtent (nested Handle cannot
                                     // be forward-declared)

namespace rocksdb {
class BlockBasedTable;
struct FSReadRequest;
}

namespace experiment {

// Page size for blob reads. Matches the 4 KB data-block size every binding
// shares (rocksdb_common_option.h), so index pages and data blocks occupy the
// block cache at the same granularity. Pages are anchored to FILE offsets
// (page boundary = file_offset & ~4095), never to blob-relative offsets, so a
// cached page always lies within one aligned device page and a direct-I/O
// fetch of it is a single device read.
inline constexpr uint32_t kBlobPageSize = 4096;

// Process-wide totals for the on-demand path, reported at the end of a run.
// Counted here rather than through RocksDB's tickers because these reads
// bypass BlockBasedTable: they are ours, not RocksDB's. Same pattern as
// BitLSM's SABIBinCacheStats (third_party/BitLSM/src/include/sabi_reader.cpp).
// One process-wide counter shared by every binding that uses FileBlobSource;
// only one binding is active per honk_player process, so there is no
// cross-binding mixing.
struct BlobSourceStats {
  uint64_t page_hits = 0;    // page served from the block cache
  uint64_t page_misses = 0;  // page read from the SST file
  uint64_t bytes_read = 0;   // bytes pulled off disk (whole pages)
  uint64_t span_reads = 0;   // bulk preads issued by the multi-page span path
                             // (each covers a run of consecutive missing
                             // pages, all counted in page_misses)
  uint64_t extent_hits = 0;    // extent served from the block cache
  uint64_t extent_misses = 0;  // extent read from the SST file (one pread
                               // each; bytes counted in bytes_read)
  // preads issued by the non-bulk single-page path (ReadFromPage's miss
  // branch) only. page_misses counts PAGES, not preads: a bulk stretch read
  // (ReadStretch) is one pread but increments page_misses once per page it
  // covers, so page_misses alone cannot recover "device reads issued" for
  // the single-page path. This is that missing piece.
  uint64_t single_page_reads = 0;
  uint64_t batch_calls = 0;  // MultiRead calls issued by PrefetchRanges
  uint64_t batch_reads = 0;  // read requests carried by those calls -- the
                             // device reads the batch path issued (one per
                             // run of consecutive missing pages; bytes in
                             // bytes_read, pages in page_misses)
};
BlobSourceStats GetBlobSourceStats();
void ResetBlobSourceStats();

// One reader-defined contiguous blob range (a whole posting list, a boundary
// leaf's record, the summary array, Section C) as a single block-cache entry.
// RAW extent bytes only -- decode stays per-query, it's cheap bitpacking; the
// expensive parts an entry eliminates across queries are the re-read and the
// per-page reassembly. Counterpart of BitLSM's SABICachedBin
// (third_party/BitLSM/src/include/sabi.h), which caches its decoded unit the
// same way.
struct CachedExtent {
  std::unique_ptr<char[]> bytes;
  size_t len = 0;
};

// RAII pin over one fetched extent: keeps the cache entry alive for as long
// as a cursor decodes from it, or owns the buffer outright when there was no
// cache entry to pin (refused insert, null cache, over-cap or non-caching
// source). Exact mirror of SABIPinnedBin's semantics: move-only, Release()
// clears everything, `owned` is the fallback. data() == nullptr means
// nothing was fetched (extent fetching disabled or the read failed).
class PinnedExtent {
 public:
  PinnedExtent() = default;
  PinnedExtent(rocksdb::Cache* cache, rocksdb::Cache::Handle* handle,
               const char* data, size_t len)
      : cache_(cache), handle_(handle), data_(data), len_(len) {}
  explicit PinnedExtent(std::unique_ptr<CachedExtent> owned)
      : owned_(std::move(owned)),
        data_(owned_->bytes.get()),
        len_(owned_->len) {}
  PinnedExtent(PinnedExtent&& o) noexcept { *this = std::move(o); }
  PinnedExtent& operator=(PinnedExtent&& o) noexcept {
    Release();
    cache_ = o.cache_;
    handle_ = o.handle_;
    owned_ = std::move(o.owned_);
    data_ = o.data_;
    len_ = o.len_;
    o.cache_ = nullptr;
    o.handle_ = nullptr;
    o.data_ = nullptr;
    o.len_ = 0;
    return *this;
  }
  PinnedExtent(const PinnedExtent&) = delete;
  PinnedExtent& operator=(const PinnedExtent&) = delete;
  ~PinnedExtent() { Release(); }
  void Release() {
    if (cache_ != nullptr && handle_ != nullptr) cache_->Release(handle_);
    cache_ = nullptr;
    handle_ = nullptr;
    owned_.reset();
    data_ = nullptr;
    len_ = 0;
  }
  const char* data() const { return data_; }
  size_t size() const { return len_; }

 private:
  rocksdb::Cache* cache_ = nullptr;
  rocksdb::Cache::Handle* handle_ = nullptr;
  std::unique_ptr<CachedExtent> owned_;
  const char* data_ = nullptr;
  size_t len_ = 0;
};

// One contiguous blob range a reader already knows it is about to decode.
// Blob-relative, like every other offset on this interface.
struct BlobRange {
  uint32_t rel_off;
  uint32_t len;
};

class BlobSource {
 public:
  virtual ~BlobSource() = default;

  // Copies [rel_off, rel_off + len) of the blob into `scratch` and returns a
  // pointer to it. Copying is deliberate: a cached page may be evicted the
  // moment its handle is released, so handing out pointers into cache memory
  // would turn every caller into a lifetime problem. Ranges are small by
  // construction -- a trie node, a summary record, one posting/filter block.
  virtual const char* Read(uint32_t rel_off, uint32_t len,
                           std::string& scratch) = 0;
  virtual uint64_t BlobSize() const = 0;

  // Fetches [rel_off, rel_off + len) as ONE extent, pinned for the caller's
  // lifetime. FileBlobSource overrides this with a shared-block-cache entry
  // per extent, so later queries reuse the bytes with zero I/O. This base
  // implementation reads through Read() into a buffer the pin owns: used by
  // MemBlobSource (tests run the same cursor code with no cache) and by
  // WindowBlobSource (a sub-extent of an already-pinned window -- e.g. a
  // boundary leaf's postings list -- stays a local copy instead of minting a
  // second cache entry overlapping the window's). Returns an empty pin
  // (data() == nullptr) when LocalExtentCap() is 0 -- extent fetching
  // disabled, callers fall back to their per-block reads -- or when the read
  // failed (ok() goes false, same channel as Read).
  virtual PinnedExtent FetchExtent(uint32_t rel_off, uint32_t len);

  // Announces every byte range this reader is about to touch, before it asks
  // for the first of them. A source that can submit reads in parallel fetches
  // the whole set in one batch here; the default does nothing. This is a
  // HINT ONLY -- bytes are still obtained through Read()/FetchExtent(), and a
  // reader must return the identical rows whether or not the source acts on
  // it, so a failed or skipped prefetch is never an error. Ranges need not be
  // sorted or disjoint.
  virtual void PrefetchRanges(const BlobRange* /*ranges*/, size_t /*n*/) {}

  // Set when a read failed; the query path turns this into a hard stop rather
  // than silently returning fewer rows. Virtual so a wrapping source
  // (WindowBlobSource) can fold in the state of the source it delegates to.
  virtual bool ok() const { return ok_; }

  uint16_t U16(uint32_t rel_off) {
    uint16_t v;
    std::memcpy(&v, Read(rel_off, 2, scratch_), 2);
    return v;
  }
  uint32_t U32(uint32_t rel_off) {
    uint32_t v;
    std::memcpy(&v, Read(rel_off, 4, scratch_), 4);
    return v;
  }
  double F64(uint32_t rel_off) {
    double v;
    std::memcpy(&v, Read(rel_off, 8, scratch_), 8);
    return v;
  }

  // Bytes available from rel_off to the end of the blob, capped at `want`.
  // Records whose length is only known after decoding are read this way.
  uint32_t Clamp(uint32_t rel_off, uint32_t want) const {
    const uint64_t left = BlobSize() - rel_off;
    return static_cast<uint32_t>(std::min<uint64_t>(want, left));
  }

 protected:
  bool ok_ = true;

 private:
  std::string scratch_;
};

// Reads through the SST file, caching pages in the table's own block cache.
// Constructed by a metadata-only UDI reader from the table RocksDB handed it
// via UserDefinedIndexReader::SetTable: the table's Rep supplies the file,
// the blob's handle (rep->udi_handle) and the cache-key base
// (rep->base_cache_key), so pages are keyed exactly like BitLSM's on-demand
// bins -- base_cache_key.WithOffset(page_file_offset >> 2) -- with no
// registry and no file-number keyspace of our own. The table outlives the
// reader (SetTable contract), and a source lives for one query at most.
class FileBlobSource : public BlobSource {
 public:
  explicit FileBlobSource(const rocksdb::BlockBasedTable* table);

  const char* Read(uint32_t rel_off, uint32_t len,
                   std::string& scratch) override;
  uint64_t BlobSize() const override { return blob_size_; }

  // Extents become entries in the table's own block cache (HIGH priority,
  // CacheEntryRole::kIndexBlock, charged like everything else the budget
  // experiment reasons about), so an extent one query decoded is served to
  // the next with zero I/O -- the exact analogue of BitLSM's on-demand bin
  // cache (SABIReader::Bin/LoadRun). A miss is ONE pread of the whole
  // extent, deliberately NOT chunked into per-page entries: the extent entry
  // replaces the pages for these bytes. Over-cap extents and refused inserts
  // degrade to a pin-owned buffer, null cache to an uncached read.
  PinnedExtent FetchExtent(uint32_t rel_off, uint32_t len) override;

  // Batched submission of everything a query is about to read. The pages
  // covering `ranges` that are NOT already cached are grouped into runs of
  // consecutive pages and handed to RandomAccessFileReader::MultiRead as one
  // scatter list, which the Posix implementation services over io_uring when
  // the process enabled it (env/io_posix.cc PosixRandomAccessFile::MultiRead;
  // the harness opts in via ApplyRocksdbCommonOptions). The bytes land in the
  // SAME data_begin-keyed page entries the serial path would have created, so
  // the reads that follow are ordinary cache hits: cache accounting, page
  // granularity and cross-query reuse are untouched, and only the SUBMISSION
  // changes from one-pread-at-a-time to one batch.
  //
  // Purely an optimisation: a failed batch leaves the pages uncached and the
  // serial path reads them (and reports the error) exactly as before. Doing
  // nothing here is always correct.
  void PrefetchRanges(const BlobRange* ranges, size_t n) override;

 private:
  // [file_begin, file_end) runs of consecutive missing pages, submitted
  // together and then chunked back into page cache entries.
  using PageRun = std::pair<uint64_t, uint64_t>;
  void SubmitBatch(std::vector<PageRun>& runs);
  // EXP_SAI_ASYNC_VERIFY: re-reads one completed request synchronously and
  // aborts on any difference. Off by default.
  void VerifyRequest(const rocksdb::FSReadRequest& req);
  // Chunks one completed run into the identical data_begin-keyed page entries
  // ReadFromPage/ReadStretch mint, counting each as a page miss.
  void CacheRunPages(uint64_t run_begin, uint64_t run_end, const char* bytes);

  // Copies [in_page, in_page + n) of the page's cached bytes into `dst`.
  // `data_begin` is the page's key identity (clamped inside the blob extent,
  // per the comment in the .cpp) and `page_len` its intersection with the
  // blob extent -- bytes before or after the blob belong to other blocks and
  // are never served from this entry.
  bool ReadFromPage(uint64_t data_begin, uint32_t page_len, uint32_t in_page,
                    uint32_t n, char* dst);

  // Bulk path for reads spanning multiple pages: cached pages are served from
  // the cache as usual, and each maximal run of consecutive MISSING pages is
  // fetched with one pread (ReadStretch) instead of one per page.
  const char* ReadSpan(uint32_t rel_off, uint32_t len, std::string& scratch);
  // One pread covering [data_begin, data_end) -- a run of missing pages --
  // copying the requested portion into `out` (which addresses [abs_begin,
  // abs_end) of the file) and caching each covered page exactly as the
  // per-page path would have.
  bool ReadStretch(uint64_t data_begin, uint64_t data_end, uint64_t abs_begin,
                   uint64_t abs_end, char* out);

  const rocksdb::BlockBasedTable* table_;
  uint64_t blob_offset_ = 0;  // file offset of the blob (rep->udi_handle)
  uint64_t blob_size_ = 0;
};

// Serves a blob already in memory. Exists so the on-demand readers can be
// tested against the resident ones over identical bytes, with no SST file.
class MemBlobSource : public BlobSource {
 public:
  MemBlobSource(const char* base, uint64_t size) : base_(base), size_(size) {}
  const char* Read(uint32_t rel_off, uint32_t len, std::string& scratch) override {
    if (static_cast<uint64_t>(rel_off) + len > size_) {
      ok_ = false;
      scratch.assign(len, '\0');
      return scratch.data();
    }
    scratch.assign(base_ + rel_off, len);
    return scratch.data();
  }
  uint64_t BlobSize() const override { return size_; }

 private:
  const char* base_;
  uint64_t size_;
};

// Sanity cap for cached/pinned extents (whole posting lists, leaf records,
// Section C): the extents this workload produces are at most a few hundred
// KB to a few MB per SST, so the cap should be unreachable. It bounds what
// may enter the CACHE: an over-cap extent is still fetched in one read but
// bypasses the cache (the pin owns the transient buffer), so a degenerate
// huge extent can neither wipe the cache nor pin cache memory.
inline constexpr uint64_t kMaxLocalExtentBytes = 64ull << 20;

// The active cap. Defaults to kMaxLocalExtentBytes; EXP_SAI_LOCAL_EXTENT_MB
// overrides it (in MB, read once at first use) for A/Bs, with 0 disabling
// extent fetching entirely -- FetchExtent then returns empty pins and the
// cursors run their original per-block reads on top of the still-active
// span coalescing in FileBlobSource::Read. The setter exists so
// sai_test_ondemand can pin the fallback paths against the same oracle.
uint64_t LocalExtentCap();
void SetLocalExtentCapForTest(uint64_t cap);

// Asynchronous/batched submission of the on-demand index reads, off unless
// EXP_SAI_ASYNC_INDEX is set to something other than 0 (read once at first
// use, same shape as LocalExtentCap()). With it off, not one byte of the
// read path changes: PrefetchRanges returns immediately and every read is
// the same pread it was. The setter exists for the unit tests.
bool AsyncIndexReads();
void SetAsyncIndexReadsForTest(bool on);

// True when the readers should announce their ranges up front. Only the
// PAGE-granular path has anything to batch: with extent fetching on, a
// cursor's whole extent is already one read and page entries are never
// populated for those bytes, so prefetching pages under it would read the
// same bytes a second time. Extent cap 0 is therefore part of the condition,
// not an independent knob.
inline bool AsyncPageBatch() {
  return AsyncIndexReads() && LocalExtentCap() == 0;
}

// Pins one contiguous window of the blob through `under`'s FetchExtent and
// serves every read inside the window from the pinned bytes, with zero cache
// round trips and zero copies of the window itself. Reads outside the window
// (and the whole window when the fetch failed or extent fetching is off)
// fall through to `under`. Used by readers that are about to decode a known
// extent block by block; over a FileBlobSource the window IS the shared
// cache entry, so later queries reopen it with zero I/O.
class WindowBlobSource : public BlobSource {
 public:
  WindowBlobSource(BlobSource* under, uint32_t win_off, uint32_t win_len)
      : under_(under), win_off_(win_off) {
    win_ = under_->FetchExtent(win_off, win_len);
    fetched_ = win_.data() != nullptr;
  }
  const char* Read(uint32_t rel_off, uint32_t len,
                   std::string& scratch) override {
    if (fetched_ && rel_off >= win_off_ &&
        static_cast<uint64_t>(rel_off) + len <= win_off_ + win_.size()) {
      scratch.assign(win_.data() + (rel_off - win_off_), len);
      return scratch.data();
    }
    return under_->Read(rel_off, len, scratch);
  }
  uint64_t BlobSize() const override { return under_->BlobSize(); }
  bool ok() const override { return ok_ && under_->ok(); }

 private:
  BlobSource* under_;
  uint32_t win_off_;
  PinnedExtent win_;  // pinned for the window's lifetime
  bool fetched_ = false;
};

}  // namespace experiment
