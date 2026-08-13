#pragma once
// On-demand readers over a SAI blob: same formats as the resident readers in
// sai_trie/sai_postings/sai_cont_index, but every field is fetched through a
// SAIBlobSource instead of dereferenced from a fully materialised blob.
//
// This is the SAI baseline's memory behaviour brought in line with Cassandra:
// a term lookup walks trie nodes one read at a time, a posting list is decoded
// one 128-posting block at a time (PostingsReader), and a numeric range touches
// only the leaves it overlaps (BlockBalancedTreeReader). The resident readers
// stay as they are -- they remain the default, and the on-demand path is
// selected per run so the two can be compared directly.
//
// The decode logic is deliberately a separate implementation rather than a
// refactor of the resident readers: those are pure, RocksDB-free, and unit
// tested against a brute-force oracle. sai_test_ondemand.cpp pins the two
// together by running both over identical bytes.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sai_blob_source.h"
#include "sai_index_registry.h"
#include "sai_plan.h"
#include "sai_postings.h"
#include "sai_trie.h"

namespace experiment::sai {

// Term lookup over the serialized trie. `trie_off` is the trie area's offset
// within the blob (the attribute region's first u32 plus the region offset).
bool TrieLookupOnDemand(SAIBlobSource& src, uint32_t trie_off,
                        std::string_view term, TrieEntry& e);

// Posting list cursor that holds one block at a time, like Cassandra's
// PostingsReader; `list_off` is the list's offset within the blob.
class PostingsCursorOnDemand : public RowCursor {
 public:
  PostingsCursorOnDemand(SAIBlobSource* src, uint32_t list_off);
  bool Valid() const override { return valid_; }
  uint32_t Row() const override { return decoded_[pos_]; }
  void Next() override;
  void AdvanceTo(uint32_t t) override;

 private:
  void LoadBlock(uint32_t block_idx);

  SAIBlobSource* src_;
  uint32_t list_off_;
  uint32_t skip_off_ = 0;
  uint32_t n_ = 0, nblocks_ = 0;
  uint32_t cur_block_ = 0;
  std::vector<uint32_t> decoded_;
  std::string scratch_;
  uint32_t pos_ = 0;
  bool valid_ = false;
};

// Numeric attribute reader: summaries are probed through the source, and only
// the overlapping leaves are decoded.
class ContReaderOnDemand {
 public:
  ContReaderOnDemand(SAIBlobSource* src, uint32_t region_off);
  uint64_t Estimate(double lo, bool lo_inc, double hi, bool hi_inc);
  std::unique_ptr<RowCursor> OpenCursor(double lo, bool lo_inc, double hi,
                                        bool hi_inc);

 private:
  struct BlockRange {
    uint32_t first_block, last_block;
    bool any;
  };
  BlockRange Overlap(double lo, bool lo_inc, double hi, bool hi_inc);
  double BlockMin(uint32_t b) { return src_->F64(summaries_off_ + b * 32); }
  double BlockMax(uint32_t b) { return src_->F64(summaries_off_ + b * 32 + 8); }
  uint32_t BlockCount(uint32_t b) { return src_->U32(summaries_off_ + b * 32 + 16); }

  SAIBlobSource* src_;
  uint32_t region_off_;
  uint32_t n_blocks_ = 0;
  uint32_t summaries_off_ = 0;
};

// Same surface as SAIIndexReader's query methods, backed by a resident
// directory plus on-demand reads.
class SAIIndexOnDemand {
 public:
  SAIIndexOnDemand(const SAIFileDirectory* dir, SAIBlobSource* src)
      : dir_(dir), src_(src) {}
  uint64_t Estimate(const SAIFact& f);
  std::unique_ptr<RowCursor> OpenCursor(const SAIFact& f);

 private:
  uint32_t Region(uint32_t attr_idx) const { return dir_->region_off[attr_idx]; }
  const SAIFileDirectory* dir_;
  SAIBlobSource* src_;
};

}  // namespace experiment::sai
