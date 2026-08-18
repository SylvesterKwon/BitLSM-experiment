// Offline SABI inspector: reports, per SST and per indexed attribute, the bin
// count the builder actually produced, the bytes its bitmaps occupy, the
// Roaring container mix, and how evenly rows fall across bins.
//
// Nominal bin budgets (1/rho) and realised ones diverge whenever an attribute
// runs out of distinct values before the budget is spent, and the surplus is
// redistributed. This tool measures that divergence directly from a built DB;
// it opens SSTs read-only through the table cache and never writes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <cxxopts.hpp>
#include <roaring/roaring.h>

#define TEST_CACHE_LINE_SIZE \
  64  // matches sai_table_iterator.cpp / sabi_table_iterator.cpp: suppresses
      // a RocksDB-internal static_assert whose TOSTRING is unavailable here

#include "bit_lsm.h"
#include "bit_lsm_iterator.h"
#include "sabi.h"
#include "taxi_schema.h"
#include "rocksdb_common_option.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "table/block_based/block_based_table_reader.h"

using namespace rocksdb;

namespace {

struct AttrStats {
  uint32_t bins = 0;
  // Bins holding no row. A threshold repeated on a heavy value produces one,
  // so this counts budget the binning policy asked for and cannot use --
  // separately from cv, which only says whether the bins that do hold rows
  // hold similar numbers of them.
  uint32_t empty_bins = 0;
  uint64_t bytes = 0;
  uint64_t rows = 0;
  uint32_t n_array = 0;
  uint32_t n_bitset = 0;
  uint32_t n_run = 0;
  double mean = 0;
  double cv = 0;  // std / mean over per-bin row counts; 0 when bins < 2
};

// Per-bin row counts drive both the mean and the coefficient of variation,
// which is what "approximately equi-depth" has to be judged on.
void Summarize(const std::vector<uint64_t>& counts, AttrStats* out) {
  if (counts.empty()) return;
  double sum = 0;
  for (uint64_t c : counts) {
    sum += static_cast<double>(c);
    if (c == 0) out->empty_bins++;
  }
  out->mean = sum / counts.size();
  if (counts.size() < 2 || out->mean == 0) return;
  double var = 0;
  for (uint64_t c : counts) {
    const double d = static_cast<double>(c) - out->mean;
    var += d * d;
  }
  out->cv = std::sqrt(var / counts.size()) / out->mean;
}

}  // namespace

int main(int argc, char* argv[]) {
  cxxopts::Options opts("sabi_dump", "Dump per-attribute SABI statistics");
  opts.allow_unrecognised_options();
  // clang-format off
  opts.add_options()
    ("db_path", "BitLSM DB to inspect", cxxopts::value<std::string>())
    ("bins_for", "Also print every bin of this attribute: its value range, "
                 "its row count, and where a few probe values land",
     cxxopts::value<std::string>()->default_value(""))
    ("indexed_attrs", "Comma-separated taxi attrs, as used when building",
     cxxopts::value<std::string>()->default_value(""))
    ("rho", "rho the DB was built with (recorded in the output only)",
     cxxopts::value<double>()->default_value("0"))
    ("files_per_level", "Sample at most N SSTs per level (0 = all)",
     cxxopts::value<int>()->default_value("0"))
    ("output", "CSV path", cxxopts::value<std::string>());
  // clang-format on
  auto result = opts.parse(argc, argv);
  const std::string bins_for = result["bins_for"].as<std::string>();
  if (!result.count("db_path") || !result.count("output")) {
    std::cerr << "Required: --db_path, --output\n";
    return 1;
  }
  const std::string db_path = result["db_path"].as<std::string>();
  const double rho = result["rho"].as<double>();
  const int files_per_level = result["files_per_level"].as<int>();

  // Attribute names, in the same order the DB was built with, so the output
  // labels match the experiment's --indexed_attrs.
  auto col_map = honk::BuildColumnIndexMap();
  std::vector<uint32_t> indexed;
  std::vector<std::string> attr_names;
  std::string attrs_str = result["indexed_attrs"].as<std::string>();
  if (!attrs_str.empty()) {
    std::istringstream iss(attrs_str);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
      auto it = col_map.find(tok);
      if (it == col_map.end()) {
        std::cerr << "Unknown attribute: " << tok << "\n";
        return 1;
      }
      indexed.push_back(it->second);
      attr_names.push_back(tok);
    }
  }

  // Open exactly as BitLSMBinding does: the table cache keeps whichever reader
  // the first opener creates, so the I/O regime has to match the experiments'.
  rocksdb::Options rocksdb_options;
  rocksdb_options.create_if_missing = false;
  experiment::ApplyRocksdbCommonOptions(rocksdb_options);
  rocksdb::BlockBasedTableOptions table_options;
  experiment::ApplyRocksdbCommonTableOptions(table_options);
  experiment::ApplyRecordCfBloom(table_options);

  bit_lsm::BitLSMOptions bitlsm_opts =
      honk::BuildTaxiBitLSMOptions(indexed);
  bitlsm_opts.rho = rho > 0 ? rho : 0.001;
  bit_lsm::BitLSM db(db_path, bitlsm_opts, rocksdb_options, table_options);

  auto* db_impl = static_cast<DBImpl*>(db.GetInternalDB());
  auto* cfd = db_impl->GetVersionSet()->GetColumnFamilySet()->GetDefault();
  SuperVersion* sv = cfd->GetReferencedSuperVersion(db_impl);
  bit_lsm::ScanContext ctx(sv);

  std::ofstream out(result["output"].as<std::string>());
  out << "rho,level,file_number,file_size,attr_idx,attr_name,role,bins,"
         "bitmap_bytes,rows_binned,distinct,n_array,n_bitset,n_run,"
         "mean_rows_per_bin,cv,empty_bins\n";

  uint64_t files_seen = 0;
  for (int level = 0; level < ctx.storage_info->num_non_empty_levels();
       ++level) {
    int taken = 0;
    for (const FileMetaData* f : ctx.storage_info->LevelFiles(level)) {
      if (files_per_level > 0 && taken >= files_per_level) break;
      ++taken;

      TableCache::TypedHandle* handle = nullptr;
      Status s = ctx.tc->FindTable(ReadOptions(), ctx.file_opts, *ctx.icmp, *f,
                                   &handle, ctx.cf_opts);
      if (!s.ok()) {
        std::cerr << "FindTable failed for file " << f->fd.GetNumber() << ": "
                  << s.ToString() << "\n";
        continue;
      }
      auto* table =
          static_cast<BlockBasedTable*>(ctx.tc->get_cache().Value(handle));
      CachableEntry<Block_kUserDefinedIndex> udi;
      s = table->GetUserDefinedIndexReader(ReadOptions(), &udi);
      if (!s.ok()) {
        std::cerr << "No SABI block in file " << f->fd.GetNumber() << ": "
                  << s.ToString() << "\n";
        ctx.tc->get_cache().Release(handle);
        continue;
      }
      auto* reader = static_cast<bit_lsm::SABIReader*>(udi.GetValue()->reader());
      const auto& bi = reader->bitmap_index;
      const auto& roles = reader->schema().roles;
      ++files_seen;

      uint32_t offset = 0;
      for (uint32_t a = 0; a < bi.bitmap_nums.size(); ++a) {
        const uint32_t bins = bi.bitmap_nums[a];
        AttrStats st;
        st.bins = bins;
        std::vector<uint64_t> counts;
        counts.reserve(bins);
        for (uint32_t b = 0; b < bins; ++b) {
          const roaring::Roaring& bm = bi.bitmaps[offset + b];
          st.bytes += bm.getSizeInBytes();
          const uint64_t card = bm.cardinality();
          st.rows += card;
          counts.push_back(card);
          roaring_statistics_t rs;
          roaring_bitmap_statistics(&bm.roaring, &rs);
          st.n_array += rs.n_array_containers;
          st.n_bitset += rs.n_bitset_containers;
          st.n_run += rs.n_run_containers;
        }
        Summarize(counts, &st);
        offset += bins;

        // --bins_for: the per-bin view the CSV cannot carry. Thresholds are
        // okeys, so decode them back, and show which bin a probe value lands
        // in -- that is what decides whether a range predicate can skip a bin
        // or has to read it.
        if (!bins_for.empty() && a < attr_names.size() &&
            attr_names[a] == bins_for &&
            roles[a] == bit_lsm::AttrRole::ORDERED) {
          const auto& thresholds =
              std::get<std::vector<uint64_t>>(bi.binning_policy[a]);
          std::cout << std::setprecision(17) << "file " << f->fd.GetNumber()
                    << " level " << level << " attr " << attr_names[a] << ": "
                    << bins << " bins, " << st.empty_bins << " empty, distinct="
                    << (a < reader->distinct_cnts.size()
                            ? reader->distinct_cnts[a]
                            : 0)
                    << "\n";
          for (uint32_t b = 0; b + 1 < thresholds.size() && b < bins; ++b) {
            std::cout << "  bin " << b << "  ["
                      << bit_lsm::OkeyToF64(thresholds[b]) << ", "
                      << bit_lsm::OkeyToF64(thresholds[b + 1])
                      << ")  rows=" << counts[b] << "\n";
          }
          for (double v : {-1.0, 0.0, 1.0, 2.0, 3.0}) {
            const uint64_t k = bit_lsm::F64ToOkey(v);
            auto it = std::upper_bound(thresholds.begin(), thresholds.end(), k);
            std::cout << "  value " << v << " -> bin "
                      << (std::distance(thresholds.begin(), it) - 1) << "\n";
          }
        }

        const char* role =
            roles[a] == bit_lsm::AttrRole::ORDERED ? "ORDERED" : "UNORDERED";
        const std::string name =
            a < attr_names.size() ? attr_names[a] : ("attr" + std::to_string(a));
        const uint64_t distinct =
            a < reader->distinct_cnts.size() ? reader->distinct_cnts[a] : 0;
        out << rho << "," << level << "," << f->fd.GetNumber() << ","
            << f->fd.GetFileSize() << "," << a << "," << name << "," << role
            << "," << st.bins << "," << st.bytes << "," << st.rows << ","
            << distinct << "," << st.n_array << "," << st.n_bitset << ","
            << st.n_run << "," << st.mean << "," << st.cv << ","
            << st.empty_bins << "\n";
      }
      udi.Reset();
      ctx.tc->get_cache().Release(handle);
    }
  }
  out.close();

  if (sv->Unref()) {
    db_impl->mutex()->Lock();
    sv->Cleanup();
    db_impl->mutex()->Unlock();
    delete sv;
  }
  std::cout << "inspected " << files_seen << " SST(s)\n";
  return 0;
}
