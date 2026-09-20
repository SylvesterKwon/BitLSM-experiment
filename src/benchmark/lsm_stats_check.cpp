// Gate for Binding::SampleLsmStats(): opens each RocksDB-backed binding on a
// throwaway DB, writes enough to fill a memtable, and checks that the sample
// reflects it before and after WaitForQuiescence(). no-index has no hook and
// must report ok=false. Exit 0 and print "LSM_STATS OK" on success.
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "binding.h"
#include "bit_lsm_option.h"

using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::IndexType;
using bit_lsm::PhysicalType;
using experiment::Attr;
using experiment::LsmStats;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond   \
                << "\n";                                                  \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: lsm_stats_check <bitlsm_path> <embedded_path> "
                 "<postings_path>\n";
    return 2;
  }
  BitLSMOptions opts;
  opts.attr_num = 2;
  opts.attr_specs = {
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary, 0),
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)};
  opts.rho = 0.1;

  const char* methods[3] = {"bitlsm", "embedded", "embedded-postings"};
  for (int m = 0; m < 3; ++m) {
    std::vector<std::string> a = {methods[m], "--rho", "0.1", "--bloom_bits",
                                  "10"};
    std::vector<char*> av;
    for (auto& s : a) av.push_back(const_cast<char*>(s.c_str()));
    auto b = experiment::CreateBinding(methods[m]);
    b->Open(static_cast<int>(av.size()), av.data(), argv[m + 1], opts);

    // 200k rows x ~100 B stay in the memtable (~20 MB, under the 64 MB
    // default write buffer); WaitForQuiescence() then flushes them, so the
    // quiet sample must show at least one flush.
    std::vector<Attr> attrs(2);
    std::string payload(64, 'p');
    for (int i = 0; i < 200000; ++i) {
      attrs[0] = std::string("c") + std::to_string(i % 8);
      attrs[1] = static_cast<double>(i);
      b->Put("k" + std::to_string(i), attrs, payload);
    }
    LsmStats live = b->SampleLsmStats();
    CHECK(live.ok);
    CHECK(live.memtable_bytes > 0);
    CHECK(live.stall_stops == 0);

    b->WaitForQuiescence();
    LsmStats quiet = b->SampleLsmStats();
    CHECK(quiet.ok);
    CHECK(quiet.running_flushes == 0);
    CHECK(quiet.running_compactions == 0);
    CHECK(quiet.flush_count >= 1);
    CHECK(quiet.flush_bytes > 0);
    CHECK(quiet.compact_write_bytes >= quiet.flush_bytes);
    // The flushed file sits in L0 unless a real compaction (beyond the
    // flushes counted in L0.CompCount) moved it. num-files-at-level0 is a
    // string property; reading it as an int silently yields 0.
    CHECK(quiet.l0_files >= 1 || quiet.compaction_count > quiet.flush_count);
    std::cout << methods[m] << ": flushes=" << quiet.flush_count
              << " compactions=" << quiet.compaction_count
              << " flush_bytes=" << quiet.flush_bytes
              << " l0_files=" << quiet.l0_files << "\n";
    b->Close();
  }

  auto ni = experiment::CreateBinding("no-index");
  CHECK(!ni->SampleLsmStats().ok);

  std::cout << "LSM_STATS OK\n";
  return 0;
}
