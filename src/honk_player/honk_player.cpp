#include "binding.h"
#include "json_record_parser.h"
#include "resource_monitor.h"
#include "taxi_schema.h"
#include "tsv_parser.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sstream>
#include <thread>
#include <vector>

struct WriteItem {
  std::string pk;
  std::vector<Attr> attrs;
  std::string payload;
};

using namespace std;

int main(int argc, char* argv[]) {
  cxxopts::Options opts("honk-player", "Taxi data workload driver");
  opts.allow_unrecognised_options();
  // clang-format off
  opts.add_options()
    ("binding", "Method: bitlsm|no-index|si-ck|si-lu",
     cxxopts::value<string>())
    ("workload", "TSV workload file path",
     cxxopts::value<string>())
    ("db_path", "DB storage path",
     cxxopts::value<string>())
    ("output_dir", "Result CSV output directory",
     cxxopts::value<string>()->default_value("./result"))
    ("indexed_attrs", "Comma-separated attr names to index (default: all)",
     cxxopts::value<string>()->default_value(""))
    ("interleave", "Interleave mode: single CSV, per-op us latency",
     cxxopts::value<bool>()->default_value("false"))
    ("num_threads", "Number of writer threads (default: 1)",
     cxxopts::value<int>()->default_value("1"))
    ("no_csv", "Disable per-binding CSV output",
     cxxopts::value<bool>()->default_value("false"))
    ("query_limit", "Stop after N read queries (0 = unlimited)",
     cxxopts::value<uint64_t>()->default_value("0"))
    ("resource_monitor",
     "Sample RSS + per-thread CPU at every 1M checkpoint",
     cxxopts::value<bool>()->default_value("false"));
  // clang-format on

  auto result = opts.parse(argc, argv);

  if (!result.count("binding") || !result.count("workload") ||
      !result.count("db_path")) {
    cerr << "Required: --binding, --workload, --db_path\n";
    return 1;
  }

  string binding_name = result["binding"].as<string>();
  string workload_path = result["workload"].as<string>();
  string db_path = result["db_path"].as<string>();
  string output_dir = result["output_dir"].as<string>();

  bool interleave_mode = result["interleave"].as<bool>();
  int num_threads = result["num_threads"].as<int>();
  bool no_csv = result["no_csv"].as<bool>();
  uint64_t query_limit = result["query_limit"].as<uint64_t>();
  bool resource_monitor_enabled = result["resource_monitor"].as<bool>();

  pthread_setname_np(pthread_self(), "writer");

  // Parse indexed_attrs (field names → column indices)
  auto all_columns = honk::GetTaxiColumns();
  auto col_map = honk::BuildColumnIndexMap();
  vector<uint32_t> indexed_indices;
  string indexed_attrs_str = result["indexed_attrs"].as<string>();
  if (!indexed_attrs_str.empty()) {
    istringstream iss(indexed_attrs_str);
    string token;
    while (getline(iss, token, ',')) {
      auto it = col_map.find(token);
      if (it == col_map.end()) {
        cerr << "Unknown attribute name: \"" << token << "\"\n";
        cerr << "Available attributes:";
        for (auto& c : all_columns) cerr << " " << c.name;
        cerr << "\n";
        return 1;
      }
      indexed_indices.push_back(it->second);
    }
    // Rebuild col_map and all_columns with remapped indices so that
    // ParseFilters produces attr_idx values matching the indexed attr space.
    col_map.clear();
    vector<honk::TaxiColumn> remapped_columns;
    for (uint32_t i = 0; i < indexed_indices.size(); i++) {
      auto& orig = all_columns[indexed_indices[i]];
      col_map[orig.name] = i;
      remapped_columns.push_back(orig);
    }
    all_columns = std::move(remapped_columns);
  }

  // Create binding
  auto binding = experiment::CreateBinding(binding_name);
  if (!binding) {
    cerr << "Unknown binding: " << binding_name << "\n";
    return 1;
  }

  // Open DB with taxi schema
  auto taxi_opts = honk::BuildTaxiBitLSMOptions(indexed_indices);
  binding->Open(argc, argv, db_path, taxi_opts);

  // Prepare output
  filesystem::create_directories(output_dir);
  string workload_stem =
      filesystem::path(workload_path).stem().string();
  string file_prefix = output_dir + "/" + workload_stem + "_" +
                        binding->Name() + binding->ParamSuffix();

  std::optional<ResourceMonitor> monitor;
  if (resource_monitor_enabled) {
    monitor.emplace(file_prefix + "_sample_log.csv",
                    file_prefix + "_thread_log.csv");
  }

  // Interleave mode: single CSV, opened on first operation
  ofstream interleave_csv;
  bool in_interleave_phase = false;
  string interleave_path = output_dir + "/interleave_" +
      binding->Name() + binding->ParamSuffix() + ".csv";
  auto ensure_interleave_csv = [&] {
    if (no_csv) return;
    if (!interleave_csv.is_open()) {
      interleave_csv.open(interleave_path);
      interleave_csv << "elapsed_us,records_written,op_type,latency_us\n";
    }
  };

  // Normal mode: CSV files are created lazily — only when the first write/read occurs.
  ofstream write_csv;
  ofstream read_csv;
  auto ensure_write_csv = [&] {
    if (no_csv) return;
    if (!write_csv.is_open()) {
      write_csv.open(file_prefix + "_write_log.csv");
      write_csv << "time_elapsed_ms,records_written\n";
    }
  };
  auto ensure_read_csv = [&] {
    if (no_csv) return;
    if (!read_csv.is_open()) {
      read_csv.open(file_prefix + "_read_log.csv");
      read_csv << "query_id,query_attr_num,filter_attrs,time_elapsed_ms,"
                  "records_matched,records_total,selectivity_actual\n";
    }
  };

  // --- Multi-threaded write path (pre-parse → parallel write) ---
  if (num_threads > 1) {
    // Phase 1: pre-parse all write records into memory
    cout << "[pre-parse] loading workload into memory...\n";
    auto parse_start = chrono::steady_clock::now();
    honk::RecordParser record_parser(indexed_indices);
    vector<Attr> tmp_attrs;
    string tmp_payload;
    honk::TSVReader reader(workload_path);
    honk::Operation op;
    vector<WriteItem> all_items;
    all_items.reserve(50'000'000);

    while (reader.Next(op)) {
      if (op.type == honk::OpType::WRITE || op.type == honk::OpType::UPDATE) {
        auto& w = get<honk::WriteOp>(op.data);
        record_parser.ParseRecord(w.json, tmp_attrs, tmp_payload);
        all_items.push_back(WriteItem{w.pk, tmp_attrs, tmp_payload});
      }
    }

    auto parse_elapsed = chrono::duration_cast<chrono::milliseconds>(
                             chrono::steady_clock::now() - parse_start).count();
    cout << "[pre-parse] " << all_items.size() << " records loaded in "
         << parse_elapsed << "ms" << endl;

    // Phase 2: parallel write — each thread writes its own segment
    uint64_t total_n = all_items.size();
    std::atomic<uint64_t> total_writes{0};
    std::mutex io_mu;
    auto wall_start = chrono::steady_clock::now();

    vector<thread> workers;
    for (int t = 0; t < num_threads; t++) {
      workers.emplace_back([&, t] {
        for (uint64_t i = t; i < total_n; i += num_threads) {
          auto& item = all_items[i];
          binding->Put(item.pk, item.attrs, item.payload);
          uint64_t prev = total_writes.fetch_add(1, std::memory_order_relaxed);
          uint64_t cur = prev + 1;
          if (cur % 1'000'000 == 0) {
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                               now - wall_start).count();
            std::lock_guard<std::mutex> lock(io_mu);
            if (!no_csv) {
              ensure_write_csv();
              write_csv << elapsed << "," << cur << "\n";
              write_csv.flush();
            }
            if (monitor) monitor->Sample(cur);
            cout << "[write] " << cur << " records, " << elapsed << "ms" << endl;
          }
        }
      });
    }

    for (auto& w : workers) w.join();

    uint64_t writes = total_writes.load();
    auto put_elapsed = chrono::duration_cast<chrono::milliseconds>(
                           chrono::steady_clock::now() - wall_start).count();

    binding->Close();

    auto total_elapsed = chrono::duration_cast<chrono::milliseconds>(
                             chrono::steady_clock::now() - wall_start).count();
    cout << "\n=== Summary ===\n"
         << "Binding: " << binding->Name() << binding->ParamSuffix() << "\n"
         << "Total writes: " << writes << "\n"
         << "Total reads: 0\n"
         << "Total time: " << put_elapsed << "ms\n"
         << "Total time (incl. compaction): " << total_elapsed << "ms\n";
    return 0;
  }

  // --- Single-threaded path (existing logic) ---
  // Prepare parsers (reusable buffers)
  honk::RecordParser record_parser(indexed_indices);
  vector<Attr> attrs;
  string payload;

  // TSV dispatch
  honk::TSVReader reader(workload_path);
  honk::Operation op;

  uint64_t writes = 0, reads = 0;
  auto wall_start = chrono::steady_clock::now();
  auto interleave_start = chrono::high_resolution_clock::now();
  bool interleave_started = false;

  while (reader.Next(op) &&
         (query_limit == 0 || reads < query_limit)) {
    switch (op.type) {
      case honk::OpType::WRITE:
      case honk::OpType::UPDATE: {
        auto& w = get<honk::WriteOp>(op.data);
        record_parser.ParseRecord(w.json, attrs, payload);
        if (interleave_mode) {
          if (!interleave_started) {
            interleave_start = chrono::high_resolution_clock::now();
            interleave_started = true;
          }
          auto t0 = chrono::high_resolution_clock::now();
          binding->Put(w.pk, attrs, payload);
          auto t1 = chrono::high_resolution_clock::now();
          auto latency = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
          writes++;
          if (writes % 1'000 == 0 && !no_csv) {
            ensure_interleave_csv();
            auto elapsed = chrono::duration_cast<chrono::microseconds>(t1 - interleave_start).count();
            interleave_csv << elapsed << "," << writes << ",PUT," << latency << "\n";
          }
          if (writes % 1'000'000 == 0) {
            if (!no_csv) interleave_csv.flush();
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                               now - wall_start).count();
            const char* phase = in_interleave_phase ? "interleave" : "pre-load";
            cout << "[" << phase << "] " << writes << " records, " << elapsed << "ms" << endl;
          }
        } else {
          binding->Put(w.pk, attrs, payload);
          writes++;
          if (writes % 1'000'000 == 0) {
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                               now - wall_start)
                               .count();
            if (!no_csv) {
              ensure_write_csv();
              write_csv << elapsed << "," << writes << "\n";
              write_csv.flush();
            }
            if (monitor) monitor->Sample(writes);
            cout << "[write] " << writes << " records, " << elapsed << "ms" << endl;
          }
        }
        break;
      }
      case honk::OpType::READ: {
        auto& r = get<honk::ReadOp>(op.data);
        auto [query, attr_names, k, hint_attr] =
            honk::ParseFilters(r.json, all_columns, col_map);
        // Oracle hint: move clauses for the most-selective attr to the front
        // so that MapQueryToSILookups places it at si_lookups[0] for PF.
        if (hint_attr != UINT32_MAX) {
          auto& cg = query.clause_groups;
          std::stable_partition(cg.begin(), cg.end(),
              [hint_attr](const bit_lsm::OrClause& clause) {
                return !clause.empty() && clause[0].attr_idx == hint_attr;
              });
        }
        if (interleave_mode) {
          if (!in_interleave_phase) {
            in_interleave_phase = true;
            cout << "[interleave] phase begins at " << writes << " records\n";
          }
          auto t0 = chrono::high_resolution_clock::now();
          binding->Scan(query);
          auto t1 = chrono::high_resolution_clock::now();
          auto latency = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
          auto elapsed = chrono::duration_cast<chrono::microseconds>(t1 - interleave_start).count();
          if (!no_csv) {
            ensure_interleave_csv();
            interleave_csv << elapsed << "," << writes << ",QUERY," << latency << "\n";
          }
          reads++;
          cout << "[interleave] QUERY #" << reads << " at " << writes << " records, " << latency << "us\n";
        } else {
          auto scan_result = binding->Scan(query);
          if (!no_csv) {
            ensure_read_csv();
            double selectivity =
                writes > 0
                    ? static_cast<double>(scan_result.matched) / writes
                    : 0.0;
            read_csv << reads << "," << k << ",\"" << attr_names << "\","
                     << scan_result.elapsed_ms << "," << scan_result.matched
                     << "," << writes << "," << fixed << setprecision(6)
                     << selectivity << "\n";
            read_csv.flush();
          }
          reads++;
        }
        break;
      }
      case honk::OpType::PAUSE: {
        auto& p = get<honk::PauseOp>(op.data);
        cout << "[pause] " << p.seconds << "s\n";
        this_thread::sleep_for(chrono::duration<double>(p.seconds));
        break;
      }
    }
  }

  auto put_elapsed = chrono::duration_cast<chrono::milliseconds>(
                         chrono::steady_clock::now() - wall_start)
                         .count();

  binding->Close();

  auto total_elapsed = chrono::duration_cast<chrono::milliseconds>(
                           chrono::steady_clock::now() - wall_start)
                           .count();
  cout << "\n=== Summary ===\n"
       << "Binding: " << binding->Name() << binding->ParamSuffix() << "\n"
       << "Total writes: " << writes << "\n"
       << "Total reads: " << reads << "\n"
       << "Total time: " << put_elapsed << "ms\n"
       << "Total time (incl. compaction): " << total_elapsed << "ms\n";

  return 0;
}
