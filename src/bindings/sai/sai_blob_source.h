#pragma once
// Byte-range access to one SSTable's SAI index blob.
//
// Corresponds to Cassandra's per-column index file access: a query reads the
// ranges it actually touches and holds one posting block at a time
// (disk/v1/postings/PostingsReader.java: "Holds exactly one posting block in
// memory at a time"), instead of materialising a whole SSTable's index.
//
// RocksDB hands a user-defined index to its reader as one block, so the
// resident variant (SAIIndexReader) keeps the entire blob live for as long as
// its cache entry survives -- deviation D5 in the design doc. A source is the
// on-demand alternative: SAIFileBlobSource reads blob ranges straight from the
// SST file and caches them page by page in the same block cache that every
// method's data blocks compete for, so index residency answers to the same
// budget.
//
// Reading a range out of the middle of the blob is only possible because
// BlockBasedTableBuilder writes BlockType::kUserDefinedIndex with
// kNoCompression, leaving the bytes on disk exactly as the builder emitted
// them.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "rocksdb/cache.h"
#include "rocksdb/slice.h"
#include "sai_coding.h"

namespace rocksdb {
class RandomAccessFileReader;
}

namespace experiment::sai {

// Page size for blob reads. Matches the 4 KB data-block size every binding
// shares (rocksdb_common_option.h), so index pages and data blocks occupy the
// block cache at the same granularity.
inline constexpr uint32_t kBlobPageSize = 4096;

// Process-wide totals for the on-demand path, reported at the end of a run.
// Counted here rather than through RocksDB's tickers because these reads
// bypass BlockBasedTable: they are ours, not RocksDB's.
struct SAIBlobSourceStats {
  uint64_t page_hits = 0;    // page served from the block cache
  uint64_t page_misses = 0;  // page read from the SST file
  uint64_t bytes_read = 0;   // bytes pulled off disk (whole pages)
};
SAIBlobSourceStats GetSAIBlobSourceStats();
void ResetSAIBlobSourceStats();

class SAIBlobSource {
 public:
  virtual ~SAIBlobSource() = default;

  // Copies [rel_off, rel_off + len) of the blob into `scratch` and returns a
  // pointer to it. Copying is deliberate: a cached page may be evicted the
  // moment its handle is released, so handing out pointers into cache memory
  // would turn every caller into a lifetime problem. Ranges are small by
  // construction -- a trie node, a summary record, one posting block.
  virtual const char* Read(uint32_t rel_off, uint32_t len,
                           std::string& scratch) = 0;
  virtual uint64_t BlobSize() const = 0;

  // Set when a read failed; the query path turns this into a scan error rather
  // than silently returning fewer rows.
  bool ok() const { return ok_; }

  uint16_t U16(uint32_t rel_off) { return GetU16(Read(rel_off, 2, scratch_)); }
  uint32_t U32(uint32_t rel_off) { return GetU32(Read(rel_off, 4, scratch_)); }
  double F64(uint32_t rel_off) { return GetF64(Read(rel_off, 8, scratch_)); }

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

// Reads through the SST file, caching pages in the block cache.
class SAIFileBlobSource : public SAIBlobSource {
 public:
  // `file` is owned by the table reader and `cache` by the table options; both
  // outlive a source, which lives for one query at most.
  SAIFileBlobSource(rocksdb::RandomAccessFileReader* file, uint64_t blob_offset,
                    uint64_t blob_size, uint64_t file_number,
                    std::shared_ptr<rocksdb::Cache> cache)
      : file_(file),
        blob_offset_(blob_offset),
        blob_size_(blob_size),
        file_number_(file_number),
        cache_(std::move(cache)) {}

  const char* Read(uint32_t rel_off, uint32_t len, std::string& scratch) override;
  uint64_t BlobSize() const override { return blob_size_; }

 private:
  // Copies one page's [in_page, in_page + n) into `dst`.
  bool ReadFromPage(uint64_t page_idx, uint32_t page_len, uint32_t in_page,
                    uint32_t n, char* dst);

  rocksdb::RandomAccessFileReader* file_;
  uint64_t blob_offset_;
  uint64_t blob_size_;
  uint64_t file_number_;
  std::shared_ptr<rocksdb::Cache> cache_;
};

// Serves a blob already in memory. Exists so the on-demand readers can be
// tested against the resident ones over identical bytes, with no SST file.
class SAIMemBlobSource : public SAIBlobSource {
 public:
  SAIMemBlobSource(const char* base, uint64_t size) : base_(base), size_(size) {}
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

}  // namespace experiment::sai
