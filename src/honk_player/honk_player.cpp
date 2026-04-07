#include "binding.h"
#include "json_record_parser.h"
#include "taxi_schema.h"
#include "tsv_parser.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <vector>

struct WriteItem {
  std::string pk;
  std::vector<Attr> attrs;
  std::string payload;
};

template <typename T>
class BoundedQueue {
  std::queue<T> queue_;
  std::mutex mu_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  size_t capacity_;
  bool done_ = false;

 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

  void Push(T item) {
    std::unique_lock<std::mutex> lock(mu_);
    not_full_.wait(lock, [this] { return queue_.size() < capacity_; });
    queue_.push(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
  }

  bool Pop(T& item) {
    std::unique_lock<std::mutex> lock(mu_);
    not_empty_.wait(lock, [this] { return !queue_.empty() || done_; });
    if (queue_.empty() && done_) return false;
    item = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    not_full_.notify_one();
    return true;
  }

  void Close() {
    std::lock_guard<std::mutex> lock(mu_);
    done_ = true;
    not_empty_.notify_all();
  }
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

  // --- Multi-threaded write path ---
  if (num_threads > 1) {
    BoundedQueue<WriteItem> queue(4096);
    std::atomic<uint64_t> total_writes{0};
    auto wall_start = chrono::steady_clock::now();

    // CSV output (protected by mutex since workers write checkpoints)
    ensure_write_csv();
    std::mutex io_mu;

    // Worker threads: dequeue and Put()
    vector<thread> workers;
    for (int t = 0; t < num_threads; t++) {
      workers.emplace_back([&] {
        WriteItem item;
        while (queue.Pop(item)) {
          binding->Put(item.pk, item.attrs, item.payload);
          uint64_t prev = total_writes.fetch_add(1, std::memory_order_relaxed);
          uint64_t cur = prev + 1;
          if (cur % 1'000'000 == 0) {
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                               now - wall_start).count();
            std::lock_guard<std::mutex> lock(io_mu);
            if (!no_csv) {
              write_csv << elapsed << "," << cur << "\n";
              write_csv.flush();
            }
            cout << "[write] " << cur << " records, " << elapsed << "ms\n";
          }
        }
      });
    }

    // Producer: read TSV, parse, push to queue
    honk::RecordParser record_parser(indexed_indices);
    vector<Attr> attrs;
    string payload;
    honk::TSVReader reader(workload_path);
    honk::Operation op;

    while (reader.Next(op)) {
      if (op.type == honk::OpType::WRITE || op.type == honk::OpType::UPDATE) {
        auto& w = get<honk::WriteOp>(op.data);
        record_parser.ParseRecord(w.json, attrs, payload);
        queue.Push(WriteItem{w.pk, attrs, payload});
      }
    }

    queue.Close();
    for (auto& w : workers) w.join();

    binding->Close();

    uint64_t writes = total_writes.load();
    auto total_elapsed = chrono::duration_cast<chrono::milliseconds>(
                             chrono::steady_clock::now() - wall_start).count();
    cout << "\n=== Summary ===\n"
         << "Binding: " << binding->Name() << binding->ParamSuffix() << "\n"
         << "Total writes: " << writes << "\n"
         << "Total reads: 0\n"
         << "Total time: " << total_elapsed << "ms\n";
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

  while (reader.Next(op)) {
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
            cout << "[" << phase << "] " << writes << " records, " << elapsed << "ms\n";
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
            cout << "[write] " << writes << " records, " << elapsed << "ms\n";
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

  binding->Close();

  auto total_elapsed = chrono::duration_cast<chrono::milliseconds>(
                           chrono::steady_clock::now() - wall_start)
                           .count();
  cout << "\n=== Summary ===\n"
       << "Binding: " << binding->Name() << binding->ParamSuffix() << "\n"
       << "Total writes: " << writes << "\n"
       << "Total reads: " << reads << "\n"
       << "Total time: " << total_elapsed << "ms\n";

  return 0;
}
