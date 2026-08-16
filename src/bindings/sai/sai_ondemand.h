#pragma once
// On-demand readers over a SAI blob: same formats as the resident readers in
// sai_trie/sai_postings/sai_cont_index, but every field is fetched through a
// BlobSource instead of dereferenced from a fully materialised blob.
//
// This is the SAI baseline's memory behaviour brought in line with Cassandra:
// a term lookup walks trie nodes one read at a time, a posting list is decoded
// one 128-posting block at a time (PostingsReader), and a numeric range touches
// only the leaves it overlaps (BlockBalancedTreeReader). The resident readers
// stay as they are -- they remain the default, and the on-demand path is
// selected per run (--index_mode ondemand) so the two can be compared
// directly.
//
// The decode logic is deliberately a separate implementation rather than a
// refactor of the resident readers: those are pure, RocksDB-free, and unit
// tested against a brute-force oracle. sai_test_ondemand.cpp pins the two
// together by running both over identical bytes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "blob_source.h"
#include "sai_plan.h"
#include "sai_postings.h"
#include "sai_trie.h"

namespace experiment::sai {

// A failed blob read has no status channel on the reader's query interface,
// and a cursor that silently went !Valid() would under-return rows and skew
// the comparison; treat it as fatal (same stance as SABIReader's allocation
// failure in sabi_reader.cpp). Term-absent and range-empty outcomes leave
// ok() true and are not errors.
inline void FailIfError(const BlobSource& src) {
  if (!src.ok()) {
    std::fprintf(stderr, "[sai] on-demand index blob read failed; aborting\n");
    std::abort();
  }
}

// Term lookup over the serialized trie. `trie_off` is the trie area's offset
// within the blob (the attribute region's first u32 plus the region offset).
// Returns false when the term is absent OR a read failed; callers distinguish
// via src.ok().
bool TrieLookupOnDemand(BlobSource& src, uint32_t trie_off,
                        std::string_view term, TrieEntry& e);

// Posting list cursor that holds one block at a time, like Cassandra's
// PostingsReader; `list_off` is the list's offset within the blob.
class PostingsCursorOnDemand : public RowCursor {
 public:
  PostingsCursorOnDemand(BlobSource* src, uint32_t list_off);
  bool Valid() const override { return valid_; }
  uint32_t Row() const override { return decoded_[pos_]; }
  void Next() override;
  void AdvanceTo(uint32_t t) override;

 private:
  void LoadBlock(uint32_t block_idx);

  BlobSource* src_;
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
  ContReaderOnDemand(BlobSource* src, uint32_t region_off);
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

  BlobSource* src_;
  uint32_t region_off_;
  uint32_t n_blocks_ = 0;
  uint32_t summaries_off_ = 0;
};

// Fact-level entry points, the same surface as SAIIndexReader's query methods
// but reading through a source. `region_off` is the attribute-region table
// the reader parsed out of the blob footer (owned copies; blob-relative
// offsets). The metadata-only SAIIndexReader routes here with a
// FileBlobSource; sai_test_ondemand drives the same code over a
// MemBlobSource.
uint64_t OnDemandEstimate(BlobSource& src,
                          const std::vector<uint32_t>& region_off,
                          const SAIFact& f);
std::unique_ptr<RowCursor> OnDemandOpenCursor(
    BlobSource* src, const std::vector<uint32_t>& region_off,
    const SAIFact& f);

// Owns the blob source its inner cursor reads through, so a cursor returned
// by SAIIndexReader::OpenCursor stays self-contained (the categorical cursor
// keeps fetching posting blocks through the source as it advances). Declared
// source-first so reverse-order destruction tears the cursor down before the
// source it reads from.
class OwningCursorOnDemand : public RowCursor {
 public:
  OwningCursorOnDemand(std::unique_ptr<BlobSource> src,
                       std::unique_ptr<RowCursor> inner)
      : src_(std::move(src)), inner_(std::move(inner)) {}
  bool Valid() const override { return inner_->Valid(); }
  uint32_t Row() const override { return inner_->Row(); }
  void Next() override {
    inner_->Next();
    FailIfError(*src_);
  }
  void AdvanceTo(uint32_t t) override {
    inner_->AdvanceTo(t);
    FailIfError(*src_);
  }

 private:
  std::unique_ptr<BlobSource> src_;
  std::unique_ptr<RowCursor> inner_;
};

}  // namespace experiment::sai
