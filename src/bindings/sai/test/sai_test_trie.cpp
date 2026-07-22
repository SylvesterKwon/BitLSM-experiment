// Assert-based unit test for sai_trie. Run: ./build/bin/sai_test_trie
#include "sai_trie.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <string>

using namespace experiment::sai;

static std::string Build(const std::map<std::string, std::pair<uint32_t, uint32_t>>& terms) {
  TrieWriter w;
  for (const auto& [t, oc] : terms) w.Add(t, oc.first, oc.second);
  std::string out;
  w.AppendTo(out);
  return out;
}

int main() {
  // 1. Empty trie: every lookup misses.
  {
    std::string r = Build({});
    TrieEntry e;
    assert(!TrieLookup(r.data(), "a", e));
    assert(!TrieLookup(r.data(), "", e));
  }
  // 2. Prefix relationships: "car" vs "carpet" both present; "ca" absent;
  //    "carpets" absent; empty-string term present.
  {
    std::map<std::string, std::pair<uint32_t, uint32_t>> m = {
        {"", {1, 11}}, {"car", {2, 22}}, {"carpet", {3, 33}},
        {"cat", {4, 44}}, {"dog", {5, 55}}};
    std::string r = Build(m);
    TrieEntry e;
    for (const auto& [t, oc] : m) {
      assert(TrieLookup(r.data(), t, e));
      assert(e.postings_off == oc.first && e.count == oc.second);
    }
    assert(!TrieLookup(r.data(), "ca", e));
    assert(!TrieLookup(r.data(), "carpets", e));
    assert(!TrieLookup(r.data(), "d", e));
    assert(!TrieLookup(r.data(), "z", e));
  }
  // 3. Binary-safe: terms containing 0x00 and 0xFF bytes.
  {
    std::string a("a\x00b", 3), b("a\xffz", 3);
    std::map<std::string, std::pair<uint32_t, uint32_t>> m = {{a, {7, 70}}, {b, {8, 80}}};
    std::string r = Build(m);
    TrieEntry e;
    assert(TrieLookup(r.data(), a, e) && e.count == 70);
    assert(TrieLookup(r.data(), b, e) && e.count == 80);
    assert(!TrieLookup(r.data(), std::string("a\x00", 2), e));
  }
  // 4. Fuzz vs std::map oracle (1000 random terms, 2000 probes).
  {
    std::mt19937 gen(11);
    std::map<std::string, std::pair<uint32_t, uint32_t>> m;
    for (int i = 0; i < 1000; ++i) {
      std::string t;
      int len = gen() % 12;
      for (int j = 0; j < len; ++j) t.push_back(static_cast<char>(gen() % 256));
      m[t] = {static_cast<uint32_t>(i), static_cast<uint32_t>(i * 3 + 1)};
    }
    std::string r = Build(m);
    TrieEntry e;
    for (const auto& [t, oc] : m) {
      assert(TrieLookup(r.data(), t, e) && e.postings_off == oc.first && e.count == oc.second);
    }
    for (int i = 0; i < 2000; ++i) {
      std::string t;
      int len = gen() % 12;
      for (int j = 0; j < len; ++j) t.push_back(static_cast<char>(gen() % 256));
      bool expect = m.count(t) > 0;
      assert(TrieLookup(r.data(), t, e) == expect);
    }
  }
  std::printf("sai_test_trie OK\n");
  return 0;
}
