#include "sai_index.h"
#include "sai_coding.h"
#include "sai_value_codec.h"

namespace experiment::sai {
using namespace rocksdb;

static constexpr uint32_t kMagic = 0x53414931;  // "SAI1"

SAIIndexBuilder::SAIIndexBuilder(const bit_lsm::BitLSMOptions& options)
    : options_(options) {
  cat_terms_.resize(options_.attr_num);
  cont_.resize(options_.attr_num);
}

void SAIIndexBuilder::OnKeyAdded(const Slice& /*key*/, ValueType type,
                                 const Slice& value) {
  const uint32_t row = entries_total_++;  // every entry gets a rowId (psum alignment)
  if (type != ValueType::kValue) return;  // tombstones counted, not indexed
  std::string_view v(value.data(), value.size());
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    auto a = SAICodec::DecodeAttr(options_, v, i);
    if (options_.attr_specs[i].index_type == bit_lsm::IndexType::kEquality) {
      // Map insert allocates only on first sight of a term (dictionary build is
      // inherently allocating, same as Cassandra's SegmentTrieBuffer); the
      // per-row append is a vector push_back.
      auto sv = std::get<std::string_view>(a);
      auto it = cat_terms_[i].find(sv);
      if (it == cat_terms_[i].end())
        it = cat_terms_[i].emplace(std::string(sv), PostingsBuilder()).first;
      it->second.Add(row);
    } else {
      cont_[i].Add(std::get<double>(a), row);
    }
  }
}

Slice SAIIndexBuilder::AddIndexEntry(const Slice& last_key_in_current_block,
                                     const Slice* /*first_next*/,
                                     const BlockHandle& bh,
                                     std::string* /*scratch*/) {
  blocks_.push_back({entries_total_, bh.offset, bh.size});
  return last_key_in_current_block;
}

Status SAIIndexBuilder::Finish(Slice* index_contents) {
  blob_.clear();
  // Section A (same record shape as embedded_index_builder.cpp:84-89).
  for (const auto& b : blocks_) {
    PutU32(blob_, b.psum);
    PutU32(blob_, static_cast<uint32_t>(b.off));
    PutU32(blob_, static_cast<uint32_t>(b.size));
  }
  // Attr regions.
  std::vector<uint32_t> region_off(options_.attr_num);
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    const size_t region_base = blob_.size();
    region_off[i] = static_cast<uint32_t>(region_base);
    if (options_.attr_specs[i].index_type == bit_lsm::IndexType::kEquality) {
      // [u32 trie_off][postings area][trie area]
      PutU32(blob_, 0);  // trie_off placeholder
      TrieWriter tw;
      for (const auto& [term, pb] : cat_terms_[i]) {
        const uint32_t poff = static_cast<uint32_t>(blob_.size() - region_base);
        pb.AppendTo(blob_);
        tw.Add(term, poff, pb.Count());
      }
      const uint32_t trie_off = static_cast<uint32_t>(blob_.size() - region_base);
      tw.AppendTo(blob_);
      PatchU32(blob_, region_base, trie_off);
    } else {
      cont_[i].AppendTo(blob_);
    }
  }
  // Tail + footer.
  for (uint32_t i = 0; i < options_.attr_num; ++i) PutU32(blob_, region_off[i]);
  PutU32(blob_, static_cast<uint32_t>(blocks_.size()));
  PutU32(blob_, options_.attr_num);
  PutU32(blob_, entries_total_);
  PutU32(blob_, kMagic);
  *index_contents = Slice(blob_);
  return Status::OK();
}

UserDefinedIndexBuilder* SAIIndexFactory::NewBuilder() const {
  return new SAIIndexBuilder(options_);
}
std::unique_ptr<UserDefinedIndexReader> SAIIndexFactory::NewReader(
    Slice& index_block) const {
  // Mode follows the factory flag so it can never disagree with
  // ProducesMetadataOnlyReaders(): the open path checks that BEFORE the
  // reader exists, to decide whether to cache the raw blob.
  return std::make_unique<SAIIndexReader>(index_block, options_,
                                          options_.ondemand_index);
}

}  // namespace experiment::sai
