#include "sai_index.h"
#include "sai_blob_source.h"
#include "sai_coding.h"
#include "sai_ondemand.h"
#include <algorithm>
#include <cassert>

namespace experiment::sai {
using namespace rocksdb;

SAIIndexReader::SAIIndexReader(Slice& index_block,
                               const bit_lsm::BitLSMOptions& options,
                               bool metadata_only)
    : options_(options), metadata_only_(metadata_only) {
  // Both modes parse the same directory into owned vectors. Resident mode
  // additionally copies the whole blob and keeps base_ into that copy;
  // metadata mode parses straight out of index_block and retains no pointer
  // into it -- the block is freed after construction (RetainsIndexContents()
  // == false), so keeping any would be a use-after-free.
  const char* base;
  if (metadata_only_) {
    base = index_block.data();
  } else {
    owned_.assign(index_block.data(), index_block.size());
    base_ = owned_.data();
    base = base_;
  }
  const char* end = base + index_block.size();
  assert(GetU32(end - 4) == 0x53414931 && "bad SAI blob magic");
  entries_total_ = GetU32(end - 8);
  const uint32_t n_attrs = GetU32(end - 12);
  const uint32_t B = GetU32(end - 16);
  assert(n_attrs == options_.attr_num);
  const char* tail = end - 16 - 4 * n_attrs;
  region_off_.resize(n_attrs);
  for (uint32_t i = 0; i < n_attrs; ++i) region_off_[i] = GetU32(tail + i * 4);
  entry_count_psum.resize(B);
  block_handles.resize(B);
  for (uint32_t i = 0; i < B; ++i) {
    const char* p = base + i * 12;
    entry_count_psum[i] = GetU32(p);
    block_handles[i].offset = GetU32(p + 4);
    block_handles[i].size = GetU32(p + 8);
  }
}

uint64_t SAIIndexReader::Estimate(const SAIFact& f) const {
  if (metadata_only_) {
    assert(table_ != nullptr);  // SetTable ran before any query (pinned path)
    SAIFileBlobSource src(table_);
    const uint64_t est = OnDemandEstimate(src, region_off_, f);
    FailIfError(src);
    return est;
  }
  const char* region = Region(f.attr_idx);
  if (f.is_cat) {
    TrieEntry e;
    if (!TrieLookup(region + GetU32(region), f.cat_value, e)) return 0;
    return e.count;
  }
  return ContReader(region).Estimate(f.lo, f.lo_inc, f.hi, f.hi_inc);
}

std::unique_ptr<RowCursor> SAIIndexReader::OpenCursor(const SAIFact& f) const {
  if (metadata_only_) {
    assert(table_ != nullptr);
    auto src = std::make_unique<SAIFileBlobSource>(table_);
    auto inner = OnDemandOpenCursor(src.get(), region_off_, f);
    // A read failure is fatal (FailIfError); a clean nullptr means the term
    // is absent or the range overlaps nothing -- same contract as resident.
    FailIfError(*src);
    if (!inner) return nullptr;
    // The categorical cursor keeps reading posting blocks through the source
    // as it advances, so the source rides along inside the returned cursor.
    return std::make_unique<OwningCursorOnDemand>(std::move(src),
                                                  std::move(inner));
  }
  const char* region = Region(f.attr_idx);
  if (f.is_cat) {
    TrieEntry e;
    if (!TrieLookup(region + GetU32(region), f.cat_value, e)) return nullptr;
    return std::make_unique<PostingsCursor>(region + e.postings_off);
  }
  return ContReader(region).OpenCursor(f.lo, f.lo_inc, f.hi, f.hi_inc);
}

void SAIIndexReader::Locate(uint32_t row, uint32_t& block_idx,
                            uint32_t& ordinal) const {
  auto it = std::upper_bound(entry_count_psum.begin(), entry_count_psum.end(), row);
  block_idx = static_cast<uint32_t>(it - entry_count_psum.begin());
  ordinal = row - (block_idx == 0 ? 0 : entry_count_psum[block_idx - 1]);
}

}  // namespace experiment::sai
