#pragma once
// Serialized byte-trie term dictionary for categorical attrs.
// Corresponds to Cassandra's TrieTermsDictionaryWriter / TrieTermsDictionaryReader
// (src/java/org/apache/cassandra/index/sai/disk/v1/trie/TrieTermsDictionaryWriter.java):
// byte-comparable trie, leaf payload = (postings offset, count). Deviation D8:
// plain DFS serialization instead of the page-aware incremental codec.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace experiment::sai {

struct TrieEntry {
  uint32_t postings_off;
  uint32_t count;
};

class TrieWriter {
 public:
  // Terms may be added in any order (std::map sorts); duplicates assert.
  void Add(std::string_view term, uint32_t postings_off, uint32_t count);
  // Appends [u32 root_off][nodes...] at out's end; node offsets are relative
  // to the first appended byte (the region base passed to TrieLookup).
  void AppendTo(std::string& out) const;

 private:
  std::map<std::string, TrieEntry, std::less<>> terms_;
};

// region must point at the first byte TrieWriter appended.
bool TrieLookup(const char* region, std::string_view term, TrieEntry& e);

}  // namespace experiment::sai
