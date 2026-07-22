#pragma once
// Corresponds to the embedded baseline's EmbeddedDB (src/bindings/embedded/embedded_db.h);
// ported to namespace experiment::sai, installs SAIIndexFactory (Task 6) instead of
// EmbeddedIndexFactory, carries the SAI intersection_limit knob.
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "bit_lsm_option.h"
#include "bit_lsm_query.h"

using Attr =
    std::variant<std::monostate, int64_t, uint64_t, double, std::string>;

namespace experiment::sai {

// The real SAIIterator is defined in sai_iterator.h (Task 7). Only a forward
// declaration is needed here: SAIDB::NewIterator returns
// std::unique_ptr<SAIIterator>, but sai_db.cpp includes sai_iterator.h so the
// complete type is visible where NewIterator is defined and where unique_ptr's
// destructor is instantiated.
class SAIIterator;

class SAIDB {
 private:
  rocksdb::DB* db_;
  std::string db_path_;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  rocksdb::Options rocksdb_options_;
  bit_lsm::BitLSMOptions bit_lsm_options_;
  int intersection_limit_;

 public:
  SAIDB(const std::string& db_path, const bit_lsm::BitLSMOptions& bit_lsm_options,
        const rocksdb::Options& rocksdb_options,
        const rocksdb::BlockBasedTableOptions& table_options,
        int intersection_limit);
  ~SAIDB();
  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload);
  std::unique_ptr<SAIIterator> NewIterator(bit_lsm::BitLSMQuery& query);
  rocksdb::DB* GetInternalDB() { return db_; }
};

}  // namespace experiment::sai
