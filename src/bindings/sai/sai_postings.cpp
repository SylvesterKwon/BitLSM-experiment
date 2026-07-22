#include "sai_postings.h"
#include "sai_coding.h"
#include <algorithm>

namespace experiment::sai {

namespace {
// LSB-first bit packing of (k-1) deltas at bpv bits each.
void PackDeltas(std::string& out, const uint32_t* deltas, uint32_t cnt, uint32_t bpv) {
  uint64_t acc = 0;
  uint32_t acc_bits = 0;
  for (uint32_t i = 0; i < cnt; ++i) {
    acc |= static_cast<uint64_t>(deltas[i]) << acc_bits;
    acc_bits += bpv;
    while (acc_bits >= 8) {
      out.push_back(static_cast<char>(acc & 0xFF));
      acc >>= 8;
      acc_bits -= 8;
    }
  }
  if (acc_bits > 0) out.push_back(static_cast<char>(acc & 0xFF));
}

// Number of bits needed to represent v (v is always >= 1 here: deltas are
// strictly positive since PostingsBuilder::Add requires strictly ascending
// rows). Loop is bounded at 32 so it never evaluates `v >> 32`, which would
// be undefined behavior for a uint32_t (shift amount == type width) — the
// naive `while ((v >> b) != 0) ++b;` hits this for deltas needing exactly
// 32 bits, i.e. gaps >= 2^31 between consecutive rows.
uint32_t BitsFor(uint32_t v) {
  uint32_t b = 1;
  while (b < 32 && (v >> b) != 0) ++b;
  return b;
}
}  // namespace

void PostingsBuilder::AppendTo(std::string& out) const {
  const uint32_t n = Count();
  const uint32_t nblocks = (n + kPostingsBlockSize - 1) / kPostingsBlockSize;
  const size_t list_start = out.size();
  PutU32(out, n);
  PutU32(out, nblocks);
  const size_t skip_start = out.size();
  for (uint32_t i = 0; i < nblocks; ++i) { PutU32(out, 0); PutU32(out, 0); }  // reserve
  std::vector<uint32_t> deltas;
  for (uint32_t b = 0; b < nblocks; ++b) {
    const uint32_t lo = b * kPostingsBlockSize;
    const uint32_t hi = std::min(n, lo + kPostingsBlockSize);
    const uint32_t rel_off = static_cast<uint32_t>(out.size() - list_start);
    PatchU32(out, skip_start + b * 8, rel_off);
    PatchU32(out, skip_start + b * 8 + 4, rows_[hi - 1]);  // block max
    PutU32(out, rows_[lo]);                                 // first raw
    deltas.clear();
    uint32_t maxd = 0;
    for (uint32_t i = lo + 1; i < hi; ++i) {
      uint32_t d = rows_[i] - rows_[i - 1];
      deltas.push_back(d);
      maxd = std::max(maxd, d);
    }
    const uint8_t bpv = deltas.empty() ? 0 : static_cast<uint8_t>(BitsFor(maxd));
    out.push_back(static_cast<char>(bpv));
    if (bpv > 0) PackDeltas(out, deltas.data(), static_cast<uint32_t>(deltas.size()), bpv);
  }
}

PostingsCursor::PostingsCursor(const char* list_base) : base_(list_base) {
  n_ = GetU32(base_);
  nblocks_ = GetU32(base_ + 4);
  skip_ = base_ + 8;
  if (n_ > 0) {
    LoadBlock(0);
    valid_ = true;
  }
}

void PostingsCursor::LoadBlock(uint32_t block_idx) {
  cur_block_ = block_idx;
  const uint32_t rel_off = GetU32(skip_ + block_idx * 8);
  const char* p = base_ + rel_off;
  const uint32_t lo = block_idx * kPostingsBlockSize;
  const uint32_t cnt = std::min(n_, lo + kPostingsBlockSize) - lo;
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

void PostingsCursor::Next() {
  ++pos_;
  if (pos_ < decoded_.size()) return;
  if (cur_block_ + 1 < nblocks_) {
    LoadBlock(cur_block_ + 1);
  } else {
    valid_ = false;
  }
}

void PostingsCursor::AdvanceTo(uint32_t t) {
  if (!valid_ || Row() >= t) return;
  // 1. Find first block whose max >= t via the skip table.
  uint32_t b = cur_block_;
  while (b < nblocks_ && GetU32(skip_ + b * 8 + 4) < t) ++b;
  if (b >= nblocks_) { valid_ = false; return; }
  if (b != cur_block_) LoadBlock(b);
  // 2. lower_bound within the decoded block.
  auto it = std::lower_bound(decoded_.begin() + pos_, decoded_.end(), t);
  pos_ = static_cast<uint32_t>(it - decoded_.begin());
  // block max >= t guarantees pos_ < decoded_.size()
}

IntersectionCursor::IntersectionCursor(
    std::vector<std::unique_ptr<RowCursor>> children)
    : children_(std::move(children)) {
  Align();
}

// Corresponds to Cassandra's KeyRangeIntersectionIterator.computeNext
// (iterators/KeyRangeIntersectionIterator.java:66-126): compute the highest
// current key, advance every lagging child to it; an overshoot raises the
// target and restarts; all-equal emits.
void IntersectionCursor::Align() {
  valid_ = false;
  if (children_.empty()) return;
  for (auto& c : children_)
    if (!c->Valid()) return;
  uint32_t target = 0;
  for (auto& c : children_) target = std::max(target, c->Row());
  while (true) {
    bool all_equal = true;
    for (auto& c : children_) {
      if (c->Row() < target) {
        c->AdvanceTo(target);
        if (!c->Valid()) return;
      }
      if (c->Row() > target) {  // overshoot -> raise target, restart
        target = c->Row();
        all_equal = false;
        break;
      }
    }
    if (all_equal) {
      current_ = target;
      valid_ = true;
      return;
    }
  }
}

void IntersectionCursor::Next() {
  children_[0]->Next();  // advance one child, realign (advanceOneRange analogue)
  Align();
}

void IntersectionCursor::AdvanceTo(uint32_t t) {
  if (!valid_ || current_ >= t) return;
  for (auto& c : children_) {
    c->AdvanceTo(t);
    if (!c->Valid()) { valid_ = false; return; }
  }
  Align();
}

}  // namespace experiment::sai
