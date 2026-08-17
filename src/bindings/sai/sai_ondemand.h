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

// Cursors fetch their whole extent (a posting list, a boundary leaf's
// record, the summary array) as ONE pinned cache extent
// (BlobSource::FetchExtent) and decode from the pinned bytes -- zero cache
// round trips per block, and over a FileBlobSource the extent is a shared
// block-cache entry, so later queries touching the same structure reuse it
// with zero I/O. LocalExtentCap() == 0 (blob_source.h:
// EXP_SAI_LOCAL_EXTENT_MB) makes FetchExtent return empty pins and every
// cursor degrade to its original per-block reads.

// Term lookup over the serialized trie. `trie_off` is the trie area's offset
// within the blob (the attribute region's first u32 plus the region offset).
// Returns false when the term is absent OR a read failed; callers distinguish
// via src.ok(). Stays a read per node: the walk is pointer-chasing, so there
// is no extent to coalesce.
bool TrieLookupOnDemand(BlobSource& src, uint32_t trie_off,
                        std::string_view term, TrieEntry& e);

// Posting list cursor. At open it fetches the WHOLE list extent (header,
// skip table, every block) as one pinned extent and decodes 128-posting
// blocks and skip probes from the pinned bytes -- on a FileBlobSource the
// first open reads the list with one pread and caches it, every later open
// of the same list pins the cached bytes with zero I/O. When FetchExtent
// declines (cap 0, or a read failure) the cursor falls back to one block at
// a time, like Cassandra's PostingsReader. `list_off` is the list's offset
// within the blob.
//
// `known_extent_len`: callers that can bound the whole list extent from a
// directory they already hold (ContReaderOnDemand: leaf records are
// contiguous, so leaf b's postings end exactly where leaf b+1's values
// begin) pass the bound so the cursor fetches its extent FIRST and decodes
// the header out of the pinned bytes -- a cold open is then exactly one
// extent read. With 0 (categorical lists: the trie leaf gives no length)
// the header and last skip entry are read through the source first to size
// the fetch, costing one page-granular read ahead of the extent on a cold
// open.
class PostingsCursorOnDemand : public RowCursor {
 public:
  PostingsCursorOnDemand(BlobSource* src, uint32_t list_off,
                         uint32_t known_extent_len = 0);
  bool Valid() const override { return valid_; }
  uint32_t Row() const override { return decoded_[pos_]; }
  void Next() override;
  void AdvanceTo(uint32_t t) override;

 private:
  void LoadBlock(uint32_t block_idx);
  uint32_t SkipU32(uint32_t rel_to_list);  // read within the skip table

  BlobSource* src_;
  uint32_t list_off_;
  uint32_t skip_off_ = 0;
  uint32_t n_ = 0, nblocks_ = 0;
  uint32_t cur_block_ = 0;
  std::vector<uint32_t> decoded_;
  std::string scratch_;
  PinnedExtent local_;     // the whole list extent, pinned for the cursor's
                           // lifetime (cache entry, or owned on fallback)
  bool local_ok_ = false;  // false -> per-block fallback through src_
  uint32_t pos_ = 0;
  bool valid_ = false;
};

// Numeric attribute reader. The summary array is pinned once at
// construction (Overlap()'s scans and Estimate()'s per-block counts would
// otherwise pay a cache round trip per 8-byte probe); on a FileBlobSource it
// is a shared cache extent, so only the first query per SST reads it.
// OpenCursor gives each BOUNDARY leaf (at most the range's two ends) a
// pinned window over its whole values|perm|postings record; interior leaves
// only ever decode postings, so they skip the window and let their postings
// cursor fetch exactly the list extent. Everything degrades to
// per-probe/per-block reads when extent fetching is off (cap 0).
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
  double BlockMin(uint32_t b);
  double BlockMax(uint32_t b);
  uint32_t BlockCount(uint32_t b);
  uint32_t ValuesOff(uint32_t b);
  // Pointer to block b's 32-byte summary record: into the local array when
  // fetched, else read through the source into `buf`.
  const char* SummaryRec(uint32_t b, std::string& buf);

  BlobSource* src_;
  uint32_t region_off_;
  uint32_t n_blocks_ = 0;
  uint32_t summaries_off_ = 0;
  PinnedExtent summaries_;     // whole summary array, pinned once
  bool summaries_ok_ = false;  // false -> per-probe fallback through src_
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
