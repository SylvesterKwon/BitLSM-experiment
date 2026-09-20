// qui_player: query-under-ingestion driver.
//
// One update thread issues full-record overwrites of existing keys at a fixed
// rate (open loop, schedule from qui::Pacer), one query thread runs the query
// set closed-loop (next query as soon as the previous returns), one sampler
// thread records the LSM's background state. Time-bounded by --duration_s.
// Everything the update thread needs is parsed before t0, so the measured
// window contains only Put calls at their scheduled times.
//
// Outputs, all under --output_dir with prefix
// <query stem>_<binding><suffix>_w<rate>:
//   _query_log.csv  one row per query
//   _write_log.csv  one row per elapsed second of the update thread
//   _lsm_log.csv    one row per --sample_interval_s
//   _meta.json      run parameters, timings, totals, LSM state at t0/T/drain
#include "binding.h"
#include "json_record_parser.h"
#include "qui_pacer.h"
#include "taxi_schema.h"
#include "tsv_parser.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pthread.h>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;
using json = nlohmann::json;

struct WriteItem {
  string pk;
  vector<Attr> attrs;
  string payload;
};

struct Config {
  string binding, db_path, query_workload, update_workload, output_dir;
  string indexed_attrs;
  double update_rate = 0;      // updates per second; 0 = no update thread
  double duration_s = 1800;
  double window_s = 60;        // progress print interval
  double sample_interval_s = 1;
  uint64_t seed = 42;
  bool settle_only = false;
  bool verify = false;
  string base_workload;        // --verify: the w-trace that built the DB
};

// Taxi column set remapped to the indexed-attribute space, exactly as
// honk_player does it, so ParseFilters yields attr_idx values the binding
// understands.
struct Columns {
  vector<uint32_t> indexed_indices;
  vector<honk::TaxiColumn> columns;
  unordered_map<string, uint32_t> col_map;
};

static Columns SetupColumns(const string& indexed_attrs) {
  Columns c;
  c.columns = honk::GetTaxiColumns();
  c.col_map = honk::BuildColumnIndexMap();
  if (indexed_attrs.empty()) return c;
  istringstream iss(indexed_attrs);
  string token;
  while (getline(iss, token, ',')) {
    auto it = c.col_map.find(token);
    if (it == c.col_map.end()) {
      cerr << "Unknown attribute name: \"" << token << "\"\n";
      exit(1);
    }
    c.indexed_indices.push_back(it->second);
  }
  c.col_map.clear();
  vector<honk::TaxiColumn> remapped;
  for (uint32_t i = 0; i < c.indexed_indices.size(); i++) {
    auto& orig = c.columns[c.indexed_indices[i]];
    c.col_map[orig.name] = i;
    remapped.push_back(orig);
  }
  c.columns = std::move(remapped);
  return c;
}

static long ReadStatusKb(const char* field) {
  ifstream f("/proc/self/status");
  string line;
  const size_t n = strlen(field);
  while (getline(f, line)) {
    if (line.compare(0, n, field) == 0 && line.size() > n && line[n] == ':') {
      long kb = 0;
      sscanf(line.c_str() + n + 1, "%ld", &kb);
      return kb;
    }
  }
  return 0;
}

static int64_t ElapsedNs(Clock::time_point t0) {
  return chrono::duration_cast<chrono::nanoseconds>(Clock::now() - t0).count();
}

// "w1000" for integral rates, "w0.5" otherwise.
static string RatePrefix(double rate) {
  char buf[32];
  if (rate == floor(rate))
    snprintf(buf, sizeof buf, "w%lld", static_cast<long long>(rate));
  else
    snprintf(buf, sizeof buf, "w%g", rate);
  return buf;
}

static json LsmToJson(const experiment::LsmStats& s) {
  return json{{"ok", s.ok},
              {"running_flushes", s.running_flushes},
              {"running_compactions", s.running_compactions},
              {"flush_pending", s.flush_pending},
              {"compaction_pending", s.compaction_pending},
              {"pending_compaction_bytes", s.pending_compaction_bytes},
              {"l0_files", s.l0_files},
              {"memtable_bytes", s.memtable_bytes},
              {"immutable_memtables", s.immutable_memtables},
              {"write_stopped", s.write_stopped},
              {"delayed_write_rate", s.delayed_write_rate},
              {"stall_stops", s.stall_stops},
              {"stall_delays", s.stall_delays},
              {"flush_bytes", s.flush_bytes},
              {"compact_read_bytes", s.compact_read_bytes},
              {"compact_write_bytes", s.compact_write_bytes},
              {"flush_count", s.flush_count},
              {"compaction_count", s.compaction_count}};
}

// The query set: r-lines kept as their filter JSON. BitLSMQuery is consumed
// by Scan(), so each issue re-parses (microseconds) instead of copying.
static vector<string> LoadQueries(const string& path) {
  vector<string> out;
  honk::TSVReader reader(path);
  honk::Operation op;
  while (reader.Next(op))
    if (op.type == honk::OpType::READ)
      out.push_back(get<honk::ReadOp>(op.data).json);
  if (out.empty()) {
    cerr << "No r lines in " << path << "\n";
    exit(1);
  }
  return out;
}

// The first `max_items` w/u lines, parsed into what Put() takes. Same shape
// as honk_player's --num_threads preload.
static vector<WriteItem> LoadUpdates(const string& path, uint64_t max_items,
                                     const Columns& cols) {
  vector<WriteItem> items;
  if (max_items == 0) return items;
  honk::RecordParser parser(cols.indexed_indices);
  honk::TSVReader reader(path);
  honk::Operation op;
  vector<Attr> attrs;
  string payload;
  items.reserve(max_items);
  while (items.size() < max_items && reader.Next(op)) {
    if (op.type != honk::OpType::WRITE && op.type != honk::OpType::UPDATE)
      continue;
    auto& w = get<honk::WriteOp>(op.data);
    parser.ParseRecord(w.json, attrs, payload);
    items.push_back(WriteItem{w.pk, attrs, payload});
  }
  if (items.empty()) {
    cerr << "No w/u lines in " << path << "\n";
    exit(1);
  }
  return items;
}

// Filters JSON -> query with the oracle hint applied, as honk_player does.
static honk::FilterResult MakeQuery(const string& filter_json,
                                    const Columns& cols) {
  auto fr = honk::ParseFilters(filter_json, cols.columns, cols.col_map);
  if (fr.hint_most_selective_attr != UINT32_MAX) {
    uint32_t hint = fr.hint_most_selective_attr;
    auto& cg = fr.query.clause_groups;
    stable_partition(cg.begin(), cg.end(), [hint](const bit_lsm::OrClause& c) {
      return !c.empty() && c[0].attr_idx == hint;
    });
  }
  return fr;
}

// One query (creates and releases a snapshot; on a fresh DB RocksDB then marks
// bottommost files with live sequence numbers for compaction) followed by a
// full drain, so that compaction lands before t0 rather than inside the
// window. Cheap when the base DB was settled once already.
static json Settle(experiment::Binding* b, const vector<string>& queries,
                   const Columns& cols) {
  auto before = b->SampleLsmStats();
  auto t = Clock::now();
  auto fr = MakeQuery(queries[0], cols);
  auto r = b->Scan(fr.query);
  int64_t scan_ms = ElapsedNs(t) / 1'000'000;
  b->WaitForQuiescence();
  int64_t total_ms = ElapsedNs(t) / 1'000'000;
  auto after = b->SampleLsmStats();
  cout << "[settle] scan " << scan_ms << " ms (" << r.matched
       << " rows), drained at " << total_ms << " ms\n";
  return json{{"scan_ms", scan_ms},
              {"total_ms", total_ms},
              {"matched", r.matched},
              {"lsm_before", LsmToJson(before)},
              {"lsm_after", LsmToJson(after)}};
}

static json EnvJson() {
  json e;
  for (const char* k : {"EXP_BLOCK_CACHE_MB", "EXP_CACHE_STATS", "EXP_IO_URING",
                        "EXP_DIRECT_IO", "EXP_SHADOW_STATS"}) {
    const char* v = getenv(k);
    e[k] = v ? json(v) : json(nullptr);
  }
  return e;
}

// Shared between the three threads and main. Counters are relaxed atomics:
// they are progress indicators, the logs carry the measurements.
struct RunState {
  Clock::time_point t0;
  int64_t duration_ns = 0;
  atomic<bool> stop{false};       // set by main at t0 + T
  atomic<bool> threads_done{false};
  atomic<bool> failed{false};
  atomic<uint64_t> issued{0};     // Puts started
  atomic<uint64_t> completed{0};  // queries whose end <= T
  // --verify (Task 6): newest-wins table of indexed attrs, guarded by mu.
  mutex verify_mu;
  unordered_map<string, vector<Attr>> truth;
  vector<string> update_log;      // pk per applied update, in issue order
  uint64_t applied = 0;           // updates applied to `truth`
  bool verify = false;
};

static void SleepUntilNs(Clock::time_point t0, int64_t due_ns) {
  auto tp = t0 + chrono::nanoseconds(due_ns);
  // Coarse sleep, then spin the last stretch: sleep_until alone overshoots by
  // tens of microseconds, which at high rates is a large share of the period.
  this_thread::sleep_until(tp - chrono::microseconds(200));
  while (Clock::now() < tp) {
  }
}

struct WriterTotals {
  uint64_t scheduled = 0, issued = 0, done_in_window = 0,
           put_completed_after_end = 0, unissued = 0;
  bool wrapped = false;
};

// Open loop: update i is due at Pacer::DueNs(i); if the thread is behind it
// issues back-to-back without sleeping and never drops. One CSV row per
// elapsed second, dense (a Put that stalls across seconds yields rows with
// zero count and a growing backlog).
static WriterTotals WriterLoop(experiment::Binding* b,
                               const vector<WriteItem>& items,
                               const qui::Pacer& pacer, RunState& st,
                               ofstream& csv) {
  pthread_setname_np(pthread_self(), "writer");
  csv << "sec,scheduled_cum,issued_cum,backlog_end,lag_mean_us,lag_max_us,"
         "put_mean_us,put_max_us,put_count\n";
  struct Bin {
    uint64_t count = 0;
    int64_t lag_sum = 0, lag_max = 0, put_sum = 0, put_max = 0;
  };
  int64_t cur_sec = 0;
  Bin bin;
  uint64_t issued = 0;
  WriterTotals tot;

  auto flush_through = [&](int64_t now_ns) {
    // Emit rows for every whole second that ended before now_ns.
    int64_t sec = now_ns / 1'000'000'000LL;
    for (; cur_sec < sec; ++cur_sec) {
      int64_t end_ns = (cur_sec + 1) * 1'000'000'000LL;
      csv << cur_sec << "," << pacer.ScheduledBy(end_ns) << "," << issued << ","
          << pacer.Backlog(end_ns, issued) << ","
          << (bin.count ? bin.lag_sum / static_cast<int64_t>(bin.count) / 1000 : 0)
          << "," << bin.lag_max / 1000 << ","
          << (bin.count ? bin.put_sum / static_cast<int64_t>(bin.count) / 1000 : 0)
          << "," << bin.put_max / 1000 << "," << bin.count << "\n";
      bin = Bin{};
    }
  };

  for (uint64_t i = 0;; ++i) {
    int64_t due = pacer.DueNs(i);
    if (due >= st.duration_ns) break;
    int64_t now = ElapsedNs(st.t0);
    if (now < due) {
      SleepUntilNs(st.t0, due);
      now = ElapsedNs(st.t0);
    }
    if (st.stop.load(memory_order_relaxed) || st.failed.load()) break;
    flush_through(now);
    if (i >= items.size()) tot.wrapped = true;
    const WriteItem& it = items[i % items.size()];
    b->Put(it.pk, it.attrs, it.payload);
    int64_t done = ElapsedNs(st.t0);
    ++issued;
    st.issued.store(issued, memory_order_relaxed);
    if (st.verify) {
      lock_guard<mutex> g(st.verify_mu);
      st.truth[it.pk] = it.attrs;
      st.update_log.push_back(it.pk);
      st.applied = issued;
    }
    int64_t lag = now - due, put = done - now;
    ++bin.count;
    bin.lag_sum += lag;
    bin.lag_max = max(bin.lag_max, lag);
    bin.put_sum += put;
    bin.put_max = max(bin.put_max, put);
    if (done <= st.duration_ns) ++tot.done_in_window;
    else ++tot.put_completed_after_end;
  }
  flush_through(max(ElapsedNs(st.t0), st.duration_ns));
  csv.flush();
  tot.issued = issued;
  tot.scheduled = pacer.ScheduledBy(st.duration_ns);
  tot.unissued = tot.scheduled > issued ? tot.scheduled - issued : 0;
  return tot;
}

struct QueryTotals {
  uint64_t started = 0, completed_in_window = 0, in_flight_at_end = 0;
  uint64_t verify_failures = 0;
};

// Independent predicate evaluation over raw attrs: OR inside a clause, AND
// across clauses; the drivers only emit string equality and double bounds.
static bool CondHolds(const bit_lsm::QueryCondition& c, const Attr& a) {
  if (holds_alternative<string>(c.value))
    return c.op == bit_lsm::CompareOp::EQUAL &&
           holds_alternative<string>(a) && get<string>(a) == get<string>(c.value);
  if (!holds_alternative<double>(a) || !holds_alternative<double>(c.value))
    return false;
  double got = get<double>(a), want = get<double>(c.value);
  switch (c.op) {
    case bit_lsm::CompareOp::EQUAL: return got == want;
    case bit_lsm::CompareOp::LESS_EQUAL: return got <= want;
    case bit_lsm::CompareOp::GREATER_EQUAL: return got >= want;
    case bit_lsm::CompareOp::LESS: return got < want;
    case bit_lsm::CompareOp::GREATER: return got > want;
  }
  return false;
}

static bool RowMatches(const bit_lsm::BitLSMQuery& q, const vector<Attr>& row) {
  for (const auto& clause : q.clause_groups) {
    bool any = false;
    for (const auto& cond : clause)
      if (CondHolds(cond, row[cond.attr_idx])) { any = true; break; }
    if (!any) return false;
  }
  return true;
}

// Let U be the keys updated between `applied_before` (read before the scan)
// and now. Keys outside U have one state at the scan's snapshot and in the
// table, so their membership is fixed; keys in U may be either version:
// |truth \ U| <= matched <= |truth \ U| + |U|.
static bool VerifyOne(const bit_lsm::BitLSMQuery& q, uint64_t applied_before,
                      uint64_t matched, uint64_t seq, RunState& st) {
  lock_guard<mutex> g(st.verify_mu);
  unordered_set<string> u(st.update_log.begin() + applied_before,
                          st.update_log.begin() + st.applied);
  uint64_t lo = 0;
  for (const auto& kv : st.truth)
    if (!u.count(kv.first) && RowMatches(q, kv.second)) ++lo;
  uint64_t hi = lo + u.size();
  if (matched < lo || matched > hi) {
    cerr << "[verify] FAIL seq=" << seq << " matched=" << matched
         << " expected in [" << lo << ", " << hi << "] (|U|=" << u.size()
         << ")\n";
    return false;
  }
  return true;
}

// Closed loop over the query set. Each pass is a seeded shuffle (seed + pass),
// identical across methods, so a 60 s window holds a random mix of the set
// rather than a fixed slice of it.
static QueryTotals QueryLoop(experiment::Binding* b, const vector<string>& queries,
                             const Columns& cols, uint64_t seed, RunState& st,
                             ofstream& csv) {
  pthread_setname_np(pthread_self(), "query");
  csv << "seq,pass,query_id,query_attr_num,filter_attrs,start_ms,end_ms,"
         "latency_us,records_matched,in_window\n";
  QueryTotals tot;
  vector<size_t> order(queries.size());
  size_t pos = order.size();
  uint64_t pass = 0;
  for (uint64_t seq = 0; !st.stop.load(memory_order_relaxed) && !st.failed.load(); ++seq) {
    // No query starts at or after T; main's stop flag lags T by its wakeup.
    if (ElapsedNs(st.t0) >= st.duration_ns) break;
    if (pos == order.size()) {
      if (seq) ++pass;
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      mt19937_64 rng(seed + pass);
      shuffle(order.begin(), order.end(), rng);
      pos = 0;
    }
    size_t qid = order[pos++];
    auto fr = MakeQuery(queries[qid], cols);
    uint64_t applied_before = 0;
    bit_lsm::BitLSMQuery verify_query;  // Scan may reorder or move `fr.query`
    if (st.verify) {
      verify_query = fr.query;
      lock_guard<mutex> g(st.verify_mu);
      applied_before = st.applied;
    }
    int64_t start = ElapsedNs(st.t0);
    auto r = b->Scan(fr.query);
    int64_t end = ElapsedNs(st.t0);
    ++tot.started;
    if (!r.ok) {
      cerr << "ERROR: scan stopped on an error at seq " << seq << "; aborting\n";
      st.failed.store(true);
      break;
    }
    bool in_window = end <= st.duration_ns;
    if (in_window) {
      ++tot.completed_in_window;
      st.completed.fetch_add(1, memory_order_relaxed);
    } else {
      ++tot.in_flight_at_end;
    }
    csv << seq << "," << pass << "," << qid << "," << fr.k << ",\""
        << fr.attr_names << "\"," << start / 1'000'000 << "," << end / 1'000'000
        << "," << (end - start) / 1000 << "," << r.matched << "," << in_window
        << "\n";
    if (st.verify && !VerifyOne(verify_query, applied_before, r.matched, seq, st))
      ++tot.verify_failures;
  }
  csv.flush();
  return tot;
}

static void WriteLsmRow(ofstream& csv, int64_t t_ms, const experiment::LsmStats& s) {
  csv << t_ms << "," << s.running_flushes << "," << s.running_compactions << ","
      << s.flush_pending << "," << s.compaction_pending << ","
      << s.pending_compaction_bytes << "," << s.l0_files << ","
      << s.memtable_bytes << "," << s.immutable_memtables << ","
      << s.write_stopped << "," << s.delayed_write_rate << "," << s.stall_stops
      << "," << s.stall_delays << "," << s.flush_bytes << ","
      << s.compact_read_bytes << "," << s.compact_write_bytes << ","
      << s.flush_count << "," << s.compaction_count << ","
      << ReadStatusKb("VmRSS") << "\n";
}

// Fixed-interval samples from t0 until both worker threads have finished
// (so the tail past T is visible), then one final row.
static void SamplerLoop(experiment::Binding* b, double interval_s, RunState& st,
                        ofstream& csv) {
  pthread_setname_np(pthread_self(), "sampler");
  csv << "t_ms,running_flushes,running_compactions,flush_pending,"
         "compaction_pending,pending_compaction_bytes,l0_files,memtable_bytes,"
         "immutable_memtables,write_stopped,delayed_write_rate,stall_stops,"
         "stall_delays,flush_bytes,compact_read_bytes,compact_write_bytes,"
         "flush_count,compaction_count,rss_kb\n";
  auto interval = chrono::nanoseconds(static_cast<int64_t>(interval_s * 1e9));
  auto next = st.t0;
  while (true) {
    WriteLsmRow(csv, ElapsedNs(st.t0) / 1'000'000, b->SampleLsmStats());
    csv.flush();
    if (st.threads_done.load()) break;
    next += interval;
    this_thread::sleep_until(next);
  }
}

// The measured window. Returns the totals block for meta.
static json RunMixed(experiment::Binding* b, const Config& cfg,
                     const vector<string>& queries,
                     const vector<WriteItem>& updates, const Columns& cols,
                     const string& prefix, RunState& st) {
  st.duration_ns = static_cast<int64_t>(cfg.duration_s * 1e9);
  ofstream qcsv(prefix + "_query_log.csv");
  ofstream lcsv(prefix + "_lsm_log.csv");
  ofstream wcsv;
  if (cfg.update_rate > 0) wcsv.open(prefix + "_write_log.csv");

  json totals;
  totals["lsm_at_t0"] = LsmToJson(b->SampleLsmStats());
  st.t0 = Clock::now();
  thread sampler(SamplerLoop, b, cfg.sample_interval_s, ref(st), ref(lcsv));
  QueryTotals qt;
  WriterTotals wt;
  thread query([&] { qt = QueryLoop(b, queries, cols, cfg.seed, st, qcsv); });
  thread writer;
  if (cfg.update_rate > 0) {
    qui::Pacer pacer{cfg.update_rate};
    writer = thread([&, pacer] { wt = WriterLoop(b, updates, pacer, st, wcsv); });
  }

  // Progress until T, then stop. Workers finish their in-flight operation.
  auto window = chrono::nanoseconds(static_cast<int64_t>(cfg.window_s * 1e9));
  auto next = st.t0 + window;
  auto end = st.t0 + chrono::nanoseconds(st.duration_ns);
  while (Clock::now() < end && !st.failed.load()) {
    this_thread::sleep_until(min(next, end));
    if (Clock::now() >= next) {
      cout << "[qui] t=" << ElapsedNs(st.t0) / 1'000'000'000 << "s queries="
           << st.completed.load() << " updates=" << st.issued.load() << endl;
      next += window;
    }
  }
  st.stop.store(true);
  query.join();
  if (writer.joinable()) writer.join();
  int64_t threads_done_ns = ElapsedNs(st.t0);
  totals["lsm_at_end"] = LsmToJson(b->SampleLsmStats());

  auto t_drain = Clock::now();
  b->WaitForQuiescence();
  int64_t drain_ms = ElapsedNs(t_drain) / 1'000'000;
  st.threads_done.store(true);
  sampler.join();
  totals["lsm_after_drain"] = LsmToJson(b->SampleLsmStats());

  double T = cfg.duration_s;
  totals["queries_started"] = qt.started;
  totals["queries_completed_in_window"] = qt.completed_in_window;
  totals["queries_in_flight_at_end"] = qt.in_flight_at_end;
  totals["qps"] = qt.completed_in_window / T;
  totals["verify_failures"] = qt.verify_failures;
  totals["updates_scheduled"] = wt.scheduled;
  totals["updates_issued"] = wt.issued;
  totals["updates_unissued"] = wt.unissued;
  totals["updates_done_in_window"] = wt.done_in_window;
  totals["put_completed_after_end"] = wt.put_completed_after_end;
  totals["actual_update_rate"] = wt.done_in_window / T;
  totals["wrapped"] = wt.wrapped;
  totals["threads_done_ms"] = threads_done_ns / 1'000'000;
  totals["drain_ms"] = drain_ms;
  totals["failed"] = st.failed.load();
  cout << "[qui] done: qps=" << totals["qps"] << " actual_update_rate="
       << totals["actual_update_rate"] << " unissued=" << wt.unissued
       << " drain_ms=" << drain_ms << "\n";
  return totals;
}

static Config ParseArgs(int argc, char* argv[]) {
  cxxopts::Options opts("qui_player", "Query-under-ingestion driver");
  opts.allow_unrecognised_options();
  // clang-format off
  opts.add_options()
    ("binding", "bitlsm|embedded-postings|embedded", cxxopts::value<string>())
    ("db_path", "DB path (a per-run copy; the run overwrites records)", cxxopts::value<string>())
    ("query_workload", "TSV of r lines, cycled closed-loop", cxxopts::value<string>())
    ("update_workload", "TSV of u lines (honk_run.py overwrite)", cxxopts::value<string>()->default_value(""))
    ("update_rate", "Updates per second (0 = queries only)", cxxopts::value<double>()->default_value("0"))
    ("duration_s", "Measured window length", cxxopts::value<double>()->default_value("1800"))
    ("window_s", "Progress print interval", cxxopts::value<double>()->default_value("60"))
    ("sample_interval_s", "LSM sampler interval", cxxopts::value<double>()->default_value("1"))
    ("seed", "Query-order shuffle seed", cxxopts::value<uint64_t>()->default_value("42"))
    ("output_dir", "Result directory", cxxopts::value<string>()->default_value("./result"))
    ("indexed_attrs", "Comma-separated indexed attribute names", cxxopts::value<string>()->default_value(""))
    ("settle_only", "Open, settle (one query + drain), close, exit", cxxopts::value<bool>()->default_value("false"))
    ("verify", "Check every query against an in-memory truth table", cxxopts::value<bool>()->default_value("false"))
    ("base_workload", "--verify: the w trace that built --db_path", cxxopts::value<string>()->default_value(""));
  // clang-format on
  auto r = opts.parse(argc, argv);
  Config c;
  if (!r.count("binding") || !r.count("db_path") || !r.count("query_workload")) {
    cerr << "Required: --binding, --db_path, --query_workload\n";
    exit(1);
  }
  c.binding = r["binding"].as<string>();
  c.db_path = r["db_path"].as<string>();
  c.query_workload = r["query_workload"].as<string>();
  c.update_workload = r["update_workload"].as<string>();
  c.update_rate = r["update_rate"].as<double>();
  c.duration_s = r["duration_s"].as<double>();
  c.window_s = r["window_s"].as<double>();
  c.sample_interval_s = r["sample_interval_s"].as<double>();
  c.seed = r["seed"].as<uint64_t>();
  c.output_dir = r["output_dir"].as<string>();
  c.indexed_attrs = r["indexed_attrs"].as<string>();
  c.settle_only = r["settle_only"].as<bool>();
  c.verify = r["verify"].as<bool>();
  c.base_workload = r["base_workload"].as<string>();
  if (c.update_rate > 0 && c.update_workload.empty()) {
    cerr << "--update_rate > 0 needs --update_workload\n";
    exit(1);
  }
  if (c.verify && c.base_workload.empty()) {
    cerr << "--verify needs --base_workload\n";
    exit(1);
  }
  return c;
}

// --verify: newest-wins table of indexed attrs from the w trace that built
// the DB. Later updates are applied by the writer under verify_mu.
static void LoadTruth(const string& base_workload, const Columns& cols,
                      RunState& st) {
  honk::RecordParser parser(cols.indexed_indices);
  honk::TSVReader reader(base_workload);
  honk::Operation op;
  vector<Attr> attrs;
  string payload;
  while (reader.Next(op)) {
    if (op.type != honk::OpType::WRITE && op.type != honk::OpType::UPDATE)
      continue;
    auto& w = get<honk::WriteOp>(op.data);
    parser.ParseRecord(w.json, attrs, payload);
    st.truth[w.pk] = attrs;
  }
  cout << "[verify] truth table: " << st.truth.size() << " keys\n";
}

int main(int argc, char* argv[]) {
  Config cfg = ParseArgs(argc, argv);
  Columns cols = SetupColumns(cfg.indexed_attrs);

  auto binding = experiment::CreateBinding(cfg.binding);
  if (!binding) {
    cerr << "Unknown binding: " << cfg.binding << "\n";
    return 1;
  }
  auto taxi_opts = honk::BuildTaxiBitLSMOptions(cols.indexed_indices);
  binding->Open(argc, argv, cfg.db_path, taxi_opts);

  json meta;
  meta["binding"] = binding->Name();
  meta["param_suffix"] = binding->ParamSuffix();
  meta["db_path"] = cfg.db_path;
  meta["query_workload"] = cfg.query_workload;
  meta["update_workload"] = cfg.update_workload;
  meta["update_rate"] = cfg.update_rate;
  meta["duration_s"] = cfg.duration_s;
  meta["window_s"] = cfg.window_s;
  meta["sample_interval_s"] = cfg.sample_interval_s;
  meta["seed"] = cfg.seed;
  meta["indexed_attrs"] = cfg.indexed_attrs;
  meta["env"] = EnvJson();
  {
    vector<string> args(argv, argv + argc);
    meta["argv"] = args;
  }

  vector<string> queries = LoadQueries(cfg.query_workload);
  meta["query_count"] = queries.size();

  if (cfg.settle_only) {
    meta["settle"] = Settle(binding.get(), queries, cols);
    binding->Close();
    cout << meta.dump(2) << "\n";
    return 0;
  }

  // Preload: only what this rate can issue in the window (+5 %), so memory
  // scales with the rate. Wrap-around is reported, never silent.
  uint64_t want = cfg.update_rate > 0
                      ? static_cast<uint64_t>(ceil(cfg.update_rate * cfg.duration_s * 1.05))
                      : 0;
  auto t_pre = Clock::now();
  vector<WriteItem> updates = LoadUpdates(cfg.update_workload, want, cols);
  meta["preload"] = json{{"wanted", want},
                         {"loaded", updates.size()},
                         {"ms", ElapsedNs(t_pre) / 1'000'000},
                         {"rss_kb", ReadStatusKb("VmRSS")}};
  cout << "[preload] " << updates.size() << " updates in "
       << ElapsedNs(t_pre) / 1'000'000 << " ms\n";

  meta["settle"] = Settle(binding.get(), queries, cols);

  filesystem::create_directories(cfg.output_dir);
  string prefix = cfg.output_dir + "/" +
                  filesystem::path(cfg.query_workload).stem().string() + "_" +
                  binding->Name() + binding->ParamSuffix() + "_" +
                  RatePrefix(cfg.update_rate);

  RunState st;
  st.verify = cfg.verify;
  if (cfg.verify) LoadTruth(cfg.base_workload, cols, st);
  meta["totals"] = RunMixed(binding.get(), cfg, queries, updates, cols, prefix, st);

  binding->Close();
  ofstream(prefix + "_meta.json") << meta.dump(2) << "\n";
  cout << "[done] " << prefix << "_meta.json\n";
  if (meta["totals"]["failed"].get<bool>()) return 1;
  if (cfg.verify && meta["totals"]["verify_failures"].get<uint64_t>() > 0) return 1;
  return 0;
}
