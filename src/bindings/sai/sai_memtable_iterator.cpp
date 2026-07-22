// Corresponds to the embedded baseline's EmbeddedMemTableIterator
// (src/bindings/embedded/embedded_memtable_iterator.cpp); ported to namespace
// experiment::sai. SAI-specific changes are marked below.

#include "sai_iterator.h"

#include <bit_lsm_query.h>

#include <cassert>
#include <cstdint>

#include "db/version_set.h"
#include "sai_value_codec.h"
#include "rocksdb/options.h"

using namespace std;
using namespace rocksdb;

namespace experiment::sai {

void SAIMemTableIterator::FindNextValidEntry() {
  valid_ = false;
  Status s;

  while (iter_->Valid()) {
    ParsedInternalKey ikey;

    // 1. Skip corrupted key
    s = rocksdb::ParseInternalKey(iter_->key(), &ikey, false);
    if (!s.ok()) {
      iter_->Next();
      continue;
    }

    // 2. MVCC filtering
    if (ikey.sequence > options_.read_seqno) {
      iter_->Next();
      continue;
    }

    // 3. Filter tombstone
    if (ikey.type == rocksdb::kTypeDeletion ||
        ikey.type == rocksdb::kTypeSingleDeletion) {
      iter_->Next();
      continue;
    }

    // 4. Filter query condition (predicate swapped to SAICodec::Evaluate)
    if (SAICodec::Evaluate(query_,
                                std::string_view(iter_->value().data(),
                                                 iter_->value().size()),
                                options_)) {
      valid_ = true;
      return;
    }

    iter_->Next();
  }
}

SAIMemTableIterator::SAIMemTableIterator(rocksdb::MemTable* mem,
                                                   bit_lsm::BitLSMOptions options,
                                                   bit_lsm::BitLSMQuery query)
    : options_(options), mem_(mem), query_(std::move(query)), iter_(nullptr) {
  assert(mem_ != nullptr);
  ReadOptions ro;
  iter_ = mem_->NewIterator(ro, nullptr, &arena_, nullptr, false);
}

SAIMemTableIterator::~SAIMemTableIterator() {
  // Since iter_'s memory space is managed by arena, use destruct instead of
  // delete
  if (iter_ != nullptr) iter_->~InternalIterator();
}

void SAIMemTableIterator::SeekToFirst() {
  iter_->SeekToFirst();
  FindNextValidEntry();
}

void SAIMemTableIterator::Next() {
  assert(Valid());
  iter_->Next();
  FindNextValidEntry();
}

Slice SAIMemTableIterator::key() const {
  assert(Valid());
  return iter_->key();
}

Slice SAIMemTableIterator::value() const {
  assert(Valid());
  return iter_->value();
}

}  // namespace experiment::sai
