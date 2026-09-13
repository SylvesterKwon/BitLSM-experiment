// Per-query candidate rows of a BitLSM-format DB (bitlsm or bitlsm-global).
//
// For every read query in a workload, walks the live SSTs and computes the
// rows the BitLSM query path would fetch and verify (sabi_candidates.h): the
// bins SABI selects, combined per the query's CNF. Candidates minus matches is
// the over-read the binning causes, which latency alone cannot separate from
// I/O and cache effects. Offline and read-only: no timing is taken here.
//
//   candidate_count --db_path <db> --workload workloads/read_seq/<wl>.tsv \
//       --indexed_attrs PULocationID,... --output <csv> [--query_limit N]
//       [--verify]
//
// query_id numbers read operations in workload order, as honk_player's read
// CSV does, so the two join on it. --verify also scans every row of every SST
// and evaluates the query on it, reporting the true match count and any
// matching row the candidates missed; the match count must equal
// honk_player's records_matched and false_negatives must be 0.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <cxxopts.hpp>

#define TEST_CACHE_LINE_SIZE \
  64  // matches sabi_dump.cpp: suppresses a RocksDB-internal static_assert

#include "bit_lsm.h"
#include "bit_lsm_iterator.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "json_record_parser.h"
#include "rocksdb_common_option.h"
#include "sabi.h"
#include "sabi_candidates.h"
#include "table/block_based/block_based_table_reader.h"
#include "taxi_schema.h"
#include "tsv_parser.h"

using namespace rocksdb;

namespace {

struct Query {
  bit_lsm::BitLSMQuery query;
  bit_lsm::SABIQuery sabi;
  std::string attr_names;
  uint32_t k;
  uint64_t ssts_skipped = 0;
  uint64_t candidates = 0;
  uint64_t candidate_blocks = 0;
  uint64_t matched = 0;
  uint64_t false_negatives = 0;
};

// Data blocks holding at least one candidate: the blocks the iterator reads.
uint64_t CountBlocks(const bit_lsm::SABIReader& reader,
                     const roaring::Roaring& rows) {
  uint64_t blocks = 0;
  int64_t last = -1;
  auto begin = reader.data_entries_cnt_psum.begin();
  const auto end = reader.data_entries_cnt_psum.end();
  for (uint32_t row : rows) {
    auto it = std::upper_bound(begin, end, row);
    const int64_t block = std::distance(reader.data_entries_cnt_psum.begin(), it);
    if (block != last) {
      ++blocks;
      last = block;
      begin = it;
    }
  }
  return blocks;
}

uint64_t IntProperty(bit_lsm::BitLSM& db, const char* name) {
  uint64_t v = 0;
  db.GetInternalDB()->GetIntProperty(name, &v);
  return v;
}

}  // namespace

int main(int argc, char* argv[]) {
  cxxopts::Options opts("candidate_count",
                        "Per-query SABI candidate rows of a BitLSM DB");
  // clang-format off
  opts.add_options()
    ("db_path", "BitLSM-format DB", cxxopts::value<std::string>())
    ("workload", "Read workload TSV", cxxopts::value<std::string>())
    ("indexed_attrs", "Comma-separated taxi attrs, as used when building",
     cxxopts::value<std::string>())
    ("output", "CSV path", cxxopts::value<std::string>())
    ("query_limit", "Stop after N queries (0 = all)",
     cxxopts::value<uint64_t>()->default_value("0"))
    ("verify", "Also scan every row and count true matches and misses",
     cxxopts::value<bool>()->default_value("false"));
  // clang-format on
  auto result = opts.parse(argc, argv);
  if (!result.count("db_path") || !result.count("workload") ||
      !result.count("indexed_attrs") || !result.count("output")) {
    std::cerr << "Required: --db_path, --workload, --indexed_attrs, --output\n";
    return 1;
  }
  const bool verify = result["verify"].as<bool>();
  const uint64_t query_limit = result["query_limit"].as<uint64_t>();

  // Attribute remapping exactly as honk_player does it, so ParseFilters
  // produces the same attr indices.
  auto all_columns = honk::GetTaxiColumns();
  auto col_map = honk::BuildColumnIndexMap();
  std::vector<uint32_t> indexed;
  std::istringstream iss(result["indexed_attrs"].as<std::string>());
  for (std::string tok; std::getline(iss, tok, ',');) {
    auto it = col_map.find(tok);
    if (it == col_map.end()) {
      std::cerr << "Unknown attribute: " << tok << "\n";
      return 1;
    }
    indexed.push_back(it->second);
  }
  col_map.clear();
  std::vector<honk::TaxiColumn> remapped;
  for (uint32_t i = 0; i < indexed.size(); ++i) {
    col_map[all_columns[indexed[i]].name] = i;
    remapped.push_back(all_columns[indexed[i]]);
  }
  bit_lsm::BitLSMOptions bitlsm_opts = honk::BuildTaxiBitLSMOptions(indexed);
  bitlsm_opts.rho = 0.001;  // builder-only; nothing is written here

  std::vector<Query> queries;
  {
    honk::TSVReader reader(result["workload"].as<std::string>());
    honk::Operation op;
    while (reader.Next(op) &&
           (query_limit == 0 || queries.size() < query_limit)) {
      if (op.type != honk::OpType::READ) continue;
      auto parsed = honk::ParseFilters(std::get<honk::ReadOp>(op.data).json,
                                       remapped, col_map);
      Query q;
      q.query = std::move(parsed.query);
      q.attr_names = parsed.attr_names;
      q.k = parsed.k;
      Status vs = q.query.Validate(bitlsm_opts);
      if (!vs.ok()) {
        std::cerr << "query " << queries.size()
                  << " is invalid: " << vs.ToString() << "\n";
        return 1;
      }
      q.sabi = bit_lsm::EncodeQuery(q.query, bitlsm_opts);
      queries.push_back(std::move(q));
    }
  }

  // Open exactly as BitLSMBinding does (see sabi_dump.cpp).
  rocksdb::Options rocksdb_options;
  rocksdb_options.create_if_missing = false;
  experiment::ApplyRocksdbCommonOptions(rocksdb_options);
  rocksdb::BlockBasedTableOptions table_options;
  experiment::ApplyRocksdbCommonTableOptions(table_options);
  experiment::ApplyRecordCfBloom(table_options);
  bit_lsm::BitLSM db(result["db_path"].as<std::string>(), bitlsm_opts,
                     rocksdb_options, table_options);

  // Memtable rows are not in any SST and the query path checks them in full,
  // so candidates computed from SSTs alone would undercount.
  if (IntProperty(db, "rocksdb.num-entries-active-mem-table") != 0 ||
      IntProperty(db, "rocksdb.num-entries-imm-mem-tables") != 0) {
    std::cerr << "DB has unflushed memtable rows; candidates would be "
                 "incomplete\n";
    return 1;
  }

  auto* db_impl = static_cast<DBImpl*>(db.GetInternalDB());
  auto* cfd = db_impl->GetVersionSet()->GetColumnFamilySet()->GetDefault();
  SuperVersion* sv = cfd->GetReferencedSuperVersion(db_impl);
  bit_lsm::ScanContext ctx(sv);

  uint64_t ssts = 0;
  std::vector<std::string> values;
  std::vector<uint8_t> live;
  for (int level = 0; level < ctx.storage_info->num_non_empty_levels();
       ++level) {
    for (const FileMetaData* f : ctx.storage_info->LevelFiles(level)) {
      TableCache::TypedHandle* handle = nullptr;
      Status s = ctx.tc->FindTable(ReadOptions(), ctx.file_opts, *ctx.icmp, *f,
                                   &handle, ctx.cf_opts);
      if (!s.ok()) {
        std::cerr << "FindTable failed for file " << f->fd.GetNumber() << ": "
                  << s.ToString() << "\n";
        return 1;
      }
      auto* table =
          static_cast<BlockBasedTable*>(ctx.tc->get_cache().Value(handle));
      CachableEntry<Block_kUserDefinedIndex> udi;
      s = table->GetUserDefinedIndexReader(ReadOptions(), &udi);
      if (!s.ok()) {
        std::cerr << "No SABI block in file " << f->fd.GetNumber() << ": "
                  << s.ToString() << "\n";
        return 1;
      }
      auto* reader = static_cast<bit_lsm::SABIReader*>(udi.GetValue()->reader());
      ++ssts;

      if (verify) {
        values.clear();
        live.clear();
        std::unique_ptr<InternalIterator> it(table->NewIterator(
            ReadOptions(), nullptr, nullptr, false,
            TableReaderCaller::kUncategorized));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          ParsedInternalKey ikey;
          const bool ok = ParseInternalKey(it->key(), &ikey, false).ok() &&
                          ikey.type == kTypeValue;
          live.push_back(ok);
          values.emplace_back(ok ? it->value().ToString() : std::string());
        }
        if (!it->status().ok()) {
          std::cerr << "scan of file " << f->fd.GetNumber()
                    << " failed: " << it->status().ToString() << "\n";
          return 1;
        }
        const uint32_t sabi_rows = reader->data_entries_cnt_psum.empty()
                                       ? 0
                                       : reader->data_entries_cnt_psum.back();
        if (values.size() != sabi_rows) {
          std::cerr << "file " << f->fd.GetNumber() << " has " << values.size()
                    << " entries but SABI indexes " << sabi_rows << "\n";
          return 1;
        }
      }

      for (Query& q : queries) {
        const auto c = experiment::ComputeSabiCandidates(*reader, q.sabi);
        if (c.skipped) ++q.ssts_skipped;
        q.candidates += c.rows.cardinality();
        q.candidate_blocks += CountBlocks(*reader, c.rows);
        if (!verify) continue;
        for (uint32_t row = 0; row < values.size(); ++row) {
          if (!live[row] || !q.query.CheckCondition(values[row], bitlsm_opts))
            continue;
          ++q.matched;
          if (c.skipped || !c.rows.contains(row)) ++q.false_negatives;
        }
      }
      udi.Reset();
      ctx.tc->get_cache().Release(handle);
    }
  }

  if (sv->Unref()) {
    db_impl->mutex()->Lock();
    sv->Cleanup();
    db_impl->mutex()->Unlock();
    delete sv;
  }

  std::ofstream out(result["output"].as<std::string>());
  out << "query_id,query_attr_num,filter_attrs,ssts_total,ssts_skipped,"
         "candidates,candidate_blocks";
  if (verify) out << ",matched,false_negatives";
  out << "\n";
  uint64_t misses = 0;
  for (size_t i = 0; i < queries.size(); ++i) {
    const Query& q = queries[i];
    out << i << "," << q.k << ",\"" << q.attr_names << "\"," << ssts << ","
        << q.ssts_skipped << "," << q.candidates << "," << q.candidate_blocks;
    if (verify) out << "," << q.matched << "," << q.false_negatives;
    out << "\n";
    misses += q.false_negatives;
  }
  std::cout << "[candidate_count] " << queries.size() << " queries over "
            << ssts << " SSTs -> " << result["output"].as<std::string>()
            << "\n";
  if (verify && misses != 0) {
    std::cerr << "FALSE NEGATIVES: " << misses << " matching row(s) were not "
              << "candidates\n";
    return 2;
  }
  return 0;
}
