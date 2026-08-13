#pragma once
// Per-SSTable index directory, held for as long as the DB is open.
//
// Corresponds to Cassandra's V1SSTableIndex: one small object per SSTable that
// stays resident (segment metadata, row-id range, component offsets) while the
// bulk of the index -- terms, postings, tree leaves -- is left on disk and read
// per query (disk/v1/V1SSTableIndex.java:64-75).
//
// The resident SAI variant keeps its parsed directory inside the block cache
// entry that holds the blob, so evicting the blob also destroys the directory
// and the next query must reload and reparse the whole thing. Keeping the
// directory here instead breaks that coupling: eviction costs only the bytes a
// later lookup re-reads.
//
// Size: 12 bytes per data block (the Section-A record) plus a word per
// attribute, so a 62 MB SSTable with 4 KB blocks costs about 180 KB. That is
// resident memory outside the block cache budget, the same category as
// RocksDB's own per-table reader state; it is reported by
// ApproximateMemoryUsage so a run can state it rather than assume it.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rocksdb/cache.h"
#include "rocksdb/listener.h"
#include "rocksdb/user_defined_index.h"

namespace rocksdb {
class BlockBasedTable;
}

namespace experiment::sai {

class SAIBlobSource;

struct SAIFileDirectory {
  // Where the blob lives inside the SST file.
  uint64_t blob_offset = 0;
  uint64_t blob_size = 0;
  // Total entries indexed in this file (== next rowId), tombstones included.
  uint32_t entries_total = 0;
  // Start of each attribute's region, relative to the blob.
  std::vector<uint32_t> region_off;
  // Section A, one record per data block: rowIds cumulative through block i,
  // and the block's handle.
  std::vector<uint32_t> entry_count_psum;
  std::vector<rocksdb::UserDefinedIndexBuilder::BlockHandle> block_handles;

  // Block and ordinal within it holding `row`. Same mapping as
  // SAIIndexReader::Locate.
  void Locate(uint32_t row, uint32_t& block_idx, uint32_t& ordinal) const;
  size_t ApproximateMemoryUsage() const;
};

// Parses a blob's footer, region table and Section A into `out`. Exposed so a
// test can build a directory over an in-memory blob, with no SST file.
bool BuildDirectoryFromSource(SAIBlobSource& src, SAIFileDirectory* out);

class SAIIndexRegistry {
 public:
  // Directory for `file_number`, parsed from the blob's footer on first use.
  // Returns nullptr if the file carries no readable SAI blob.
  const SAIFileDirectory* Get(uint64_t file_number, rocksdb::BlockBasedTable* bbt,
                              std::shared_ptr<rocksdb::Cache> cache);
  // Called when an SST is deleted; without it a long-running write workload
  // would accumulate directories for files that no longer exist.
  void Drop(uint64_t file_number);
  size_t ApproximateMemoryUsage() const;
  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, std::unique_ptr<SAIFileDirectory>> map_;
};

// Drops registry entries as compaction and flush retire their SSTables.
class SAIRegistryCleaner : public rocksdb::EventListener {
 public:
  explicit SAIRegistryCleaner(SAIIndexRegistry* registry) : registry_(registry) {}
  const char* Name() const override { return "SAIRegistryCleaner"; }
  void OnTableFileDeleted(const rocksdb::TableFileDeletionInfo& info) override;

 private:
  SAIIndexRegistry* registry_;
};

}  // namespace experiment::sai
