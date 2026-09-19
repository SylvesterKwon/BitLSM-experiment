#include "embedded_index.h"

#include <cstring>

#include "embedded_bloom.h"
#include "embedded_value_codec.h"
#include "util/coding.h"  // RocksDB internal: PutFixed32/PutFixed64

namespace experiment::embedded {
using namespace rocksdb;

static uint64_t D2U(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

EmbeddedIndexBuilder::EmbeddedIndexBuilder(const bit_lsm::BitLSMOptions& options,
                                           uint32_t bloom_bits)
    : options_(options), bloom_bits_(bloom_bits) {
  cat_bloom_.resize(options_.attr_num);
  cont_zone_.resize(options_.attr_num, ZoneMap{0, 0});
  cont_seen_.assign(options_.attr_num, false);
  for (uint32_t i = 0; i < options_.attr_num; ++i)
    if (options_.attr_specs[i].index_type == bit_lsm::IndexType::kRange) {
      file_zone_.push_back(ZoneMap{0, 0});
      file_seen_.push_back(false);
    }
}

void EmbeddedIndexBuilder::OnKeyAdded(const Slice& /*key*/, ValueType type,
                                      const Slice& value) {
  ++entries_total_;
  // No deletes in the target workloads; if a tombstone ever arrives it adds no
  // attribute info (skip), and the entry is still counted above so psum stays
  // aligned with the data block's entry count.
  if (type != ValueType::kValue) return;
  std::string_view v(value.data(), value.size());
  uint32_t cont_ord = 0;
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    auto a = EmbeddedCodec::DecodeAttr(layout_, v, i);
    if (options_.attr_specs[i].index_type == bit_lsm::IndexType::kEquality) {
      cat_bloom_[i].Add(std::get<std::string_view>(a));
    } else {
      double d = std::get<double>(a);
      if (!cont_seen_[i]) { cont_zone_[i] = {d, d}; cont_seen_[i] = true; }
      else { cont_zone_[i].min = std::min(cont_zone_[i].min, d);
             cont_zone_[i].max = std::max(cont_zone_[i].max, d); }
      if (!file_seen_[cont_ord]) { file_zone_[cont_ord] = {d, d};
                                   file_seen_[cont_ord] = true; }
      else { file_zone_[cont_ord].min = std::min(file_zone_[cont_ord].min, d);
             file_zone_[cont_ord].max = std::max(file_zone_[cont_ord].max, d); }
      ++cont_ord;
    }
  }
}

Slice EmbeddedIndexBuilder::AddIndexEntry(const Slice& last_key_in_current_block,
                                          const Slice* /*first_next*/,
                                          const BlockHandle& bh,
                                          std::string* /*scratch*/) {
  FlushCurrentBlock(bh);
  return last_key_in_current_block;
}

void EmbeddedIndexBuilder::FlushCurrentBlock(const BlockHandle& bh) {
  blocks_.push_back({entries_total_, bh.offset, bh.size});
  std::string payload;
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (options_.attr_specs[i].index_type == bit_lsm::IndexType::kEquality) {
      std::string bits = cat_bloom_[i].Finish(bloom_bits_);
      PutFixed32(&payload, static_cast<uint32_t>(bits.size()) * 8);  // nbits
      payload.append(bits);
      cat_bloom_[i] = BloomBuilder();  // reset
    } else {
      ZoneMap z = cont_seen_[i] ? cont_zone_[i] : ZoneMap{0, 0};
      PutFixed64(&payload, D2U(z.min));
      PutFixed64(&payload, D2U(z.max));
      cont_seen_[i] = false;
    }
  }
  block_payloads_.push_back(std::move(payload));
}

Status EmbeddedIndexBuilder::Finish(Slice* index_contents) {
  blob_.clear();
  uint32_t B = static_cast<uint32_t>(blocks_.size());
  // Section A: [psum(u32)][off(u32)][size(u32)] per block
  for (const auto& b : blocks_) {
    PutFixed32(&blob_, b.psum);
    PutFixed32(&blob_, static_cast<uint32_t>(b.off));
    PutFixed32(&blob_, static_cast<uint32_t>(b.size));
  }
  uint32_t sectionB_off = static_cast<uint32_t>(blob_.size());
  // Section B: (B+1) absolute offsets into the blob pointing at Section C regions
  uint32_t sectionC_start = sectionB_off + (B + 1) * sizeof(uint32_t);
  std::vector<uint32_t> c_off(B + 1);
  uint32_t cur = sectionC_start;
  for (uint32_t i = 0; i < B; ++i) {
    c_off[i] = cur;
    cur += static_cast<uint32_t>(block_payloads_[i].size());
  }
  c_off[B] = cur;
  for (uint32_t i = 0; i <= B; ++i) PutFixed32(&blob_, c_off[i]);
  // Section C
  for (const auto& p : block_payloads_) blob_.append(p);
  // Section D: file-level continuous zone maps
  uint32_t sectionD_off = static_cast<uint32_t>(blob_.size());
  for (size_t i = 0; i < file_zone_.size(); ++i) {
    ZoneMap z = file_seen_[i] ? file_zone_[i] : ZoneMap{0, 0};
    PutFixed64(&blob_, D2U(z.min));
    PutFixed64(&blob_, D2U(z.max));
  }
  // Footer: [B][sectionB_off][sectionD_off][num_continuous]
  PutFixed32(&blob_, B);
  PutFixed32(&blob_, sectionB_off);
  PutFixed32(&blob_, sectionD_off);
  PutFixed32(&blob_, static_cast<uint32_t>(file_zone_.size()));
  *index_contents = Slice(blob_);
  return Status::OK();
}

}  // namespace experiment::embedded
