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
#include <string>

#include "sai_coding.h"

namespace rocksdb {
class BlockBasedTable;
}

namespace experiment::sai {

// Page size for blob reads. Matches the 4 KB data-block size every binding
// shares (rocksdb_common_option.h), so index pages and data blocks occupy the
// block cache at the same granularity. Pages are anchored to FILE offsets
// (page boundary = file_offset & ~4095), never to blob-relative offsets, so a
// cached page always lies within one aligned device page and a direct-I/O
// fetch of it is a single device read.
inline constexpr uint32_t kSAIBlobPageSize = 4096;

// Process-wide totals for the on-demand path, reported at the end of a run.
// Counted here rather than through RocksDB's tickers because these reads
// bypass BlockBasedTable: they are ours, not RocksDB's. Same pattern as
// BitLSM's SABIBinCacheStats (third_party/BitLSM/src/include/sabi_reader.cpp).
struct SAIOndemandStats {
  uint64_t page_hits = 0;    // page served from the block cache
  uint64_t page_misses = 0;  // page read from the SST file
  uint64_t bytes_read = 0;   // bytes pulled off disk (whole pages)
};
SAIOndemandStats GetSAIOndemandStats();
void ResetSAIOndemandStats();

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

  // Set when a read failed; the query path turns this into a hard stop rather
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

// Reads through the SST file, caching pages in the table's own block cache.
// Constructed by the metadata-only SAIIndexReader from the table RocksDB
// handed it via UserDefinedIndexReader::SetTable: the table's Rep supplies the
// file, the blob's handle (rep->udi_handle) and the cache-key base
// (rep->base_cache_key), so pages are keyed exactly like BitLSM's on-demand
// bins -- base_cache_key.WithOffset(page_file_offset >> 2) -- with no
// registry and no file-number keyspace of our own. The table outlives the
// reader (SetTable contract), and a source lives for one query at most.
class SAIFileBlobSource : public SAIBlobSource {
 public:
  explicit SAIFileBlobSource(const rocksdb::BlockBasedTable* table);

  const char* Read(uint32_t rel_off, uint32_t len,
                   std::string& scratch) override;
  uint64_t BlobSize() const override { return blob_size_; }

 private:
  // Copies [in_page, in_page + n) of the page's cached bytes into `dst`.
  // `page_off` is the aligned page boundary (file offset, key identity) and
  // `data_begin`/`page_len` the page's intersection with the blob extent --
  // bytes before or after the blob belong to other blocks and are never
  // served from this entry.
  bool ReadFromPage(uint64_t page_off, uint64_t data_begin, uint32_t page_len,
                    uint32_t in_page, uint32_t n, char* dst);

  const rocksdb::BlockBasedTable* table_;
  uint64_t blob_offset_ = 0;  // file offset of the blob (rep->udi_handle)
  uint64_t blob_size_ = 0;
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
