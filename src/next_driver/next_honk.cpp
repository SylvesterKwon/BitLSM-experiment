#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/slice.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/perf_level.h"
#include "util/rtree.h"
#include "next_schema.h"
#include "next_dict.h"
#include "next_delta.h"
#include "next_record.h"
#include "next_query.h"
#include "tsv_parser.h"

namespace fs = std::filesystem;

// Copied verbatim from
// third_party/NEXT/rocksdb-7.7.3/examples/secondary_index_read_num.cc:57-115
// (the 1D secondary index relies on these; NOT reimplemented/simplified).
class NoiseComparator : public rocksdb::Comparator {
 public:
  const char* Name() const {
    return "rocksdb.NoiseComparator";
  }

  int Compare(const rocksdb::Slice& const_a, const rocksdb::Slice& const_b) const {
    rocksdb::Slice slice_a = rocksdb::Slice(const_a);
    rocksdb::Slice slice_b = rocksdb::Slice(const_b);

    // keypaths are the same, compare the value. The previous
    // `GetLengthPrefixedSlice()` did advance the Slice already, hence a call
    // to `.data()` can directly be used.
    const int* value_a = reinterpret_cast<const int*>(slice_a.data());
    const int* value_b = reinterpret_cast<const int*>(slice_b.data());


    return slice_a.compare(slice_b);
  }

  void FindShortestSeparator(std::string* start,
                             const rocksdb::Slice& limit) const {
    return;
  }

  void FindShortSuccessor(std::string* key) const  {
    return;
  }
};

class NoiseComparator1 : public rocksdb::Comparator {
 public:
  const char* Name() const {
    return "rocksdb.NoiseComparator";
  }

  int Compare(const rocksdb::Slice& const_a, const rocksdb::Slice& const_b) const {
    rocksdb::Slice slice_a = rocksdb::Slice(const_a);
    rocksdb::Slice slice_b = rocksdb::Slice(const_b);

    // keypaths are the same, compare the value. The previous
    // `GetLengthPrefixedSlice()` did advance the Slice already, hence a call
    // to `.data()` can directly be used.
    const int* value_a = reinterpret_cast<const int*>(slice_a.data());
    const int* value_b = reinterpret_cast<const int*>(slice_b.data());

    // specific comparator to allow random output order
    return 1;
  }

  void FindShortestSeparator(std::string* start,
                             const rocksdb::Slice& limit) const {
    return;
  }

  void FindShortSuccessor(std::string* key) const  {
    return;
  }
};

// Copied verbatim from
// third_party/NEXT/rocksdb-7.7.3/examples/secondary_index_read_num.cc:40-45.
std::string serialize_query(double x_value_min, double x_value_max) {
  std::string query;
  query.append(reinterpret_cast<const char*>(&x_value_min), sizeof(double));
  query.append(reinterpret_cast<const char*>(&x_value_max), sizeof(double));
  return query;
}

int main(int argc, char* argv[]) {
  if (argc > 1 && std::string(argv[1]) == "--selfcheck-record") {
    // Round-trip check for ParseRecord / IndexedValue / DecodePayloadAttr,
    // exercising N=2 co-resident indexed attrs (trip_distance @ position 0,
    // fare_amount @ position 1) to prove the multi-attr codec's offsets.
    std::string json =
        R"({"trip_distance":3.5,"payment_type":"2","fare_amount":12.5})";
    std::vector<int> indexed_cols = {nextd::ColIndex("trip_distance"),
                                      nextd::ColIndex("fare_amount")};

    std::vector<double> out_indexed;
    std::string out_payload;
    nextd::ParseRecord(json, indexed_cols, out_indexed, out_payload);

    std::string value;
    for (double d : out_indexed)
      value.append(reinterpret_cast<const char*>(&d), sizeof(double));
    value += out_payload;

    bool pass = true;

    double iv0 = nextd::IndexedValue(value, 0);
    if (iv0 != 3.5) {
      printf("FAIL: IndexedValue(pos=0)=%f, expected 3.5\n", iv0);
      pass = false;
    }
    double iv1 = nextd::IndexedValue(value, 1);
    if (iv1 != 12.5) {
      printf("FAIL: IndexedValue(pos=1)=%f, expected 12.5\n", iv1);
      pass = false;
    }

    nextd::Attr payment_type_attr;
    int pt_idx = nextd::ColIndex("payment_type");
    if (!nextd::DecodePayloadAttr(value, pt_idx, indexed_cols.size(),
                                  payment_type_attr) ||
        !std::holds_alternative<std::string>(payment_type_attr) ||
        std::get<std::string>(payment_type_attr) != "2") {
      printf("FAIL: DecodePayloadAttr(payment_type) mismatch\n");
      pass = false;
    }

    // fare_amount is now indexed (position 1), so it must be ABSENT from
    // the payload -- DecodePayloadAttr should return false, not stale data.
    nextd::Attr fare_amount_attr;
    int fa_idx = nextd::ColIndex("fare_amount");
    if (nextd::DecodePayloadAttr(value, fa_idx, indexed_cols.size(),
                                 fare_amount_attr)) {
      printf("FAIL: DecodePayloadAttr(fare_amount) unexpectedly present "
             "(should be routed to the indexed area, not payload)\n");
      pass = false;
    }

    printf(pass ? "PASS\n" : "FAIL\n");
    return pass ? 0 : 1;
  }
  if (argc > 1 && std::string(argv[1]) == "--selfcheck-schema") {
    const auto& cols = nextd::Columns();
    printf("Columns().size() = %zu\n", cols.size());
    int td = nextd::ColIndex("trip_distance");
    int pt = nextd::ColIndex("payment_type");
    printf("trip_distance: index=%d type=%s\n", td,
           td >= 0 && cols[td].type == nextd::AttrType::CONTINUOUS
               ? "CONTINUOUS"
               : "CATEGORICAL");
    printf("payment_type: index=%d type=%s\n", pt,
           pt >= 0 && cols[pt].type == nextd::AttrType::CATEGORICAL
               ? "CATEGORICAL"
               : "CONTINUOUS");
    return 0;
  }
  // --- Write mode: manual argv loop (no cxxopts; keeps the isolated build
  // free of the experiment's dependency on cxxopts). ---
  std::string workload_path, db_path, indexed_attr_name, indexed_attrs_arg;
  std::string output_dir = "./result";
  int max_background_jobs = 6;
  double drain_seconds = 0.0;
  // NEXT (Phase 4, Task 4.2): opt-in SST data-block size override, default
  // unchanged (RocksDB's own 4KB default -- every existing caller gets
  // byte-identical behavior). Exists ONLY so the I/O-advantage test
  // (scripts/test/next_im_xcheck.sh) can force many small data blocks: with
  // the default 4KB block and ~100 small taxi records, ALL ~9 data blocks
  // end up covering nearly the FULL value range of every indexed column
  // (records are inserted in random uuid-key order, unrelated to attribute
  // value, so a ~11-record block's per-attribute MBR is rarely narrower
  // than the whole domain -- worst case for a 2-valued categorical column
  // like VendorID) -- neither PF nor IM's candidate-block sets can be
  // selective on such a coarse layout, so it cannot demonstrate IM's
  // intersection advantage regardless of correctness. A small --block_size
  // (e.g. one taxi record per block) makes each data block's per-attribute
  // MBR essentially exact, letting every indexed predicate's R-tree search
  // exclude the blocks that provably cannot match.
  uint64_t block_size = 4 * 1024;
  // NEXT (Task 3.1): --mode splits the driver into a write-only pass and a
  // read-only pass so a cross-process reopen can be exercised: "write"
  // processes WRITE/UPDATE ops only (no reads) then closes the DB; "read"
  // opens an EXISTING DB (does not create one) and processes READ ops only,
  // still counting (but not re-executing) WRITE/UPDATE ops so read CSV
  // denominators (records_total) match the write pass. Default "both" is
  // the original interleaved behavior, unchanged for existing callers
  // (next_xcheck.sh, next_smoke_test.sh).
  std::string mode = "both";
  // NEXT (Phase 4, Task 4.1): "pf" (default, Phase 3, unchanged) drives ONE
  // indexed attribute via NEXT and post-filters every other predicate. "im"
  // (index merge) enumerates EVERY indexed attribute that has a predicate in
  // the query, intersects their candidate SST data-block sets by
  // (filenum, offset) engine-side (db/version_set.cc), and post-filters
  // ALL predicates (same Matches() call as pf) on whatever the engine
  // returns -- correctness never depends on the engine, only on this
  // post-filter.
  std::string read_strategy = "pf";
  // NEXT (Phase 4, Task 4.2): opt-in per-query "blocks touched" counter, via
  // RocksDB's existing PerfContext (no engine change -- pure driver-side use
  // of a public API already linked into librocksdb.a). Proves IM's I/O
  // advantage: with the counter enabled, block_cache_hit_count/
  // block_read_count (minus their index/filter/compression-dict-specific
  // sub-counts -- see the DATA-block-only computation below, mirroring
  // table/block_based/block_based_table_reader_test.cc:272-274) are reset
  // before each query and read back after, giving the exact number of
  // DATA blocks this query's iterator visited (cache hit + miss combined,
  // so it is correct regardless of what an earlier query already warmed in
  // the block cache).
  bool report_blocks = false;
  // NEXT (Phase 5, Task 5.2): single user knob controlling per-attribute
  // global-index granularity. Each indexed attribute's gap threshold delta_i
  // (BlockBasedTableOptions::sec_index_deltas, consumed by the Task 5.1
  // engine's strict-`>` gap split) is DERIVED from the data distribution to
  // target E_i = clamp(lround(N/(m*B)), 1, C_i) global-index entries; bigger m
  // => fewer target entries => coarser (smaller) index. Used only in WRITE
  // mode (the index is built once, at write time); READ mode reopens the built
  // DB and never recomputes delta, so --m is accepted but inert there.
  double m_knob = 4.0;

  auto need_val = [&](int& i) -> std::string {
    if (i + 1 >= argc) {
      fprintf(stderr, "missing value for %s\n", argv[i]);
      exit(1);
    }
    return argv[++i];
  };

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--workload") workload_path = need_val(i);
    else if (arg == "--db_path") db_path = need_val(i);
    else if (arg == "--indexed_attr") indexed_attr_name = need_val(i);
    else if (arg == "--indexed_attrs") indexed_attrs_arg = need_val(i);
    else if (arg == "--output_dir") output_dir = need_val(i);
    else if (arg == "--max_background_jobs") max_background_jobs = std::stoi(need_val(i));
    else if (arg == "--drain_seconds") drain_seconds = std::stod(need_val(i));
    else if (arg == "--block_size") block_size = std::stoull(need_val(i));
    else if (arg == "--mode") mode = need_val(i);
    else if (arg == "--read_strategy") read_strategy = need_val(i);
    else if (arg == "--m") m_knob = std::stod(need_val(i));
    else if (arg == "--report_blocks") report_blocks = true;
    else {
      fprintf(stderr, "unknown arg: %s\n", arg.c_str());
      return 1;
    }
  }

  if (mode != "both" && mode != "write" && mode != "read") {
    fprintf(stderr, "invalid --mode '%s' (expected write|read|both)\n",
            mode.c_str());
    return 1;
  }
  if (read_strategy != "pf" && read_strategy != "im") {
    fprintf(stderr, "invalid --read_strategy '%s' (expected pf|im)\n",
            read_strategy.c_str());
    return 1;
  }
  const bool do_writes = (mode != "read");
  const bool do_reads = (mode != "write");

  // `--indexed_attrs a,b,c` (N co-resident 1D sec-indexes, list order ->
  // value offset 8*position) takes precedence if given; otherwise fall back
  // to the original single-attr `--indexed_attr <name>` (N=1). Keeping both
  // flags means the existing next_xcheck.sh (which uses --indexed_attr)
  // continues to work unchanged.
  std::vector<std::string> indexed_attr_names;
  if (!indexed_attrs_arg.empty()) {
    size_t start = 0;
    while (start <= indexed_attrs_arg.size()) {
      size_t comma = indexed_attrs_arg.find(',', start);
      std::string tok = indexed_attrs_arg.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!tok.empty()) indexed_attr_names.push_back(tok);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  } else if (!indexed_attr_name.empty()) {
    indexed_attr_names.push_back(indexed_attr_name);
  }

  if (workload_path.empty() || db_path.empty() || indexed_attr_names.empty()) {
    fprintf(stderr,
            "usage: next_honk --workload <tsv> --db_path <dir> "
            "(--indexed_attr <name> | --indexed_attrs <a,b,c>) "
            "[--output_dir <dir>] "
            "[--max_background_jobs N] [--drain_seconds S] "
            "[--mode write|read] [--read_strategy pf|im] "
            "[--m <double: E_i=N/(m*B) global-entry coarsening; default 4>] "
            "[--report_blocks] [--block_size N]\n");
    return 1;
  }

  // indexed_cols[p] is the column routed to value offset 8*p (matching the
  // engine's index p reading its double at that offset -- Task 2.2-2.4).
  // Both CONTINUOUS and CATEGORICAL columns may be indexed (Task 3.3):
  // CATEGORICAL columns are dictionary-encoded to a double id (cat_dicts,
  // below) so they share the same 8-byte slot.
  std::vector<int> indexed_cols;
  indexed_cols.reserve(indexed_attr_names.size());
  for (const auto& name : indexed_attr_names) {
    int col = nextd::ColIndex(name);
    if (col < 0) {
      fprintf(stderr, "unknown indexed attr: %s\n", name.c_str());
      return 1;
    }
    indexed_cols.push_back(col);
  }

  // Task 3.3: one CategoricalDict per CATEGORICAL indexed column, keyed by
  // column index. Persisted next to the DB as dict_<attr_name>.txt so a
  // separate --mode read process decodes/encodes with the IDENTICAL
  // mapping the write process assigned (see next_dict.h).
  std::map<int, nextd::CategoricalDict> cat_dicts;
  for (int col : indexed_cols) {
    if (nextd::Columns()[col].type == nextd::AttrType::CATEGORICAL)
      cat_dicts.emplace(col, nextd::CategoricalDict());
  }
  auto dict_path_for = [](const fs::path& db_dir, int col) {
    return (db_dir / ("dict_" + nextd::Columns()[col].name + ".txt")).string();
  };

  // Resolve ALL path args to absolute BEFORE the chdir() below -- otherwise
  // relative --workload/--output_dir paths would (mis)resolve against
  // db_path instead of the invocation directory.
  fs::path abs_workload_path = fs::absolute(workload_path);
  fs::path abs_output_dir = fs::absolute(output_dir);
  fs::path abs_db_path = fs::absolute(db_path);
  fs::path original_cwd = fs::current_path();

  // CONTAIN unmodified NEXT's stray "<<Global Index Component Directory>>"
  // file: it is hardcoded (db/version_set.h:1511 global_rtree_loc_) to a
  // bare relative filename written into the CWD on DB::Open. mkdir -p the
  // (absolute) db_path and chdir into it BEFORE DB::Open so that stray file
  // -- and all RocksDB files -- land under db_path, not the repo root.
  std::error_code ec;
  fs::create_directories(abs_db_path, ec);
  fs::current_path(abs_db_path);

  // NEXT (Phase 5, Task 5.2): stats PRE-PASS (write mode only). Scan the
  // workload ONCE up front to derive each indexed attribute's gap threshold
  // delta_i from the DATA distribution (never the query workload) before the
  // engine builds any SST. Collects, per attribute, the set of DISTINCT
  // indexed values plus N (total WRITE/UPDATE ops, matching the read CSV's
  // records_total denominator) and the average serialized value size (to
  // estimate B = values per data block).
  //
  // Categoricals are dict-encoded through a THROWAWAY dict local to this pass
  // (stats_dicts), NOT the write pass's cat_dicts: a CategoricalDict assigns
  // dense ids 0,1,2,... in first-seen file order, so this pass and the main
  // write pass -- which traverse the SAME file in the SAME order -- assign
  // byte-identical ids. Deriving delta_i on stats_dicts's ids therefore
  // calibrates it to the exact id space the engine will store, while leaving
  // cat_dicts untouched so the main loop populates it incrementally EXACTLY as
  // before (this matters for an interleaved read-under-ingestion workload,
  // where a read must see only the dict entries from writes that preceded it).
  //
  // Distinct values are held in a std::set<double> per attribute so peak
  // memory is O(C_i * a), not O(N * a) -- ComputeSecIndexDeltas distinct-ifies
  // its input regardless, so this is equivalent to (and lighter than) storing
  // all N values as the brief's signature literally allows.
  std::vector<double> sec_index_deltas;       // -> bbto.sec_index_deltas
  std::vector<uint64_t> delta_cardinalities;  // C_i (for logging only)
  std::vector<uint64_t> delta_targets;        // E_i (for logging only)
  double est_B = 1.0;                          // values per data block estimate
  uint64_t stats_N = 0;                        // total indexed rows seen
  if (do_writes) {
    const size_t a = indexed_cols.size();
    std::map<int, nextd::CategoricalDict> stats_dicts;
    for (int col : indexed_cols) {
      if (nextd::Columns()[col].type == nextd::AttrType::CATEGORICAL)
        stats_dicts.emplace(col, nextd::CategoricalDict());
    }
    std::vector<std::set<double>> distinct_vals(a);
    uint64_t payload_bytes_total = 0;
    honk::TSVReader stats_reader(abs_workload_path.string());
    honk::Operation stats_op;
    std::vector<double> stats_indexed;
    std::string stats_payload;
    while (stats_reader.Next(stats_op)) {
      if (stats_op.type != honk::OpType::WRITE &&
          stats_op.type != honk::OpType::UPDATE)
        continue;
      auto& w = std::get<honk::WriteOp>(stats_op.data);
      nextd::ParseRecord(w.json, indexed_cols, stats_indexed, stats_payload,
                         [&](int col, const std::string& val) -> double {
                           return stats_dicts.at(col).EncodeOrInsert(val);
                         });
      for (size_t p = 0; p < a; p++) distinct_vals[p].insert(stats_indexed[p]);
      payload_bytes_total += stats_payload.size();
      stats_N++;
    }

    // B = block_size / avg_serialized_value_bytes; value = [8*a doubles] +
    // payload. Guard a zero-write workload (est_B stays 1.0).
    if (stats_N > 0) {
      double avg_payload =
          static_cast<double>(payload_bytes_total) / static_cast<double>(stats_N);
      double avg_serialized = static_cast<double>(8 * a) + avg_payload;
      if (avg_serialized > 0.0)
        est_B = static_cast<double>(block_size) / avg_serialized;
    }

    std::vector<std::vector<double>> per_attr_values(a);
    for (size_t p = 0; p < a; p++)
      per_attr_values[p].assign(distinct_vals[p].begin(), distinct_vals[p].end());

    sec_index_deltas =
        nextd::ComputeSecIndexDeltas(per_attr_values, stats_N, m_knob, est_B,
                                     &delta_cardinalities, &delta_targets);
  }

  rocksdb::Options options;
  // NEXT (Task 3.1): --mode read must reopen an EXISTING DB, never silently
  // create an empty one -- that would defeat the cross-process reopen gate
  // (it would "pass" against an empty, freshly-created DB instead of
  // exercising MANIFEST replay against the writer's data).
  options.create_if_missing = (mode != "read");

  static NoiseComparator cmp;
  static NoiseComparator1 sec_cmp;
  options.comparator = &cmp;
  options.sec_comparator = &sec_cmp;

  options.max_background_jobs = max_background_jobs;
  options.max_write_buffer_number = 5;
  options.write_buffer_size = 64 * 1024 * 1024;
  // NEXT (Phase 5, Task 5.3): FAIR-CONFIG ALIGNMENT. Every store-config knob
  // that shapes the write/read comparison is pinned to the baselines' EFFECTIVE
  // value so the benchmark isolates the INDEX DESIGN, not RocksDB tuning. The
  // baselines (honk_player bindings, RocksDB 10.10.0) set max_write_buffer_number
  // and block_size explicitly and leave write_buffer_size / block_cache /
  // compression at the 10.10.0 defaults; their effective values (verified from
  // that source tree + the linked libsnappy) are:
  //   write_buffer_size       = 64 MB   (options.h default)      -> pinned above
  //   max_write_buffer_number = 5                                -> pinned above
  //   block_size              = 4 KB                             -> pinned (bbto)
  //   block_cache             = 32 MB                            -> pinned (bbto)
  //   compression             = Snappy  (libsnappy linked)       -> pinned here
  //   max_background_jobs     = 6       (CLI default)            -> --max_background_jobs
  //   level_compaction_dynamic_level_bytes = true (all baselines) -> pinned here
  //   bytes_per_sync          = 1 MB    (all baselines)          -> pinned here
  // These are pinned EXPLICITLY (not left to defaults) because NEXT's engine is
  // RocksDB 7.7.3, whose OWN defaults differ (block cache default is 8 MB, not
  // 32 MB; level_compaction_dynamic_level_bytes defaults false vs the baselines'
  // true, which would otherwise change NEXT's LSM level-target cascade -- level
  // count, read/write amplification -- exactly what this benchmark measures);
  // relying on defaults would silently give NEXT a 4x smaller read cache and a
  // different level layout. Snappy is 7.7.3's default too, but is pinned for the
  // same robustness.
  options.compression = rocksdb::kSnappyCompression;
  options.level_compaction_dynamic_level_bytes = true;
  options.bytes_per_sync = 1048576;
  options.allow_concurrent_memtable_write = false;
  options.create_global_sec_index = true;
  options.global_sec_index_is_spatial = false;
  options.memtable_factory.reset(new rocksdb::SkipListSecFactory);

  rocksdb::BlockBasedTableOptions bbto;
  bbto.create_secondary_index = true;
  bbto.create_sec_index_reader = true;
  bbto.sec_index_type = rocksdb::BlockBasedTableOptions::kOneDRtreeSec;
  // 32 MB matches the baselines' EFFECTIVE block cache: RocksDB 10.10.0's
  // default (when a binding leaves block_cache unset, as all baselines do) is a
  // 32 MB HyperClockCache. We pin 32 MB here because 7.7.3's OWN default is only
  // 8 MB -- leaving it unset would hand NEXT a 4x smaller read cache. LRU vs HCC
  // is a cache-implementation difference (7.7.3 predates AutoHCC); the CAPACITY,
  // which drives hit rate and thus the read comparison, is identical. (Task 5.3)
  bbto.block_cache = rocksdb::NewLRUCache(32 * 1024 * 1024);
  bbto.block_size = block_size;
  // N co-resident 1D sec-indexes: index j reads the double at value offset
  // 8*j (engine side, Task 2.2-2.4). Defaults to 1 for the single-attr path.
  bbto.num_sec_indexes = static_cast<uint32_t>(indexed_cols.size());
  // NEXT (Phase 5, Task 5.2): feed the data-derived per-attribute gap
  // thresholds to the Task 5.1 engine (element i = delta for the index over
  // attribute i in --indexed_attrs order). Only meaningful in write mode (the
  // index is built once); left EMPTY in read mode, where the engine's 0.005
  // fallback is irrelevant because OpenForReadOnly never rebuilds an index.
  bbto.sec_index_deltas = sec_index_deltas;
  options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbto));

  rocksdb::DB* db = nullptr;
  rocksdb::Status s;
  if (mode == "read") {
    // Task 4.2 finding: a plain rocksdb::DB::Open() reopen (used
    // unconditionally here before this fix) always rolls a FRESH manifest
    // generation via DBImpl::Recover -> LogAndApplyForRecovery (see
    // db/db_impl/db_impl_open.cc), even for a read-only workload. That
    // fresh manifest is written by core (unpatched) VersionSet code, which
    // does not round-trip this fork's custom per-file secondary-index
    // byte-range fields on FileMetaData -- so a SUBSEQUENT reopen against
    // that rolled manifest silently gets an EMPTY global R-tree (every
    // query, PF and IM alike, matches 0 records, no error). This was
    // invisible to every prior gate (next_reopen_xcheck.sh,
    // next_multiattr_xcheck.sh) because each does exactly ONE --mode read
    // reopen; it surfaces the moment a SECOND --mode read process reopens
    // the same --db_path (exactly what running --read_strategy pf and im
    // back to back against one on-disk DB, as the runner now does, needs).
    // DB::OpenForReadOnly (db/db_impl/db_impl_readonly.cc) shares the same
    // VersionSet::Recover/VersionEditHandler replay that repopulates the
    // global R-tree (Task 3.1/3.2's fix), but never calls
    // LogAndApplyForRecovery, so it never rolls the manifest -- every
    // read-mode reopen keeps replaying the SAME manifest the write process
    // produced, indefinitely. --mode read never writes (do_writes below is
    // false), so read-only semantics lose nothing here.
    s = rocksdb::DB::OpenForReadOnly(options, abs_db_path.string(), &db);
  } else {
    s = rocksdb::DB::Open(options, abs_db_path.string(), &db);
  }
  if (!s.ok()) {
    fprintf(stderr, "DB::Open failed: %s\n", s.ToString().c_str());
    return 2;
  }

  // Task 4.2: kEnableCount is enough for block_read_count/
  // block_cache_hit_count (and their index/filter/compression-dict
  // breakdowns) -- no per-call timing overhead is paid unless this flag is
  // passed.
  if (report_blocks) rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableCount);

  // Task 3.3: --mode read is a SEPARATE process from the one that wrote the
  // DB, so it never sees the write pass's in-memory EncodeOrInsert() calls
  // -- load each categorical indexed column's dict from the file the write
  // process saved (below). --mode write/both build the dict in-memory as
  // they parse records instead (nothing to load yet).
  if (!do_writes) {
    for (auto& [col, dict] : cat_dicts)
      dict.Load(dict_path_for(abs_db_path, col));
  }

  fs::create_directories(abs_output_dir, ec);
  std::string workload_stem = abs_workload_path.stem().string();
  std::string joined_attr_names;
  for (size_t i = 0; i < indexed_attr_names.size(); i++) {
    if (i > 0) joined_attr_names += "_";
    joined_attr_names += indexed_attr_names[i];
  }
  std::string binding_label = "next_1d_" + joined_attr_names;
  // Task 5.3 final-review fix: nyc_taxi_seq_write/read sweep --m (2/4/8, see
  // exp_set/*.json "next" params) against the SAME --output_dir per workload,
  // and "m" is a DB_PARAMS entry in experiments/nyc_taxi_seq_read/run.py
  // precisely because it changes what m_knob derives at write time
  // (per-attribute sec_index_deltas, see the comment above m_knob's
  // declaration) -- i.e. different m = different on-disk index, not just a
  // different scan plan. Without m in the filename here, every m in the
  // sweep computes the identical file_prefix and the last one (m8) silently
  // overwrites the earlier ones' write/read CSVs -- the same clobbering
  // class 9a2f44d fixed for read_strategy (below), just on file_prefix
  // itself since m affects both the write and read CSV, not read alone.
  // Format m_knob the same way BitLSMBinding::ParamSuffix() formats rho
  // (benchmark_experiment.h's format_double(): ostringstream's default
  // 6-sig-fig double formatting, e.g. "4" not "4.000000") so the CSV token
  // matches the db_path token the runner encodes via fmt() (run_common.py),
  // which also strips the decimal for integer-valued JSON m (2/4/8 -> "m2"/
  // "m4"/"m8", not "m2.0"/"m4.0"/"m8.0").
  std::ostringstream m_oss;
  m_oss << m_knob;
  std::string m_token = m_oss.str();
  std::string file_prefix =
      (abs_output_dir /
       (workload_stem + "_" + binding_label + "_m" + m_token))
          .string();
  std::string write_csv_path = file_prefix + "_write_log.csv";

  std::ofstream write_csv;
  auto ensure_write_csv = [&] {
    if (!write_csv.is_open()) {
      write_csv.open(write_csv_path);
      write_csv << "time_elapsed_ms,records_written\n";
    }
  };

  // Task 4.2 review-fix: the runner (nyc_taxi_seq_read/run.py) sweeps
  // --read_strategy im|pf against the SAME --output_dir per workload. Without
  // read_strategy in the filename, both invocations compute the identical
  // read_csv_path and the second run's ensure_read_csv() (a truncating
  // ofstream::open) silently overwrites the first run's results -- mirrors
  // SICKBinding::ParamSuffix() (si_ck_binding.cpp), which appends
  // "_strategy_" + strategy for the same reason (im/pf/lu binding.h methods
  // sharing one binding_label). The write CSV path does not need this: it is
  // only ever written under do_writes (mode write|both), where
  // read_strategy is not swept.
  std::string read_csv_path =
      file_prefix + "_strategy_" + read_strategy + "_read_log.csv";
  std::ofstream read_csv;
  auto ensure_read_csv = [&] {
    if (!read_csv.is_open()) {
      read_csv.open(read_csv_path);
      read_csv << "query_id,query_attr_num,filter_attrs,time_elapsed_ms,"
                  "records_matched,records_total,selectivity_actual\n";
    }
  };

  honk::TSVReader reader(abs_workload_path.string());
  honk::Operation op;
  std::vector<double> indexed_vals;
  std::string payload;
  std::string value;
  uint64_t writes = 0;
  uint64_t reads = 0;
  rocksdb::Status put_status;  // OR-ed across all Put calls (B-I2 carry-over)
  // Task 3.4 (3.3-m3 carry-over): a --mode read process with a missing/empty
  // categorical dict file silently matches 0 records on every query
  // touching that column (Encode() always fails), indistinguishable from a
  // genuinely selective query. Warn once per affected column, the first
  // time a query actually references it (not at Load() time -- an unused
  // indexed column with an empty dict is not itself a problem).
  std::unordered_set<int> warned_empty_dict_for;
  auto wall_start = std::chrono::steady_clock::now();

  while (reader.Next(op)) {
    if (op.type == honk::OpType::WRITE || op.type == honk::OpType::UPDATE) {
      // NEXT (Task 3.1): --mode read must not mutate the reopened DB (that
      // would defeat the point of testing a pure-read reopen), but the
      // record count still advances so read_csv's records_total denominator
      // matches the (already-persisted) write pass on the same workload.
      writes++;
      if (!do_writes) continue;

      auto& w = std::get<honk::WriteOp>(op.data);
      nextd::ParseRecord(w.json, indexed_cols, indexed_vals, payload,
                         [&](int col, const std::string& val) -> double {
                           return cat_dicts.at(col).EncodeOrInsert(val);
                         });

      // value = [8B indexed_0 double][8B indexed_1 double]...[payload]; key
      // = workload pk (string) so that 'u' (UPDATE) ops overwrite the prior
      // record in place.
      value.clear();
      for (double d : indexed_vals)
        value.append(reinterpret_cast<const char*>(&d), sizeof(double));
      value += payload;
      rocksdb::Status put_s = db->Put(rocksdb::WriteOptions(), w.pk, value);
      if (!put_s.ok()) put_status = put_s;

      if (writes % 1'000'000 == 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - wall_start)
                           .count();
        ensure_write_csv();
        write_csv << elapsed << "," << writes << "\n";
        write_csv.flush();
        printf("[write] %llu records, %lldms\n",
               static_cast<unsigned long long>(writes),
               static_cast<long long>(elapsed));
      }
    } else if (op.type == honk::OpType::READ) {
      // NEXT (Task 3.1): --mode write skips reads entirely (no read_csv is
      // produced); the cross-process reopen gate runs the read pass as a
      // separate --mode read invocation instead.
      if (!do_reads) continue;
      auto& r = std::get<honk::ReadOp>(op.data);
      nextd::Query q = nextd::ParseFilters(r.json);

      // Task 3.4 (3.3-m3 carry-over): see warned_empty_dict_for above. Only
      // meaningful in --mode read: --mode write/both start with an empty
      // dict by design (nothing indexed yet) and populate it as they parse,
      // which is not the failure this diagnostic targets.
      if (!do_writes) {
        for (const auto& name : q.attr_names) {
          int idx = nextd::ColIndex(name);
          auto dit = cat_dicts.find(idx);
          if (dit != cat_dicts.end() && dit->second.size() == 0 &&
              warned_empty_dict_for.insert(idx).second) {
            fprintf(stderr,
                    "WARNING: categorical dict for indexed column '%s' is "
                    "empty (missing/unreadable dict file at reopen) -- "
                    "queries filtering on it will silently match 0 records, "
                    "not because the query is selective.\n",
                    name.c_str());
          }
        }
      }

      // Task 3.3 Step 1: dictionary-encode every condition on an INDEXED
      // CATEGORICAL column into the SAME double-id space the write pass
      // used (next_dict.h) -- from here on such a condition is
      // indistinguishable from a CONTINUOUS one to RangeFor/EvalCondition,
      // and get_attr (below) can return the raw indexed double unchanged
      // for both.
      nextd::EncodeIndexedCategoricals(
          q, indexed_cols,
          [&](int col, const std::string& lit, double& out_id) {
            auto dit = cat_dicts.find(col);
            return dit != cat_dicts.end() && dit->second.Encode(lit, out_id);
          });

      // Task 3.3 Step 2 (PF driver): choose ONE indexed attribute to drive
      // via NEXT. Prefer the most_selective_attr hint if it names an
      // indexed column with a usable (single-condition) predicate; else
      // the first indexed column (by --indexed_attrs position) with one;
      // else fall back to index 0 with a full range (query_mbr spans
      // [-DBL_MAX, DBL_MAX]), equivalent to a full scan through the
      // R-tree. Every OTHER predicate -- including ones on the OTHER
      // indexed columns -- is re-checked per-record by the post-filter
      // (nextd::Matches) below, so correctness never depends on this
      // choice, only efficiency.
      int driven_pos = -1;
      double lo = -std::numeric_limits<double>::max();
      double hi = std::numeric_limits<double>::max();
      if (q.hint_attr_idx >= 0) {
        for (size_t p = 0; p < indexed_cols.size(); p++) {
          if (indexed_cols[p] != q.hint_attr_idx) continue;
          double hlo, hhi;
          if (nextd::RangeFor(q, indexed_cols[p], hlo, hhi)) {
            driven_pos = static_cast<int>(p);
            lo = hlo;
            hi = hhi;
          }
          break;
        }
      }
      if (driven_pos < 0) {
        for (size_t p = 0; p < indexed_cols.size(); p++) {
          double plo, phi;
          if (nextd::RangeFor(q, indexed_cols[p], plo, phi)) {
            driven_pos = static_cast<int>(p);
            lo = plo;
            hi = phi;
            break;
          }
        }
      }
      if (driven_pos < 0) driven_pos = 0;  // no indexed attr has a usable
                                            // predicate -> full scan via index 0

      // NEXT 1D read API, verbatim shape from
      // third_party/NEXT/rocksdb-7.7.3/examples/secondary_index_read_num.cc:164-186.
      rocksdb::RtreeIteratorContext ctx;
      ctx.query_mbr = serialize_query(lo, hi);
      // NEXT (Task 3.2): the memtable secondary scan
      // (SkipListSecRep::Iterator) reads sec_attr_index off the
      // IteratorContext (it never sees ReadOptions), while the SST path
      // reads it off ReadOptions -- set both to the same value so a query on
      // records still in the memtable (unflushed) and one on flushed SSTs
      // agree on which of the N co-resident indexes drives the per-tuple
      // filter offset.
      ctx.sec_attr_index = static_cast<uint32_t>(driven_pos);
      rocksdb::ReadOptions read_options;
      read_options.iterator_context = &ctx;
      read_options.is_secondary_index_scan = true;
      read_options.is_secondary_index_spatial = false;
      read_options.sec_attr_index = static_cast<uint32_t>(driven_pos);
      read_options.async_io = true;

      // Task 4.1 (IM): build the predicate set from EVERY indexed attribute
      // that has a usable single-condition predicate in this query (not
      // just the one PF would drive) -- attr_id here is the POSITION in
      // indexed_cols (0..N-1), the same space sec_attr_index/ctx.query_mbr
      // above already use, matching the engine's per-attribute plumbing
      // (ReadOptions::sec_attr_index, util/rtree.h RtreeIteratorContext).
      // If no indexed attr has a usable predicate, im_predicates stays
      // empty and the engine (db/version_set.cc AddIteratorsForLevel) falls
      // back to the PF-shaped full scan set up above -- still correct (a
      // superset), just not via the IM code path.
      if (read_strategy == "im") {
        for (size_t p = 0; p < indexed_cols.size(); p++) {
          double plo, phi;
          if (nextd::RangeFor(q, indexed_cols[p], plo, phi)) {
            ctx.im_predicates.emplace_back(static_cast<uint32_t>(p),
                                           serialize_query(plo, phi));
          }
        }
        ctx.is_index_merge = true;
        read_options.is_index_merge = true;
      }

      // Task 4.2: reset PerfContext right before this query's iterator runs
      // so the counters read back below reflect ONLY this query, not
      // whatever earlier queries in this same process touched.
      if (report_blocks) rocksdb::get_perf_context()->Reset();

      auto read_start = std::chrono::high_resolution_clock::now();
      std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read_options));
      uint64_t matched = 0;
      std::string val_buf;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        val_buf.assign(it->value().data(), it->value().size());
        auto get_attr = [&](int col) -> nextd::Attr {
          // If `col` is one of the N indexed attrs, it lives in the raw
          // double area at its list position's offset (8*p), NOT the
          // payload -- CATEGORICAL indexed columns store a dict-encoded id
          // there (Task 3.3), indistinguishable here from a CONTINUOUS
          // double (the query side was encoded into the same space above).
          // Small N (Phase 2-3: a handful) -- linear scan is fine.
          for (size_t p = 0; p < indexed_cols.size(); p++) {
            if (indexed_cols[p] == col)
              return nextd::IndexedValue(val_buf, static_cast<uint32_t>(p));
          }
          nextd::Attr out;
          if (nextd::DecodePayloadAttr(
                  val_buf, col, static_cast<uint32_t>(indexed_cols.size()), out))
            return out;
          // Column absent from the payload: same sentinel default
          // honk::RecordParser::ParseRecord pre-fills (json_record_parser.h
          // :56-73), so no-index's ground-truth evaluation and next_honk's
          // post-filter agree on absent fields.
          return nextd::Columns()[col].type == nextd::AttrType::CATEGORICAL
                     ? nextd::Attr(std::string("Null"))
                     : nextd::Attr(-1.0);
        };
        if (nextd::Matches(q, get_attr)) matched++;
      }
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::high_resolution_clock::now() -
                            read_start)
                            .count();

      // Task 4.2: DATA-block-only "blocks touched" = (block cache hits +
      // block reads-with-IO) for THIS query, EXCLUDING index/filter/
      // compression-dict blocks -- mirrors the exact subtraction RocksDB's
      // own test asserts on (block_based_table_reader_test.cc:272-274) for
      // the read/IO side; the cache-hit side is subtracted analogously so a
      // block already warmed by an earlier query in this process still
      // counts once. Combining hits+misses (rather than just IO reads)
      // means the count reflects every DATA block this query's iterator
      // actually decoded, regardless of cache state -- the right notion of
      // "blocks scanned" for comparing IM's intersected block set against
      // PF's single-predicate block set.
      if (report_blocks) {
        const auto* pc = rocksdb::get_perf_context();
        uint64_t data_block_hits = pc->block_cache_hit_count -
                                    pc->block_cache_index_hit_count -
                                    pc->block_cache_filter_hit_count;
        uint64_t data_block_ios = pc->block_read_count -
                                   pc->index_block_read_count -
                                   pc->filter_block_read_count -
                                   pc->compression_dict_block_read_count;
        uint64_t blocks_touched = data_block_hits + data_block_ios;
        fprintf(stderr,
                "BLOCKS query_id=%llu strategy=%s blocks_touched=%llu\n",
                static_cast<unsigned long long>(reads), read_strategy.c_str(),
                static_cast<unsigned long long>(blocks_touched));
      }

      ensure_read_csv();
      std::string joined_names;
      for (size_t i = 0; i < q.attr_names.size(); i++) {
        if (i > 0) joined_names += ",";
        joined_names += q.attr_names[i];
      }
      double selectivity =
          writes > 0 ? static_cast<double>(matched) / writes : 0.0;
      read_csv << reads << "," << q.attr_names.size() << ",\"" << joined_names
                << "\"," << elapsed_ms << "," << matched << "," << writes
                << "," << std::fixed << std::setprecision(6) << selectivity
                << "\n";
      read_csv.flush();
      reads++;
    }
    // PAUSE (and any other op type) is a no-op here; the test workload has
    // none, and honk_player's PAUSE handling (sleep) is not needed for the
    // correctness gate.
  }

  if (!put_status.ok()) {
    fprintf(stderr, "WARNING: at least one Put failed: %s\n",
            put_status.ToString().c_str());
  }

  // Task 3.3: persist each categorical indexed column's dict now that the
  // write pass has assigned every id it will ever assign -- a SEPARATE
  // --mode read process (or a future --mode both run against this DB) must
  // decode/encode using this EXACT mapping, not a freshly-started one.
  if (do_writes) {
    for (const auto& [col, dict] : cat_dicts)
      dict.Save(dict_path_for(abs_db_path, col));
  }

  // NEXT (Task 3.1): force the memtable out to an SST file before closing.
  // Without this, a small write batch (e.g. the 100-record test workload)
  // never crosses write_buffer_size and stays entirely in the
  // memtable/WAL; on reopen it would be restored via WAL replay into a
  // fresh memtable, which is scanned directly and would mask the exact bug
  // this driver's --mode write/read split exists to exercise (MANIFEST
  // replay repopulating the N global R-trees from persisted, per-file
  // SecValrange -- a purely SST-level mechanism). Flushing here makes the
  // cross-process reopen gate (scripts/test/next_reopen_xcheck.sh)
  // meaningful even on tiny workloads.
  if (do_writes) {
    rocksdb::Status flush_s = db->Flush(rocksdb::FlushOptions());
    if (!flush_s.ok()) {
      // NEXT (Task 3.1 review fix): a failed Flush here means the write pass
      // silently falls back to WAL replay on reopen (has_unpersisted_data_ is
      // never set for WAL-enabled writes, so RocksDB's shutdown-flush does
      // NOT fire either). That would test the wrong code path -- WAL replay,
      // not the MANIFEST/SST-level R-tree repopulation this gate exists to
      // verify -- and could false-positive PASS. Treat it as fatal instead
      // of a warning so the failure surfaces immediately.
      fprintf(stderr, "ERROR: Flush failed: %s\n", flush_s.ToString().c_str());
      db->Close();
      delete db;
      return 3;
    }
  }

  auto put_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - wall_start)
                          .count();

  if (drain_seconds > 0.0)
    std::this_thread::sleep_for(std::chrono::duration<double>(drain_seconds));

  db->Close();
  delete db;

  fs::current_path(original_cwd, ec);

  // NEXT (Phase 5, Task 5.2): report, per indexed attribute, the DERIVED
  // granularity (name, C_i, target E_i, delta_i, B, N) and the REALIZED
  // on-disk global-index size (bytes of db_path/global_sec_index_<i>, written
  // by the engine's VersionSet on Close). The target is what the derivation
  // aimed for; the realized size is what the per-SST gap split produced (it is
  // proportional to -- not exactly -- E_i, because each SST applies delta_i
  // independently). Both are needed for the crossover analysis; the delta-
  // sanity gate asserts the realized size is monotonic non-increasing in m.
  if (do_writes) {
    const size_t a = indexed_cols.size();
    for (size_t p = 0; p < a; p++) {
      uint64_t C = p < delta_cardinalities.size() ? delta_cardinalities[p] : 0;
      uint64_t E = p < delta_targets.size() ? delta_targets[p] : 0;
      double delta = p < sec_index_deltas.size() ? sec_index_deltas[p] : 0.0;
      fprintf(stderr,
              "DELTA attr=%s pos=%zu C_i=%llu E_i=%llu delta_i=%.9g B=%.4g "
              "N=%llu m=%.4g\n",
              indexed_attr_names[p].c_str(), p,
              static_cast<unsigned long long>(C),
              static_cast<unsigned long long>(E), delta, est_B,
              static_cast<unsigned long long>(stats_N), m_knob);

      fs::path gidx = abs_db_path / ("global_sec_index_" + std::to_string(p));
      std::error_code se;
      uintmax_t sz = fs::file_size(gidx, se);
      fprintf(stderr, "GINDEX attr=%s pos=%zu file=%s bytes=%lld\n",
              indexed_attr_names[p].c_str(), p, gidx.string().c_str(),
              se ? -1LL : static_cast<long long>(sz));
    }
  }

  printf("\n=== Summary ===\n"
         "Binding: %s\n"
         "Total writes: %llu\n"
         "Total reads: %llu\n"
         "Total time: %lldms\n",
         binding_label.c_str(), static_cast<unsigned long long>(writes),
         static_cast<unsigned long long>(reads),
         static_cast<long long>(put_elapsed));

  return 0;
}
