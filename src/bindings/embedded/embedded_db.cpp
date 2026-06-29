// Corresponds to BitLSM's BitLSM wrapper (bit_lsm.cpp ctor/Put/NewIterator/dtor);
// ported, SABIFactory replaced with EmbeddedIndexFactory,
// Put uses EmbeddedCodec::Encode instead of BitLSM's EncodeValue/PutBatch.

#include "embedded_db.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "embedded_index.h"
#include "embedded_iterator.h"
#include "embedded_value_codec.h"
#include "rocksdb/options.h"

using namespace std;
using namespace rocksdb;

namespace experiment::embedded {

EmbeddedDB::EmbeddedDB(const string& db_path,
                       const bit_lsm::BitLSMOptions& bit_lsm_options,
                       const Options& rocksdb_options,
                       const BlockBasedTableOptions& table_options,
                       uint32_t bloom_bits)
    : db_path_(db_path),
      bit_lsm_options_(bit_lsm_options),
      bloom_bits_(bloom_bits) {
  rocksdb_options_ = rocksdb_options;

  BlockBasedTableOptions opts = table_options;
  // Install EmbeddedIndexFactory instead of SABIFactory
  opts.user_defined_index_factory =
      make_shared<EmbeddedIndexFactory>(bit_lsm_options_, bloom_bits_);
  rocksdb_options_.table_factory.reset(NewBlockBasedTableFactory(opts));

  ColumnFamilyOptions cf_opts(rocksdb_options_);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
  Status s =
      DB::Open(rocksdb_options_, db_path, column_families, &cf_handles_, &db_);
  if (!s.ok()) throw std::runtime_error("Failed to open DB: " + s.ToString());
}

EmbeddedDB::~EmbeddedDB() {
  Status s;
  // Close DB gracefully
  for (auto handle : cf_handles_) db_->DestroyColumnFamilyHandle(handle);
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db_->WaitForCompact(wait_for_compact_options);
  if (!s.ok()) cerr << "Failed to close DB: " << s.ToString() << "\n";
  delete db_;
  cout << "DB successfully closed\n";
}

Status EmbeddedDB::Put(const string& pk, const vector<Attr>& attrs,
                       const string& payload) {
  // 1. Validate # of indexed attrs (parity with BitLSM::Put)
  if (attrs.size() != bit_lsm_options_.attr_num) {
    return Status::InvalidArgument(
        "The number of attrs does not match with db configuration.");
  }

  // 2. Use EmbeddedCodec::Encode instead of BitLSM's EncodeValue/PutBatch
  std::string v;
  EmbeddedCodec::Encode(bit_lsm_options_, attrs, payload, v);
  return db_->Put(WriteOptions(), pk, v);
}

unique_ptr<EmbeddedIterator> EmbeddedDB::NewIterator(
    bit_lsm::BitLSMQuery& query) {
  // Invalid queries get nullptr; callers needing the reason call Validate
  // directly.
  if (!query.Validate(bit_lsm_options_).ok()) return nullptr;

  // Sort conditions within each OR clause by attr_idx so same-attr conditions
  // are adjacent — guarantees EmbeddedCodec::Evaluate's per-clause decode cache
  // hits. (Copied from bit_lsm.cpp:96-104.)
  for (auto& clause : query.clause_groups) {
    std::sort(clause.begin(), clause.end(),
              [](const bit_lsm::QueryCondition& a,
                 const bit_lsm::QueryCondition& b) {
                return a.attr_idx < b.attr_idx;
              });
  }

  return std::make_unique<EmbeddedIterator>(db_, cf_handles_[0],
                                            bit_lsm_options_, query);
}

}  // namespace experiment::embedded
