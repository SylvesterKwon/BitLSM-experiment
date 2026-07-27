// Corresponds to BitLSM's BitLSMMemTableIterator
// (third_party/BitLSM/src/include/bit_lsm_memtable_iterator.cpp); ported,
// predicate swapped to EmbeddedCodec::Evaluate.

#include "embedded_iterator.h"

#include <bit_lsm_query.h>

#include <cassert>
#include <cstdint>

#include "db/version_set.h"
#include "embedded_value_codec.h"
#include "rocksdb/options.h"

using namespace std;
using namespace rocksdb;

namespace experiment::embedded {

void EmbeddedMemTableIterator::FindNextValidEntry() {
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

    // 4. Filter query condition (predicate swapped to EmbeddedCodec::Evaluate)
    if (EmbeddedCodec::Evaluate(query_,
                                std::string_view(iter_->value().data(),
                                                 iter_->value().size()),
                                options_)) {
      valid_ = true;
      return;
    }

    iter_->Next();
  }

  // Exhausted: surface an error the underlying iterator stopped on, so the
  // merging iterator does not read it as end-of-data.
  if (!iter_->status().ok()) status_ = iter_->status();
}

EmbeddedMemTableIterator::EmbeddedMemTableIterator(rocksdb::MemTable* mem,
                                                   bit_lsm::BitLSMOptions options,
                                                   bit_lsm::BitLSMQuery query)
    : options_(options), mem_(mem), query_(std::move(query)), iter_(nullptr) {
  assert(mem_ != nullptr);
  ReadOptions ro;
  iter_ = mem_->NewIterator(ro, nullptr, &arena_, nullptr, false);
}

EmbeddedMemTableIterator::~EmbeddedMemTableIterator() {
  // Since iter_'s memory space is managed by arena, use destruct instead of
  // delete
  if (iter_ != nullptr) iter_->~InternalIterator();
}

void EmbeddedMemTableIterator::SeekToFirst() {
  iter_->SeekToFirst();
  FindNextValidEntry();
}

void EmbeddedMemTableIterator::Next() {
  assert(Valid());
  iter_->Next();
  FindNextValidEntry();
}

Slice EmbeddedMemTableIterator::key() const {
  assert(Valid());
  return iter_->key();
}

Slice EmbeddedMemTableIterator::value() const {
  assert(Valid());
  return iter_->value();
}

}  // namespace experiment::embedded
