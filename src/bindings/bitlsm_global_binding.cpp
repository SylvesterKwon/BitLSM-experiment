#include "bitlsm_global_binding.h"

#include <cstdio>
#include <cstdlib>
#include <cxxopts.hpp>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>

#include <rocksdb/table.h>

#include "block_prefetch_queue.h"
#include "global_bins/bin_policy.h"
#include "global_bins/global_sabi_factory.h"

namespace experiment {

namespace {

constexpr const char* kSstListFile = "GLOBAL_BINS_SST_LIST";

[[noreturn]] void Fail(const std::string& why) {
  fprintf(stderr, "[BitLSMGlobalBinding] %s\n", why.c_str());
  abort();
}

// (file name, size) of every live SST. Any flush or compaction changes it.
using SstSet = std::set<std::pair<std::string, uint64_t>>;

SstSet LiveSsts(rocksdb::DB* db) {
  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  SstSet out;
  for (const auto& f : files) out.insert({f.name, f.size});
  return out;
}

void WriteSstList(const std::string& db_path, const SstSet& ssts) {
  std::ofstream f(db_path + "/" + kSstListFile, std::ios::trunc);
  for (const auto& [name, size] : ssts) f << name << " " << size << "\n";
  if (!f) Fail("cannot write " + db_path + "/" + kSstListFile);
}

SstSet ReadSstList(const std::string& db_path) {
  std::ifstream f(db_path + "/" + kSstListFile);
  if (!f)
    Fail(db_path + " has no " + kSstListFile +
         ": not built by bitlsm-global (build it with --bin_policy)");
  SstSet out;
  std::string name;
  uint64_t size;
  while (f >> name >> size) out.insert({name, size});
  return out;
}

void CheckSstsUnchanged(rocksdb::DB* db, const std::string& db_path,
                        const char* when) {
  if (LiveSsts(db) != ReadSstList(db_path))
    Fail(std::string("live SSTs differ from the ones bitlsm-global built (") +
         when + "): a flush or compaction under bit_lsm::BitLSM rewrote SSTs "
         "with local bins, so this DB no longer measures global bins");
}

}  // namespace

void BitLSMGlobalBinding::Open(int argc, char* argv[],
                                const std::string& db_path,
                                const BitLSMOptions& opts) {
  db_path_ = db_path;
  cxxopts::Options cxx("bitlsm-global", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("bin_policy",
                    "Oracle bin policy file (global_bin_policy); given = build "
                    "the DB, omitted = query it",
                    cxxopts::value<std::string>()->default_value(""));
  const std::string policy_path =
      cxx.parse(argc, argv)["bin_policy"].as<std::string>();

  if (policy_path.empty()) {
    ReadSstList(db_path);  // refuse a DB bitlsm-global did not build
    BitLSMBinding::Open(argc, argv, db_path, opts);
    CheckSstsUnchanged(db_->GetInternalDB(), db_path, "at open");
    return;
  }

  rocksdb::Options rocksdb_options;
  rocksdb::BlockBasedTableOptions table_options;
  BuildOpenOptions(argc, argv, opts, &rocksdb_options, &table_options,
                   &build_opts_);

  auto policy = std::make_shared<global_bins::BinPolicy>();
  rocksdb::Status s = global_bins::LoadBinPolicy(policy_path, policy.get());
  if (!s.ok()) Fail(s.ToString());
  s = bit_lsm::ValidateAttrSpecs(build_opts_);
  if (!s.ok()) Fail(s.ToString());
  std::cout << "[BitLSMGlobalBinding] building with bin policy " << policy_path
            << " (" << policy->rows << " rows of " << policy->source << ")\n";

  // What bit_lsm::BitLSM's constructor does before DB::Open, minus the SABI
  // factory: io_uring opt-in, then the per-CF options of BuildCFOptions
  // (bit_lsm.cpp) with GlobalSABIFactory in place of SABIFactory. The core's
  // stats listener is left out: it only feeds the estimator, which is off.
  if (build_opts_.scan_prefetch_depth > 0 || build_opts_.ondemand_index)
    bit_lsm::EnableRocksDbIOUring();
  raw_options_ = rocksdb_options;
  raw_cf_options_ = rocksdb::ColumnFamilyOptions(rocksdb_options);
  table_options.user_defined_index_factory =
      std::make_shared<global_bins::GlobalSABIFactory>(build_opts_,
                                                       std::move(policy));
  raw_cf_options_.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));
  raw_cf_options_.level_compaction_dynamic_level_bytes = true;
  OpenRaw();
  layout_ = std::make_unique<bit_lsm::ValueLayout>(build_opts_);
}

void BitLSMGlobalBinding::OpenRaw() {
  std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs{
      {rocksdb::kDefaultColumnFamilyName, raw_cf_options_}};
  std::vector<rocksdb::ColumnFamilyHandle*> handles;
  rocksdb::DB* raw = nullptr;
  rocksdb::Status s =
      rocksdb::DB::Open(raw_options_, db_path_, cf_descs, &handles, &raw);
  if (!s.ok()) Fail("failed to open DB: " + s.ToString());
  raw_db_.reset(raw);
  raw_cf_ = handles[0];
}

// Closes the way bit_lsm::BitLSM's destructor does.
void BitLSMGlobalBinding::CloseRaw() {
  raw_db_->DestroyColumnFamilyHandle(raw_cf_);
  raw_cf_ = nullptr;
  rocksdb::WaitForCompactOptions close_opts;
  close_opts.close_db = true;
  rocksdb::Status s = raw_db_->WaitForCompact(close_opts);
  if (!s.ok()) std::cerr << "Failed to close DB: " << s.ToString() << "\n";
  raw_db_.reset();
}

void BitLSMGlobalBinding::Put(const std::string& pk,
                               const std::vector<Attr>& attrs,
                               const std::string& payload) {
  if (!BuildMode()) Fail("Put on a DB opened for queries (no --bin_policy)");
  // BitLSM::Put: attr count check, v3 value encoding, one Put.
  if (attrs.size() != build_opts_.attr_num)
    Fail("the number of attrs does not match the schema");
  bit_lsm::EncodeValue(*layout_, attrs, payload, serialized_value_);
  rocksdb::Status s = raw_db_->Put(rocksdb::WriteOptions(), raw_cf_, pk,
                                   serialized_value_);
  if (!s.ok()) Fail("Put failed: " + s.ToString());
}

ScanResult BitLSMGlobalBinding::Scan(BitLSMQuery& query) {
  if (BuildMode()) Fail("Scan on a DB opened for building (--bin_policy)");
  return BitLSMBinding::Scan(query);
}

void BitLSMGlobalBinding::WaitForQuiescence() {
  if (!BuildMode()) return BitLSMBinding::WaitForQuiescence();
  rocksdb::WaitForCompactOptions wfco;
  wfco.flush = true;
  wfco.wait_for_purge = true;
  raw_db_->WaitForCompact(wfco);
}

void BitLSMGlobalBinding::Close() {
  if (!BuildMode()) {
    if (db_) CheckSstsUnchanged(db_->GetInternalDB(), db_path_, "at close");
    return BitLSMBinding::Close();
  }
  // A write-only load leaves compactions RocksDB schedules only later:
  // releasing the last snapshot marks bottommost files whose sequence numbers
  // can now be zeroed (DBImpl::ReleaseSnapshot -> UpdateOldestSnapshot), and a
  // write-only load takes no snapshot, so the first query's iterator is what
  // triggers it. Left alone, that query would rewrite those SSTs with the core
  // builder (a local-bins DB gets the same compaction on its first query).
  // Settle it here, under the global builder: reopen, take and release a
  // snapshot, drain, and repeat until a round changes nothing; only then
  // record the SST set.
  WaitForQuiescence();
  SstSet settled = LiveSsts(raw_db_.get());
  for (int round = 1;; ++round) {
    CloseRaw();
    OpenRaw();
    raw_db_->ReleaseSnapshot(raw_db_->GetSnapshot());
    WaitForQuiescence();
    SstSet now = LiveSsts(raw_db_.get());
    if (now == settled) break;
    if (round == 5) Fail("SST set still changing after 5 settle rounds");
    std::cout << "[BitLSMGlobalBinding] settle round " << round
              << " rewrote SSTs; draining again\n";
    settled = std::move(now);
  }
  WriteSstList(db_path_, settled);
  CloseRaw();
}

}  // namespace experiment
