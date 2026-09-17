// Correctness cross-check: BitLSM vs the embedded and SAI index baselines.
//
// Gate before any performance use. Both engines MUST return identical match
// counts for identical queries on identical data. To avoid a vacuous pass
// (e.g. both engines returning 0 due to a read-visibility bug), every query is
// also checked against an INDEPENDENT plain-C++ ground truth computed over the
// newest-wins final state of the dataset.
//
// Flow mirrors the real two-phase write-then-read: write all records in order
// (exercising duplicate-pk updates), Close() to flush memtable to SST, then
// reopen and Scan() (exercising SST/UDI + table-iterator paths).

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "binding.h"
#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "rocksdb/types.h"

using bit_lsm::IndexType;
using bit_lsm::AttrSpec;
using bit_lsm::BitLSMOptions;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::OrClause;
using bit_lsm::QueryCondition;
using experiment::Attr;

namespace {

struct Record {
  std::string pk;
  std::vector<Attr> attrs;  // attr0: cat {c0..c7}, attr1: cont [0,1000),
                            // attr2: cat {x0..x99}
};

// ---- INDEPENDENT ground truth (plain C++, NOT EmbeddedCodec::Evaluate) ----
// A query here is a flat-AND of conditions. We evaluate each condition by
// directly comparing raw attrs, with the correct inclusive/exclusive bounds.
bool CondHolds(const QueryCondition& c, const Record& r) {
  const Attr& a = r.attrs[c.attr_idx];
  if (std::holds_alternative<std::string>(c.value)) {
    // categorical equality
    const std::string& want = std::get<std::string>(c.value);
    const std::string& got = std::get<std::string>(a);
    return c.op == CompareOp::EQUAL && got == want;
  }
  double want = std::get<double>(c.value);
  double got = std::get<double>(a);
  switch (c.op) {
    case CompareOp::EQUAL:
      return got == want;
    case CompareOp::LESS_EQUAL:
      return got <= want;
    case CompareOp::GREATER_EQUAL:
      return got >= want;
    case CompareOp::LESS:
      return got < want;
    case CompareOp::GREATER:
      return got > want;
  }
  return false;
}

// Ground truth over the newest-wins final state. flat-AND query: every clause
// is a single condition combined with AND.
uint64_t ExpectedMatches(const BitLSMQuery& q,
                         const std::vector<Record>& records,
                         const std::map<std::string, size_t>& latest) {
  uint64_t count = 0;
  for (const auto& kv : latest) {
    const Record& r = records[kv.second];
    bool all = true;
    for (const auto& clause : q.clause_groups) {
      // flat-AND: each clause has exactly one condition here.
      bool any = false;
      for (const auto& cond : clause) {
        if (CondHolds(cond, r)) {
          any = true;
          break;
        }
      }
      if (!any) {
        all = false;
        break;
      }
    }
    if (all) ++count;
  }
  return count;
}

// Build a flat-AND query from a list of single conditions.
BitLSMQuery MakeQuery(const std::vector<QueryCondition>& conds) {
  BitLSMQuery q;
  for (const auto& c : conds) q.clause_groups.push_back(OrClause{c});
  return q;
}

}  // namespace

int main(int argc, char** argv) {
  std::string bitlsm_path =
      (argc > 1) ? argv[1] : "/tmp/xcheck_bitlsm";
  std::string embedded_path =
      (argc > 2) ? argv[2] : "/tmp/xcheck_embedded";
  std::string sai_path = (argc > 3) ? argv[3] : "/tmp/xcheck_sai";

  // ---- Schema / options ----
  BitLSMOptions opts;
  opts.attr_num = 3;
  opts.attr_specs = {AttrSpec(bit_lsm::IndexType::kEquality, bit_lsm::PhysicalType::kVarBinary, 0), AttrSpec(bit_lsm::IndexType::kRange, bit_lsm::PhysicalType::kFloat, 8),
                     AttrSpec(bit_lsm::IndexType::kEquality, bit_lsm::PhysicalType::kVarBinary, 0)};
  opts.rho = 0.1;
  // read_seqno left at default for the write phase.

  // ---- Deterministic dataset ----
  const uint64_t N = 300000;
  std::mt19937 gen(12345);
  std::uniform_int_distribution<int> cat0(0, 7);      // c0..c7  (~1/8 each)
  std::uniform_real_distribution<double> cont1(0.0, 1000.0);
  std::uniform_int_distribution<int> cat2(0, 99);     // x0..x99
  std::uniform_int_distribution<int> pk_char(0, 61);
  std::uniform_real_distribution<double> dup_roll(0.0, 1.0);

  const char* kCharSet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

  std::vector<Record> records;
  records.reserve(N);
  std::vector<std::string> seen_pks;  // for duplicate selection
  std::map<std::string, size_t> latest;  // pk -> index of newest record

  for (uint64_t i = 0; i < N; ++i) {
    Record r;
    // ~1% duplicate PKs: reuse an earlier pk with DIFFERENT attrs.
    if (!seen_pks.empty() && dup_roll(gen) < 0.01) {
      std::uniform_int_distribution<size_t> pick(0, seen_pks.size() - 1);
      r.pk = seen_pks[pick(gen)];
    } else {
      std::string pk(8, '\0');
      for (int k = 0; k < 8; ++k) pk[k] = kCharSet[pk_char(gen)];
      r.pk = pk;
      seen_pks.push_back(pk);
    }

    r.attrs.resize(3);
    r.attrs[0] = std::string("c") + std::to_string(cat0(gen));
    r.attrs[1] = cont1(gen);
    r.attrs[2] = std::string("x") + std::to_string(cat2(gen));

    records.push_back(std::move(r));
    latest[records.back().pk] = records.size() - 1;  // newest wins
  }

  const uint64_t distinct_pks = latest.size();
  std::cout << "dataset: " << N << " records, " << distinct_pks
            << " distinct pks (" << (N - distinct_pks) << " updates)\n";

  // ---- Query battery (flat-AND) ----
  struct QSpec {
    std::string name;
    BitLSMQuery query;
  };
  std::vector<QSpec> queries;
  queries.push_back({"Q1 attr0=='c3'",
                     MakeQuery({{0, CompareOp::EQUAL, std::string("c3")}})});
  queries.push_back({"Q2 attr1 in [200,400)",
                     MakeQuery({{1, CompareOp::GREATER_EQUAL, 200.0},
                                {1, CompareOp::LESS, 400.0}})});
  queries.push_back({"Q3 attr0=='c3' AND attr1>=500",
                     MakeQuery({{0, CompareOp::EQUAL, std::string("c3")},
                                {1, CompareOp::GREATER_EQUAL, 500.0}})});
  queries.push_back({"Q4 attr2=='x42'",
                     MakeQuery({{2, CompareOp::EQUAL, std::string("x42")}})});
  queries.push_back({"Q5 attr0=='zzz' (zero)",
                     MakeQuery({{0, CompareOp::EQUAL, std::string("zzz")}})});
  queries.push_back({"Q6 attr1>=0 (all)",
                     MakeQuery({{1, CompareOp::GREATER_EQUAL, 0.0}})});

  // ---- Ground truth (independent) ----
  std::vector<uint64_t> expected(queries.size());
  for (size_t i = 0; i < queries.size(); ++i)
    expected[i] = ExpectedMatches(queries[i].query, records, latest);

  // Anti-vacuous guard on the generator itself.
  // Q1 (idx 0), Q2 (idx 1), Q6 (idx 5) must be non-zero, Q6 must equal all pks.
  if (expected[0] == 0 || expected[1] == 0 || expected[5] == 0) {
    std::cout << "FAIL: ground-truth degenerate (Q1/Q2/Q6 expected non-zero): "
              << "Q1=" << expected[0] << " Q2=" << expected[1]
              << " Q6=" << expected[5] << "\n";
    return 1;
  }
  if (expected[5] != distinct_pks) {
    std::cout << "FAIL: Q6 (all-match) expected " << expected[5]
              << " != distinct_pks " << distinct_pks << "\n";
    return 1;
  }

  // ---- Run each engine through the two-phase write-then-read ----
  auto run_method = [&](const std::string& method,
                        const std::string& path) -> std::vector<uint64_t> {
    // Write phase.
    std::vector<std::string> wargv_s = {method, "--exp_type", "write_seq",
                                        "--rho", "0.1", "--bloom_bits", "100"};
    std::vector<char*> wargv;
    for (auto& s : wargv_s) wargv.push_back(const_cast<char*>(s.c_str()));

    auto binding = experiment::CreateBinding(method);
    BitLSMOptions write_opts = opts;  // read_seqno default
    binding->Open(static_cast<int>(wargv.size()), wargv.data(), path,
                  write_opts);
    std::string empty_payload;
    for (const auto& r : records) binding->Put(r.pk, r.attrs, empty_payload);
    binding->Close();  // flush memtable to SST

    // Read phase: reopen with full visibility.
    auto rbinding = experiment::CreateBinding(method);
    BitLSMOptions read_opts = opts;
    read_opts.read_seqno = rocksdb::kMaxSequenceNumber;
    rbinding->Open(static_cast<int>(wargv.size()), wargv.data(), path,
                   read_opts);

    std::vector<uint64_t> got(queries.size());
    for (size_t i = 0; i < queries.size(); ++i) {
      auto sr = rbinding->Scan(queries[i].query);
      got[i] = sr.matched;
    }
    rbinding->Close();
    return got;
  };

  std::cout << "running bitlsm...\n";
  std::vector<uint64_t> bitlsm = run_method("bitlsm", bitlsm_path);
  std::cout << "running embedded...\n";
  std::vector<uint64_t> embedded = run_method("embedded", embedded_path);
  std::cout << "running embedded-postings...\n";
  std::vector<uint64_t> sai = run_method("embedded-postings", sai_path);

  // ---- Compare against ground truth ----
  bool ok = true;
  std::cout << "\n  query | expected | bitlsm | embedded | embedded-postings\n";
  std::cout << "  --------------------------------------------------\n";
  for (size_t i = 0; i < queries.size(); ++i) {
    std::cout << "  " << queries[i].name << " | " << expected[i] << " | "
              << bitlsm[i] << " | " << embedded[i] << " | " << sai[i] << "\n";
  }
  std::cout << "\n";

  for (size_t i = 0; i < queries.size(); ++i) {
    if (embedded[i] != expected[i]) {
      std::cout << "FAIL: " << queries[i].name
                << " embedded=" << embedded[i]
                << " expected=" << expected[i] << "\n";
      ok = false;
    }
    if (sai[i] != expected[i]) {
      std::cout << "FAIL: " << queries[i].name << " sai=" << sai[i]
                << " expected=" << expected[i] << "\n";
      ok = false;
    }
    if (bitlsm[i] != expected[i]) {
      std::cout << "FAIL: " << queries[i].name
                << " bitlsm=" << bitlsm[i]
                << " expected=" << expected[i] << "\n";
      ok = false;
    }
  }

  if (!ok) return 1;

  std::cout << "XCHECK OK\n";
  return 0;
}
