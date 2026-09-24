// Assert-based test of RoaringOrMergeOperator through a real RocksDB.
// Run: ./build/bin/lazy_bitmaps_test_merge
#include "lazy_bitmaps_merge.h"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>

using namespace experiment::lazy_bitmaps;

static roaring::Roaring GetBitmap(rocksdb::DB* db, const std::string& key) {
  std::string v;
  rocksdb::Status s = db->Get(rocksdb::ReadOptions(), key, &v);
  assert(s.ok());
  return ReadBitmap(v);
}

int main() {
  const std::string dir =
      std::filesystem::temp_directory_path() /
      ("lazy_bitmaps_test_merge_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);

  rocksdb::Options o;
  o.create_if_missing = true;
  o.merge_operator = std::make_shared<RoaringOrMergeOperator>();
  rocksdb::DB* db = nullptr;
  assert(rocksdb::DB::Open(o, dir, &db).ok());

  // 3000 single-rowid operands in the memtable fold at Get time.
  SingleRowidOperand operand;
  for (uint32_t r = 0; r < 3000; ++r)
    assert(db->Merge(rocksdb::WriteOptions(), "k", operand.Bytes(r * 7)).ok());
  tl_operands_folded = 0;
  roaring::Roaring bm = GetBitmap(db, "k");
  assert(bm.cardinality() == 3000);
  for (uint32_t r = 0; r < 3000; ++r) assert(bm.contains(r * 7));
  assert(tl_operands_folded == 3000);

  // A flush partial-merges them: the L0 file holds ONE operand for the key.
  assert(db->Flush(rocksdb::FlushOptions()).ok());
  tl_operands_folded = 0;
  assert(GetBitmap(db, "k") == bm);
  assert(tl_operands_folded == 1);

  // New operands stack on the flushed one; bottommost compaction turns the
  // stack into a plain value, so a Get folds nothing.
  for (uint32_t r = 0; r < 10; ++r)
    assert(db->Merge(rocksdb::WriteOptions(), "k", operand.Bytes(100000 + r)).ok());
  tl_operands_folded = 0;
  roaring::Roaring bm2 = GetBitmap(db, "k");
  assert(bm2.cardinality() == 3010 && tl_operands_folded == 11);
  rocksdb::CompactRangeOptions cro;
  cro.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  assert(db->CompactRange(cro, nullptr, nullptr).ok());
  tl_operands_folded = 0;
  assert(GetBitmap(db, "k") == bm2);
  assert(tl_operands_folded == 0);

  // Direct PartialMergeMulti: OR of operands, and only of operands.
  RoaringOrMergeOperator op;
  std::deque<rocksdb::Slice> ops;
  const std::string a(operand.Bytes(1));
  const std::string b(operand.Bytes(2));
  const std::string c(operand.Bytes(3));
  ops.emplace_back(a);
  ops.emplace_back(b);
  ops.emplace_back(c);
  std::string merged;
  assert(op.PartialMergeMulti("k", ops, &merged, nullptr));
  roaring::Roaring pm = ReadBitmap(merged);
  assert(pm.cardinality() == 3 && pm.contains(1) && pm.contains(2) &&
         pm.contains(3));

  delete db;
  std::filesystem::remove_all(dir);
  std::puts("lazy_bitmaps_test_merge: OK");
  return 0;
}
