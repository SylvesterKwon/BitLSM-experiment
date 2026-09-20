#pragma once
// Reads one LsmStats sample off a live rocksdb::DB. Every binding that owns a
// RocksDB instance implements SampleLsmStats() as one line handing its DB in
// here, so the property names live in exactly one place and RocksDB headers
// stay inside experiment_bindings.
#include <cstdlib>
#include <map>
#include <string>

#include <rocksdb/db.h>

#include "binding.h"

namespace experiment {

inline uint64_t LsmIntProperty(rocksdb::DB* db, const std::string& name) {
  uint64_t v = 0;
  db->GetIntProperty(name, &v);
  return v;
}

// rocksdb.num-files-at-level<N> has only a string handler (no handle_int in
// InternalStats' property map), so GetIntProperty fails on it and leaves 0.
inline uint64_t LsmStringProperty(rocksdb::DB* db, const std::string& name) {
  std::string v;
  if (!db->GetProperty(name, &v)) return 0;
  return std::strtoull(v.c_str(), nullptr, 10);
}

// Map-property values arrive as strings: uint64 for the stall map, double
// (in GB, or a count printed as "3.000000") for cfstats.
inline double LsmMapDouble(const std::map<std::string, std::string>& m,
                           const std::string& key) {
  auto it = m.find(key);
  return it == m.end() ? 0.0 : std::strtod(it->second.c_str(), nullptr);
}

inline uint64_t LsmGbToBytes(const std::map<std::string, std::string>& m,
                             const std::string& key) {
  return static_cast<uint64_t>(LsmMapDouble(m, key) *
                               static_cast<double>(1ULL << 30));
}

inline LsmStats SampleLsmStatsFrom(rocksdb::DB* db) {
  LsmStats s;
  if (!db) return s;
  using P = rocksdb::DB::Properties;
  s.ok = true;
  s.running_flushes = LsmIntProperty(db, P::kNumRunningFlushes);
  s.running_compactions = LsmIntProperty(db, P::kNumRunningCompactions);
  s.flush_pending = LsmIntProperty(db, P::kMemTableFlushPending);
  s.compaction_pending = LsmIntProperty(db, P::kCompactionPending);
  s.pending_compaction_bytes =
      LsmIntProperty(db, P::kEstimatePendingCompactionBytes);
  s.l0_files = LsmStringProperty(db, P::kNumFilesAtLevelPrefix + "0");
  s.memtable_bytes = LsmIntProperty(db, P::kCurSizeAllMemTables);
  s.immutable_memtables = LsmIntProperty(db, P::kNumImmutableMemTable);
  s.write_stopped = LsmIntProperty(db, P::kIsWriteStopped);
  s.delayed_write_rate = LsmIntProperty(db, P::kActualDelayedWriteRate);

  std::map<std::string, std::string> stall;
  if (db->GetMapProperty(P::kCFWriteStallStats, &stall)) {
    s.stall_stops = static_cast<uint64_t>(
        LsmMapDouble(stall, rocksdb::WriteStallStatsMapKeys::TotalStops()));
    s.stall_delays = static_cast<uint64_t>(
        LsmMapDouble(stall, rocksdb::WriteStallStatsMapKeys::TotalDelays()));
  }
  // cfstats attributes flush output to level 0, so L0.WriteGB/CompCount are
  // the flush volume/count and Sum.* include them.
  std::map<std::string, std::string> cf;
  if (db->GetMapProperty(P::kCFStats, &cf)) {
    s.flush_bytes = LsmGbToBytes(cf, "compaction.L0.WriteGB");
    s.compact_read_bytes = LsmGbToBytes(cf, "compaction.Sum.ReadGB");
    s.compact_write_bytes = LsmGbToBytes(cf, "compaction.Sum.WriteGB");
    s.flush_count =
        static_cast<uint64_t>(LsmMapDouble(cf, "compaction.L0.CompCount"));
    s.compaction_count =
        static_cast<uint64_t>(LsmMapDouble(cf, "compaction.Sum.CompCount"));
  }
  return s;
}

}  // namespace experiment
