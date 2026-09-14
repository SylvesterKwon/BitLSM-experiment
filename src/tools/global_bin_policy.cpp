// Oracle bin policy for the global-bins ablation (bitlsm-global binding).
//
// Feeds every row of a write workload through GlobalSABIBuilder with no
// policy -- the core SABI builder's own code -- as if the whole dataset were
// one SST, and saves the binning that produces: per attribute the bin count
// the attr_num / rho greedy allots over the full data, the equi-depth range
// boundaries, and the categorical value -> bin table. Rows are encoded exactly
// as BitLSM::Put encodes them, so the policy lives in the same byte domain as
// the SSTs it will be applied to.
//
//   global_bin_policy --workload workloads/<write>.tsv \
//       --indexed_attrs PULocationID,... --rho 0.001 --output <policy.bin>
//
// Also writes <output>.txt, a readable summary of the bin counts and range
// ends.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <cxxopts.hpp>

#include "global_bins/bin_policy.h"
#include "global_bins/global_sabi_builder.h"
#include "json_record_parser.h"
#include "taxi_schema.h"
#include "tsv_parser.h"

namespace {

long PeakRssMb() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmHWM:", 0) == 0) return std::stol(line.substr(6)) / 1024;
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  cxxopts::Options opts("global_bin_policy",
                        "Compute the oracle bin policy of a write workload");
  // clang-format off
  opts.add_options()
    ("workload", "Write workload TSV", cxxopts::value<std::string>())
    ("indexed_attrs", "Comma-separated taxi attrs, as used when building",
     cxxopts::value<std::string>())
    ("rho", "BitLSM rho the DBs are built with",
     cxxopts::value<double>()->default_value("0.001"))
    ("output", "Policy file path", cxxopts::value<std::string>());
  // clang-format on
  auto result = opts.parse(argc, argv);
  if (!result.count("workload") || !result.count("indexed_attrs") ||
      !result.count("output")) {
    std::cerr << "Required: --workload, --indexed_attrs, --output\n";
    return 1;
  }
  const std::string workload = result["workload"].as<std::string>();
  const std::string output = result["output"].as<std::string>();
  const double rho = result["rho"].as<double>();

  auto col_map = honk::BuildColumnIndexMap();
  std::vector<uint32_t> indexed;
  std::vector<std::string> attr_names;
  std::istringstream iss(result["indexed_attrs"].as<std::string>());
  for (std::string tok; std::getline(iss, tok, ',');) {
    auto it = col_map.find(tok);
    if (it == col_map.end()) {
      std::cerr << "Unknown attribute: " << tok << "\n";
      return 1;
    }
    indexed.push_back(it->second);
    attr_names.push_back(tok);
  }

  bit_lsm::BitLSMOptions bitlsm_opts = honk::BuildTaxiBitLSMOptions(indexed);
  bitlsm_opts.rho = rho;
  experiment::global_bins::GlobalSABIBuilder builder(
      bit_lsm::SABISchema::FromOptions(bitlsm_opts),
      std::make_unique<bit_lsm::ValueLayoutExtractor>(bitlsm_opts), nullptr);

  honk::RecordParser parser(indexed);
  const bit_lsm::ValueLayout layout(bitlsm_opts);
  honk::TSVReader reader(workload);
  honk::Operation op;
  std::vector<Attr> attrs;
  std::string payload, value;
  uint64_t rows = 0;
  while (reader.Next(op)) {
    // An update would need newest-wins deduplication to describe the final
    // data; the write workloads this serves have none, so refuse rather than
    // bin stale versions.
    if (op.type != honk::OpType::WRITE) {
      std::cerr << "Only write-only workloads are supported (found a "
                   "non-write operation at row " << rows << ")\n";
      return 1;
    }
    const auto& w = std::get<honk::WriteOp>(op.data);
    parser.ParseRecord(w.json, attrs, payload);
    bit_lsm::EncodeValue(layout, attrs, payload, value);
    builder.OnKeyAdded(w.pk, rocksdb::UserDefinedIndexBuilder::kValue, value);
    if (++rows % 10'000'000 == 0)
      std::cout << "[policy] " << rows << " rows, peak RSS " << PeakRssMb()
                << " MB" << std::endl;
    if (rows == UINT32_MAX) {
      std::cerr << "More rows than one SABI row id space holds\n";
      return 1;
    }
  }

  experiment::global_bins::BinPolicy policy = builder.ExportLocalPolicy();
  policy.source = workload;
  policy.source_bytes = std::filesystem::file_size(workload);
  rocksdb::Status s = experiment::global_bins::SaveBinPolicy(policy, output);
  if (!s.ok()) {
    std::cerr << s.ToString() << "\n";
    return 1;
  }

  std::ofstream txt(output + ".txt");
  txt << "source " << policy.source << " (" << policy.source_bytes
      << " bytes)\nrows " << policy.rows << "\nrho " << policy.rho << "\n";
  uint32_t total = 0;
  for (size_t i = 0; i < attr_names.size(); ++i) {
    total += policy.bitmap_nums[i];
    txt << attr_names[i] << " bins=" << policy.bitmap_nums[i];
    if (const auto* b =
            std::get_if<bit_lsm::BytesList>(&policy.binning_policy[i])) {
      txt << std::setprecision(17)
          << " min=" << bit_lsm::OkeyToF64(bit_lsm::OkeyFromBytes((*b)[0]))
          << " max=" << bit_lsm::OkeyToF64(bit_lsm::OkeyFromBytes(b->back()));
    } else {
      txt << " values="
          << std::get<std::vector<std::pair<std::string, uint32_t>>>(
                 policy.binning_policy[i])
                 .size();
    }
    txt << "\n";
  }
  txt << "total_bins " << total << "\n";
  std::cout << "[policy] " << rows << " rows -> " << output << " (" << total
            << " bins, peak RSS " << PeakRssMb() << " MB)\n";
  return 0;
}
