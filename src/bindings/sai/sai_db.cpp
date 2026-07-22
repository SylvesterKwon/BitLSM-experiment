// Corresponds to the embedded baseline's EmbeddedDB (src/bindings/embedded/embedded_db.cpp);
// ported to namespace experiment::sai. SAIIndexFactory install lands in Task 6,
// SAIIterator wiring in Task 7.
#include "sai_db.h"
#include <iostream>
#include <stdexcept>
#include "sai_index.h"
#include "sai_value_codec.h"
#include "rocksdb/options.h"

using namespace std;
using namespace rocksdb;

namespace experiment::sai {

SAIDB::SAIDB(const string& db_path, const bit_lsm::BitLSMOptions& bit_lsm_options,
             const Options& rocksdb_options,
             const BlockBasedTableOptions& table_options, int intersection_limit)
    : db_path_(db_path),
      bit_lsm_options_(bit_lsm_options),
      intersection_limit_(intersection_limit) {
  rocksdb_options_ = rocksdb_options;
  BlockBasedTableOptions opts = table_options;
  opts.user_defined_index_factory =
      make_shared<SAIIndexFactory>(bit_lsm_options_);
  rocksdb_options_.table_factory.reset(NewBlockBasedTableFactory(opts));

  ColumnFamilyOptions cf_opts(rocksdb_options_);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
  Status s =
      DB::Open(rocksdb_options_, db_path, column_families, &cf_handles_, &db_);
  if (!s.ok()) throw std::runtime_error("Failed to open DB: " + s.ToString());
}

SAIDB::~SAIDB() {
  for (auto handle : cf_handles_) db_->DestroyColumnFamilyHandle(handle);
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  Status s = db_->WaitForCompact(wait_for_compact_options);
  if (!s.ok()) cerr << "Failed to close DB: " << s.ToString() << "\n";
  delete db_;
  cout << "DB successfully closed\n";
}

Status SAIDB::Put(const string& pk, const vector<Attr>& attrs,
                  const string& payload) {
  if (attrs.size() != bit_lsm_options_.attr_num) {
    return Status::InvalidArgument(
        "The number of attrs does not match with db configuration.");
  }
  thread_local std::string serialized_value_buf;
  SAICodec::Encode(bit_lsm_options_, attrs, payload, serialized_value_buf);
  return db_->Put(WriteOptions(), pk, serialized_value_buf);
}

std::unique_ptr<SAIIterator> SAIDB::NewIterator(bit_lsm::BitLSMQuery& query) {
  (void)query;
  return nullptr;  // Task 7 wires SAIIterator here
}

}  // namespace experiment::sai
