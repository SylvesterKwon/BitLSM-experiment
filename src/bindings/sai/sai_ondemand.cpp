#include "sai_ondemand.h"

#include <algorithm>
#include <cassert>
#include <queue>

#include "sai_coding.h"

namespace experiment::sai {

namespace {
// Largest a serialized posting block can be: first rowId, the bits-per-value
// byte, and 127 deltas of at most 32 bits each.
constexpr uint32_t kMaxPostingBlockBytes = 4 + 1 + (127 * 32 + 7) / 8;
// Summary record: [f64 min][f64 max][u32 count][u32 values_off][u32 perm_off]
// [u32 postings_off].
constexpr uint32_t kSummaryBytes = 32;
}  // namespace

bool TrieLookupOnDemand(SAIBlobSource& src, uint32_t trie_off,
                        std::string_view term, TrieEntry& e) {
  std::string buf;
  uint32_t node = trie_off + src.U32(trie_off);  // root
  for (unsigned char b : term) {
    const bool has_value = (src.Read(node, 1, buf)[0] & 1) != 0;
    // Skip the node's value to reach its child list.
    const uint32_t child_list = node + 1 + (has_value ? 8 : 0);
    const uint16_t nch = src.U16(child_list);
    if (nch == 0) return false;
    // The (byte, offset) pairs are sorted and the fan-out is small, so the
    // whole list comes back in one read.
    const char* children = src.Read(child_list + 2, nch * 5, buf);
    if (!src.ok()) return false;
    const char* found = nullptr;
    for (uint16_t i = 0; i < nch; ++i) {
      const uint8_t cb = static_cast<uint8_t>(children[i * 5]);
      if (cb == b) {
        found = children + i * 5 + 1;
        break;
      }
      if (cb > b) break;
    }
    if (!found) return false;
    node = trie_off + GetU32(found);
  }
  const char* leaf = src.Read(node, 9, buf);
  if (!src.ok() || (leaf[0] & 1) == 0) return false;
  e.postings_off = GetU32(leaf + 1);
  e.count = GetU32(leaf + 5);
  return true;
}

PostingsCursorOnDemand::PostingsCursorOnDemand(SAIBlobSource* src,
                                               uint32_t list_off)
    : src_(src), list_off_(list_off) {
  std::string buf;
  const char* head = src_->Read(list_off_, 8, buf);
  if (!src_->ok()) return;
  n_ = GetU32(head);
  nblocks_ = GetU32(head + 4);
  skip_off_ = list_off_ + 8;
  if (n_ > 0) {
    LoadBlock(0);
    valid_ = src_->ok();
  }
}

void PostingsCursorOnDemand::LoadBlock(uint32_t block_idx) {
  cur_block_ = block_idx;
  const uint32_t rel_off = src_->U32(skip_off_ + block_idx * 8);
  const uint32_t block_off = list_off_ + rel_off;
  const uint32_t lo = block_idx * kPostingsBlockSize;
  const uint32_t cnt = std::min(n_, lo + kPostingsBlockSize) - lo;
  // The encoded length depends on the block's bits-per-value, which is only
  // known once the block is read, so take the worst case and let the source
  // clamp it against the end of the blob.
  const char* p =
      src_->Read(block_off, src_->Clamp(block_off, kMaxPostingBlockBytes),
                 scratch_);
  if (!src_->ok()) {
    valid_ = false;
    return;
  }
  decoded_.resize(cnt);
  decoded_[0] = GetU32(p);
  p += 4;
  const uint8_t bpv = static_cast<uint8_t>(*p);
  ++p;
  uint64_t acc = 0;
  uint32_t acc_bits = 0;
  const uint32_t mask_bits = bpv;
  for (uint32_t i = 1; i < cnt; ++i) {
    while (acc_bits < mask_bits) {
      acc |= static_cast<uint64_t>(static_cast<uint8_t>(*p)) << acc_bits;
      ++p;
      acc_bits += 8;
    }
    const uint32_t d = static_cast<uint32_t>(acc & ((1ull << mask_bits) - 1));
    acc >>= mask_bits;
    acc_bits -= mask_bits;
    decoded_[i] = decoded_[i - 1] + d;
  }
  pos_ = 0;
}

void PostingsCursorOnDemand::Next() {
  ++pos_;
  if (pos_ < decoded_.size()) return;
  if (cur_block_ + 1 < nblocks_) {
    LoadBlock(cur_block_ + 1);
    valid_ = src_->ok();
  } else {
    valid_ = false;
  }
}

void PostingsCursorOnDemand::AdvanceTo(uint32_t t) {
  if (!valid_ || Row() >= t) return;
  // Walk the skip table for the first block whose max reaches t. Each probe is
  // a 4-byte read; a page holds 512 of them.
  uint32_t b = cur_block_;
  while (b < nblocks_ && src_->U32(skip_off_ + b * 8 + 4) < t) ++b;
  if (b >= nblocks_ || !src_->ok()) {
    valid_ = false;
    return;
  }
  if (b != cur_block_) {
    LoadBlock(b);
    if (!src_->ok()) {
      valid_ = false;
      return;
    }
    pos_ = 0;
  }
  auto it = std::lower_bound(decoded_.begin() + pos_, decoded_.end(), t);
  pos_ = static_cast<uint32_t>(it - decoded_.begin());
}

ContReaderOnDemand::ContReaderOnDemand(SAIBlobSource* src, uint32_t region_off)
    : src_(src), region_off_(region_off) {
  n_blocks_ = src_->U32(region_off_);
  summaries_off_ = region_off_ + 4;
}

ContReaderOnDemand::BlockRange ContReaderOnDemand::Overlap(double lo,
                                                           bool lo_inc,
                                                           double hi,
                                                           bool hi_inc) {
  BlockRange r{0, 0, false};
  if (n_blocks_ == 0) return r;
  auto block_overlaps = [&](uint32_t b) {
    const double mn = BlockMin(b), mx = BlockMax(b);
    const bool below = lo_inc ? (mx < lo) : (mx <= lo);
    const bool above = hi_inc ? (mn > hi) : (mn >= hi);
    return !(below || above);
  };
  uint32_t f = 0;
  while (f < n_blocks_ && !block_overlaps(f)) ++f;
  if (f == n_blocks_) return r;
  uint32_t l = n_blocks_ - 1;
  while (!block_overlaps(l)) --l;  // f exists, so this terminates
  r = {f, l, true};
  return r;
}

uint64_t ContReaderOnDemand::Estimate(double lo, bool lo_inc, double hi,
                                      bool hi_inc) {
  const BlockRange br = Overlap(lo, lo_inc, hi, hi_inc);
  if (!br.any) return 0;
  uint64_t sum = 0;
  for (uint32_t b = br.first_block; b <= br.last_block; ++b) sum += BlockCount(b);
  return sum;  // boundary blocks counted whole, matching ContReader
}

namespace {
// One leaf's contribution, materialised the same way ContReader's BlockCursor
// does it: whole postings for an interior leaf, or the postings whose values
// fall in range for a boundary leaf.
class BlockCursorOnDemand : public RowCursor {
 public:
  BlockCursorOnDemand(SAIBlobSource* src, uint32_t region_off,
                      uint32_t summary_off, double lo, bool lo_inc, double hi,
                      bool hi_inc, bool boundary) {
    std::string buf;
    const char* srec = src->Read(summary_off, kSummaryBytes, buf);
    if (!src->ok()) return;
    const uint32_t cnt = GetU32(srec + 16);
    const uint32_t values_off = region_off + GetU32(srec + 20);
    const uint32_t perm_off = region_off + GetU32(srec + 24);
    const uint32_t postings_off = region_off + GetU32(srec + 28);

    PostingsCursorOnDemand pc(src, postings_off);
    if (!boundary) {
      rows_.reserve(cnt);
      while (pc.Valid()) {
        rows_.push_back(pc.Row());
        pc.Next();
      }
      return;
    }
    // Binary search the value-sorted array for the in-range positions. Each
    // probe is an 8-byte read; a leaf's values span at most two pages.
    auto val = [&](uint32_t i) { return src->F64(values_off + i * 8); };
    uint32_t a = 0, b = cnt;
    {
      uint32_t loi = 0, hii = cnt;
      while (loi < hii) {
        uint32_t mid = (loi + hii) / 2;
        const bool ok = lo_inc ? (val(mid) >= lo) : (val(mid) > lo);
        if (ok) hii = mid; else loi = mid + 1;
      }
      a = loi;
    }
    {
      uint32_t loi = a, hii = cnt;
      while (loi < hii) {
        uint32_t mid = (loi + hii) / 2;
        const bool ok = hi_inc ? (val(mid) <= hi) : (val(mid) < hi);
        if (ok) loi = mid + 1; else hii = mid;
      }
      b = loi;
    }
    if (a >= b) return;
    std::string perm_buf;
    const char* perm = src->Read(perm_off + a * 2, (b - a) * 2, perm_buf);
    if (!src->ok()) return;
    std::vector<uint16_t> ords;
    ords.reserve(b - a);
    for (uint32_t i = 0; i < b - a; ++i) ords.push_back(GetU16(perm + i * 2));
    std::sort(ords.begin(), ords.end());
    std::vector<uint32_t> all;
    all.reserve(cnt);
    while (pc.Valid()) {
      all.push_back(pc.Row());
      pc.Next();
    }
    rows_.reserve(ords.size());
    for (uint16_t o : ords) rows_.push_back(all[o]);  // ords asc, all asc
  }

  bool Valid() const override { return pos_ < rows_.size(); }
  uint32_t Row() const override { return rows_[pos_]; }
  void Next() override { ++pos_; }
  void AdvanceTo(uint32_t t) override {
    pos_ = static_cast<uint32_t>(
        std::lower_bound(rows_.begin() + pos_, rows_.end(), t) - rows_.begin());
  }

 private:
  std::vector<uint32_t> rows_;
  uint32_t pos_ = 0;
};

// K-way min-heap merge over leaf cursors; rowIds are unique across leaves.
class MergedCursorOnDemand : public RowCursor {
 public:
  explicit MergedCursorOnDemand(
      std::vector<std::unique_ptr<BlockCursorOnDemand>> blocks)
      : blocks_(std::move(blocks)) {
    for (auto& b : blocks_)
      if (b->Valid()) heap_.push(b.get());
  }
  bool Valid() const override { return !heap_.empty(); }
  uint32_t Row() const override { return heap_.top()->Row(); }
  void Next() override {
    BlockCursorOnDemand* top = heap_.top();
    heap_.pop();
    top->Next();
    if (top->Valid()) heap_.push(top);
  }
  void AdvanceTo(uint32_t t) override {
    while (!heap_.empty() && heap_.top()->Row() < t) {
      BlockCursorOnDemand* top = heap_.top();
      heap_.pop();
      top->AdvanceTo(t);
      if (top->Valid()) heap_.push(top);
    }
  }

 private:
  struct Cmp {
    bool operator()(const BlockCursorOnDemand* a,
                    const BlockCursorOnDemand* b) const {
      return a->Row() > b->Row();
    }
  };
  std::vector<std::unique_ptr<BlockCursorOnDemand>> blocks_;
  std::priority_queue<BlockCursorOnDemand*, std::vector<BlockCursorOnDemand*>, Cmp>
      heap_;
};
}  // namespace

std::unique_ptr<RowCursor> ContReaderOnDemand::OpenCursor(double lo, bool lo_inc,
                                                          double hi,
                                                          bool hi_inc) {
  const BlockRange br = Overlap(lo, lo_inc, hi, hi_inc);
  if (!br.any) return nullptr;
  std::vector<std::unique_ptr<BlockCursorOnDemand>> blocks;
  for (uint32_t b = br.first_block; b <= br.last_block; ++b) {
    const uint32_t summary_off = summaries_off_ + b * kSummaryBytes;
    // A leaf is interior when its whole [min, max] fits the query range.
    const double mn = BlockMin(b), mx = BlockMax(b);
    const bool lo_ok = lo_inc ? (mn >= lo) : (mn > lo);
    const bool hi_ok = hi_inc ? (mx <= hi) : (mx < hi);
    const bool interior = lo_ok && hi_ok;
    blocks.push_back(std::make_unique<BlockCursorOnDemand>(
        src_, region_off_, summary_off, lo, lo_inc, hi, hi_inc, !interior));
  }
  return std::make_unique<MergedCursorOnDemand>(std::move(blocks));
}

uint64_t SAIIndexOnDemand::Estimate(const SAIFact& f) {
  const uint32_t region = Region(f.attr_idx);
  if (f.is_cat) {
    TrieEntry e;
    const uint32_t trie_off = region + src_->U32(region);
    if (!TrieLookupOnDemand(*src_, trie_off, f.cat_value, e)) return 0;
    return e.count;
  }
  return ContReaderOnDemand(src_, region).Estimate(f.lo, f.lo_inc, f.hi, f.hi_inc);
}

std::unique_ptr<RowCursor> SAIIndexOnDemand::OpenCursor(const SAIFact& f) {
  const uint32_t region = Region(f.attr_idx);
  if (f.is_cat) {
    TrieEntry e;
    const uint32_t trie_off = region + src_->U32(region);
    if (!TrieLookupOnDemand(*src_, trie_off, f.cat_value, e)) return nullptr;
    return std::make_unique<PostingsCursorOnDemand>(src_, region + e.postings_off);
  }
  return ContReaderOnDemand(src_, region)
      .OpenCursor(f.lo, f.lo_inc, f.hi, f.hi_inc);
}

}  // namespace experiment::sai
