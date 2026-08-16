#pragma once
// Embedded Index (Qader SIGMOD'18): per-block Bloom Filter (categorical) +
// per-block raw Zone Map (continuous), plus continuous-only file-level zone map.
// Embedded in each SSTable via RocksDB's UserDefinedIndex.
// Corresponds in ROLE to BitLSM's SABIBuilder/SABIReader/SABIFactory
// (third_party/BitLSM/src/include/sabi.h) but stores BF+zonemap, not bitmaps,
// and is fully decoupled from BitLSM internals.
//
// On-demand mode (--index_mode ondemand, mirrors the SAI baseline): Sections
// A/B/D + footer are a small per-file directory (block handles, offsets into
// Section C, file-level zone maps) and stay resident either way. Section C --
// the per-block Bloom filters and zone maps, one entry per data block -- is
// the bulky payload (empirically ~20x the directory size at --bloom_bits 10)
// and is read on demand through a FileBlobSource (src/bindings/blob_source.h,
// shared with the SAI baseline) instead of being copied into the reader.
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "blob_source.h"
#include "rocksdb/user_defined_index.h"
#include "bit_lsm_option.h"

namespace rocksdb {
class BlockBasedTable;
}

namespace experiment::embedded {

struct ZoneMap {
  double min;
  double max;
  bool overlaps(double lo, bool lo_inc, double hi, bool hi_inc) const {
    bool below = lo_inc ? (max < lo) : (max <= lo);
    bool above = hi_inc ? (min > hi) : (min >= hi);
    return !(below || above);
  }
};

class EmbeddedIndexReader : public rocksdb::UserDefinedIndexReader {
 public:
  // metadata_only=false (resident, the default): copies the whole blob and
  // serves every block's filter region out of that copy -- byte-identical to
  // the pre-ondemand behavior. metadata_only=true: parses ONLY Sections A/B/D
  // + footer (block handles, Section-C offsets, file-level zone maps) into
  // owned vectors and retains NOTHING pointing into index_block; RocksDB then
  // frees the raw block (RetainsIndexContents() == false) and pins just this
  // reader in Rep, handing it the table via SetTable so Section C (the bulky
  // per-block Bloom+zonemap payload) can be read on demand through the block
  // cache.
  EmbeddedIndexReader(rocksdb::Slice& index_block,
                      const bit_lsm::BitLSMOptions& options,
                      uint32_t bloom_bits, bool metadata_only = false);
  std::unique_ptr<rocksdb::UserDefinedIndexIterator> NewIterator(
      const rocksdb::ReadOptions&) override {
    return nullptr;  // pruning is driven by EmbeddedTableIterator, not this.
  }
  // The block cache charges this on top of the raw block bytes
  // (Block_kUserDefinedIndex::ApproximateMemoryUsage), so it must cover every
  // heap structure the reader keeps resident: the blob copy plus the vectors
  // parsed out of it in the constructor. Undercounting here leaks memory past
  // the block_cache budget. (In metadata mode owned_ stays empty and the same
  // expression counts just the directory vectors.)
  size_t ApproximateMemoryUsage() const override {
    return sizeof(*this) + owned_.capacity() +
           entry_count_psum.capacity() * sizeof(uint32_t) +
           block_handles.capacity() *
               sizeof(rocksdb::UserDefinedIndexBuilder::BlockHandle) +
           section_c_off_.capacity() * sizeof(uint32_t) +
           file_zone_.capacity() * sizeof(ZoneMap);
  }
  bool RetainsIndexContents() const override { return !metadata_only_; }
  void SetTable(const rocksdb::BlockBasedTable* table) override {
    table_ = table;
  }

  bool MetadataOnly() const { return metadata_only_; }
  // A fresh source over this reader's table, for the ondemand path.
  // SetTable() must have run first (pinned path only). One source is meant
  // to be reused across every block a single query probes, not reconstructed
  // per block -- the caller (EmbeddedTableIterator::SelectCandidateBlocks)
  // builds one and keeps it alive across the whole per-block loop.
  std::unique_ptr<BlobSource> MakeBlobSource() const {
    assert(table_ != nullptr);
    return std::make_unique<FileBlobSource>(table_);
  }

  // Resident-mode region of Section C for block_idx; len set to its byte
  // length. Requires metadata_only() == false (base_ is null otherwise).
  const char* BlockFilterRegion(uint32_t block_idx, size_t& len) const {
    len = section_c_off_[block_idx + 1] - section_c_off_[block_idx];
    return base_ + section_c_off_[block_idx];
  }
  // On-demand equivalent: reads Section C for block_idx through `src` into
  // `scratch`, returning a pointer to it and setting `len`. `src` is
  // typically the one MakeBlobSource() returned for this query.
  const char* ReadBlockFilterRegion(uint32_t block_idx, BlobSource& src,
                                    std::string& scratch, size_t& len) const {
    len = section_c_off_[block_idx + 1] - section_c_off_[block_idx];
    return src.Read(section_c_off_[block_idx], static_cast<uint32_t>(len),
                    scratch);
  }
  bool FileZoneOverlaps(uint32_t cont_ordinal, double lo, bool lo_inc,
                        double hi, bool hi_inc) const {
    return file_zone_[cont_ordinal].overlaps(lo, lo_inc, hi, hi_inc);
  }

  std::vector<uint32_t> entry_count_psum;                           // size B; retained for parity/debuggability (table iterator scans full candidate blocks)
  std::vector<rocksdb::UserDefinedIndexBuilder::BlockHandle> block_handles; // size B
  bit_lsm::BitLSMOptions options;
  uint32_t bloom_bits;

 private:
  bool metadata_only_ = false;
  // Set once post-construction on the pinned path (metadata mode only); the
  // table outlives this reader and supplies the file, the blob's handle and
  // the cache-key base for on-demand reads.
  const rocksdb::BlockBasedTable* table_ = nullptr;
  const char* base_ = nullptr;
  size_t raw_len_ = 0;
  std::vector<uint32_t> section_c_off_;  // size B+1, absolute offsets into blob
  std::vector<ZoneMap> file_zone_;       // size = #continuous attrs
  std::string owned_;                    // resident mode: the blob copy
};

class EmbeddedIndexBuilder : public rocksdb::UserDefinedIndexBuilder {
 public:
  EmbeddedIndexBuilder(const bit_lsm::BitLSMOptions& options, uint32_t bloom_bits);
  rocksdb::Slice AddIndexEntry(const rocksdb::Slice& last_key_in_current_block,
                               const rocksdb::Slice* first_key_in_next_block,
                               const BlockHandle& block_handle,
                               std::string* separator_scratch) override;
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value) override;
  rocksdb::Status Finish(rocksdb::Slice* index_contents) override;

 private:
  void FlushCurrentBlock(const BlockHandle& bh);

  bit_lsm::BitLSMOptions options_;
  uint32_t bloom_bits_;
  uint32_t entries_total_ = 0;

  // Per-current-block accumulators (reset on AddIndexEntry).
  std::vector<class BloomBuilder> cat_bloom_;   // one per attr; categorical used
  std::vector<ZoneMap> cont_zone_;              // one per attr; continuous used
  std::vector<bool> cont_seen_;                 // first value flag per attr

  // File-level continuous zone maps + first-seen flags.
  std::vector<ZoneMap> file_zone_;
  std::vector<bool> file_seen_;

  // Section A entries.
  struct BlockEntry { uint32_t psum; uint64_t off; uint64_t size; };
  std::vector<BlockEntry> blocks_;
  // Section C payloads, one string per block (already serialized).
  std::vector<std::string> block_payloads_;

  // Owns the serialized blob returned by Finish().
  std::string blob_;
};

class EmbeddedIndexFactory : public rocksdb::UserDefinedIndexFactory {
 public:
  EmbeddedIndexFactory(const bit_lsm::BitLSMOptions& options, uint32_t bloom_bits)
      : options_(options), bloom_bits_(bloom_bits) {}
  const char* Name() const override { return "EmbeddedIndexFactory"; }
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice& index_block) const override;
  // True when ondemand_index is selected: NewReader then mints metadata-only
  // readers, and the open path skips caching the raw blob it is about to
  // discard. No format gate is needed: the Embedded blob layout is the same
  // in both modes, so existing DBs work either way.
  bool ProducesMetadataOnlyReaders() const override {
    return options_.ondemand_index;
  }

 private:
  bit_lsm::BitLSMOptions options_;
  uint32_t bloom_bits_;
};

}  // namespace experiment::embedded
