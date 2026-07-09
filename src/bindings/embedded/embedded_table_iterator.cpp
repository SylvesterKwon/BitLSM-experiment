// Corresponds to BitLSM's SABITableIterator
// (third_party/BitLSM/src/include/bit_lsm_iterator.h); block-pruning predicate
// is BF+raw zone map instead of a bitmap.
//
// SABI selects exact ROW indexes from a bitmap; this iterator selects candidate
// BLOCKS from BF/zonemap and then scans EVERY entry in each candidate block,
// applying EmbeddedCodec::Evaluate exactly. The RocksDB-internal mechanics of
// opening a data-block iterator (get_rep(), GetUDIReader(),
// NewDataBlockIterator<DataBlockIter>) are ported from
// third_party/BitLSM/src/include/sabi_table_iterator.cpp.

#include <bit_lsm_query.h>

#include <cstdint>
#include <limits>

#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/format.h"
#define TEST_CACHE_LINE_SIZE \
  64  // To avoid compile error when including block_based_table_reader.h
      // together with other internal headers (matches sabi_table_iterator.cpp).

#include "embedded_iterator.h"
#include "embedded_value_codec.h"
#include "embedded_bloom.h"
#include "embedded_index.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_based_table_reader_impl.h"  // Required: provides NewDataBlockIterator<> template definition
#include "util/coding.h"  // RocksDB internal: DecodeFixed32/DecodeFixed64

namespace experiment::embedded {

using namespace rocksdb;

static double U2D(uint64_t u) {
  double d;
  std::memcpy(&d, &u, sizeof(double));
  return d;
}

EmbeddedTableIterator::EmbeddedTableIterator(BlockBasedTable* bbt,
                                             bit_lsm::BitLSMOptions options,
                                             bit_lsm::BitLSMQuery query)
    : options_(std::move(options)),
      bbt_(bbt),
      query_(std::move(query)),
      // Access pattern copied from sabi_table_iterator.cpp:
      // get the IndexReader, then downcast its UDI reader to ours.
      index_reader_(bbt_->get_rep()->index_reader.get()),
      idx_(static_cast<EmbeddedIndexReader*>(index_reader_->GetUDIReader())) {
  SelectCandidateBlocks();
}

void EmbeddedTableIterator::SelectCandidateBlocks() {
  candidate_blocks_.clear();

  // --- Build flat-AND query facts from clause_groups. FLAT AND ONLY. ---
  // A clause with exactly one condition prunes; OR clauses (size > 1) and any
  // other shape DO NOT prune (skipped here, enforced by exact recheck later).
  struct CatFact {
    uint32_t attr_idx;
    std::string value;
  };
  struct ContFact {
    uint32_t attr_idx;
    uint32_t cont_ordinal;
    double lo;
    bool lo_inc;
    double hi;
    bool hi_inc;
  };
  std::vector<CatFact> cat_facts;
  std::vector<ContFact> cont_facts;

  const double NEG_INF = -std::numeric_limits<double>::infinity();
  const double POS_INF = std::numeric_limits<double>::infinity();

  // Map attr_idx -> index into cont_facts (coalesce per attr).
  auto find_cont = [&](uint32_t attr_idx) -> ContFact* {
    for (auto& f : cont_facts)
      if (f.attr_idx == attr_idx) return &f;
    return nullptr;
  };
  // cont_ordinal of an attr = number of ORDERED attrs with index < attr_idx
  // (matches the builder's file-level zone-map ordering).
  auto cont_ordinal_of = [&](uint32_t attr_idx) -> uint32_t {
    uint32_t ord = 0;
    for (uint32_t i = 0; i < attr_idx; ++i)
      if (options_.attr_specs[i].role == bit_lsm::AttrRole::ORDERED) ++ord;
    return ord;
  };

  for (const auto& clause : query_.clause_groups) {
    if (clause.size() != 1) continue;  // OR / non-single shapes do not prune.
    const bit_lsm::QueryCondition& c = clause[0];
    if (c.attr_idx >= options_.attr_specs.size()) continue;
    if (options_.attr_specs[c.attr_idx].role == bit_lsm::AttrRole::UNORDERED) {
      // Single categorical EQUAL condition only.
      if (c.op != bit_lsm::CompareOp::EQUAL) continue;
      cat_facts.push_back({c.attr_idx, std::get<std::string>(c.value)});
    } else {
      // Single continuous condition; coalesce into [lo, hi] per attr.
      double v = std::get<double>(c.value);
      ContFact* f = find_cont(c.attr_idx);
      if (f == nullptr) {
        cont_facts.push_back({c.attr_idx, cont_ordinal_of(c.attr_idx), NEG_INF,
                              true, POS_INF, true});
        f = &cont_facts.back();
      }
      switch (c.op) {
        case bit_lsm::CompareOp::EQUAL:
          f->lo = v;
          f->lo_inc = true;
          f->hi = v;
          f->hi_inc = true;
          break;
        case bit_lsm::CompareOp::GREATER_EQUAL:
          f->lo = v;
          f->lo_inc = true;
          break;
        case bit_lsm::CompareOp::GREATER:
          f->lo = v;
          f->lo_inc = false;
          break;
        case bit_lsm::CompareOp::LESS_EQUAL:
          f->hi = v;
          f->hi_inc = true;
          break;
        case bit_lsm::CompareOp::LESS:
          f->hi = v;
          f->hi_inc = false;
          break;
      }
    }
  }

  // --- FILE-LEVEL SKIP FIRST: if any continuous fact misses the file zone
  // map, no block in this file can match -> leave candidate_blocks_ empty. ---
  for (const auto& f : cont_facts) {
    if (!idx_->FileZoneOverlaps(f.cont_ordinal, f.lo, f.lo_inc, f.hi,
                                f.hi_inc)) {
      return;
    }
  }

  // --- Per-block: decode Section C in index order, test each fact. ---
  const uint32_t B = static_cast<uint32_t>(idx_->block_handles.size());
  for (uint32_t bi = 0; bi < B; ++bi) {
    size_t region_len = 0;
    const char* region = idx_->BlockFilterRegion(bi, region_len);
    const char* p = region;

    bool block_ok = true;
    // Walk attrs in index order, advancing the read cursor regardless of
    // whether this attr has a query fact, so offsets stay aligned.
    for (uint32_t ai = 0; ai < options_.attr_specs.size() && block_ok; ++ai) {
      if (options_.attr_specs[ai].role == bit_lsm::AttrRole::UNORDERED) {
        // [u32 nbits][ceil(nbits/8) bytes]
        uint32_t nbits = DecodeFixed32(p);
        p += sizeof(uint32_t);
        const char* bloom_bytes = p;
        uint32_t nbytes = (nbits + 7) / 8;
        p += nbytes;
        // Test every categorical fact for this attr.
        for (const auto& cf : cat_facts) {
          if (cf.attr_idx != ai) continue;
          if (!BloomMaybe(bloom_bytes, nbits, idx_->bloom_bits, cf.value)) {
            block_ok = false;
            break;
          }
        }
      } else {
        // [u64 min_bits][u64 max_bits]
        double mn = U2D(DecodeFixed64(p));
        p += sizeof(uint64_t);
        double mx = U2D(DecodeFixed64(p));
        p += sizeof(uint64_t);
        ZoneMap z{mn, mx};
        for (const auto& cf : cont_facts) {
          if (cf.attr_idx != ai) continue;
          if (!z.overlaps(cf.lo, cf.lo_inc, cf.hi, cf.hi_inc)) {
            block_ok = false;
            break;
          }
        }
      }
    }
    (void)region_len;  // length is implied by the index-order walk.

    if (block_ok) {
      // Convert plain-field UDI BlockHandle -> rocksdb::BlockHandle.
      BlockHandle bh;
      bh.set_offset(idx_->block_handles[bi].offset);
      bh.set_size(idx_->block_handles[bi].size);
      candidate_blocks_.push_back(bh);
    }
  }
}

void EmbeddedTableIterator::LoadNextBlock() {
  while (true) {
    ++cur_block_idx_;
    if (cur_block_idx_ >= static_cast<int32_t>(candidate_blocks_.size())) {
      valid_ = false;
      return;
    }

    keys_buf_.clear();
    values_buf_.clear();

    Status s;
    // Same NewDataBlockIterator<DataBlockIter> call shape as
    // SABITableIterator::GetAllByIndexesFromDataBlock. Resetting biter_ unpins
    // the previously held block before opening the new one.
    DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
        ReadOptions(), candidate_blocks_[cur_block_idx_], nullptr,
        BlockType::kData, nullptr, nullptr, nullptr, false, false, s, true);
    biter_.reset(new_biter);

    // Scan EVERY entry in this candidate block; buffer exact matches.
    for (biter_->SeekToFirst(); biter_->Valid(); biter_->Next()) {
      ParsedInternalKey ikey;
      Status ps = rocksdb::ParseInternalKey(biter_->key(), &ikey, false);
      if (!ps.ok()) continue;  // skip corrupted key
      // MVCC filtering.
      if (ikey.sequence > options_.read_seqno) continue;
      // Filter tombstones.
      if (ikey.type == rocksdb::kTypeDeletion ||
          ikey.type == rocksdb::kTypeSingleDeletion)
        continue;
      // Exact recheck of the full query (enforces OR clauses and any facts not
      // used for pruning).
      if (EmbeddedCodec::Evaluate(
              query_,
              std::string_view(biter_->value().data(), biter_->value().size()),
              options_)) {
        PinnableSlice k;
        k.PinSelf(biter_->key());  // key is delta-encoded -> copy.
        PinnableSlice v;
        v.PinSlice(biter_->value(), nullptr);  // value pinned (biter_ alive).
        keys_buf_.push_back(std::move(k));
        values_buf_.push_back(std::move(v));
      }
    }

    if (!keys_buf_.empty()) {
      buf_idx_ = 0;
      valid_ = true;
      return;
    }
    // No matches in this block; continue to the next candidate block.
  }
}

void EmbeddedTableIterator::SeekToFirst() {
  cur_block_idx_ = -1;
  buf_idx_ = 0;
  keys_buf_.clear();
  values_buf_.clear();
  valid_ = false;
  LoadNextBlock();
}

void EmbeddedTableIterator::Next() {
  assert(valid_);
  ++buf_idx_;
  if (buf_idx_ >= static_cast<int32_t>(keys_buf_.size())) {
    valid_ = false;
    LoadNextBlock();
  }
}

Slice EmbeddedTableIterator::key() const {
  assert(Valid());
  return keys_buf_[buf_idx_];
}

Slice EmbeddedTableIterator::value() const {
  assert(Valid());
  return values_buf_[buf_idx_];
}

}  // namespace experiment::embedded
