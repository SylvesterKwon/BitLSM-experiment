#include "sai_trie.h"
#include "sai_coding.h"
#include <cassert>
#include <memory>
#include <vector>

namespace experiment::sai {

void TrieWriter::Add(std::string_view term, uint32_t postings_off, uint32_t count) {
  auto [it, inserted] = terms_.emplace(std::string(term), TrieEntry{postings_off, count});
  (void)it;
  assert(inserted && "duplicate term");
}

namespace {
struct Node {
  bool has_value = false;
  TrieEntry entry{0, 0};
  std::map<uint8_t, std::unique_ptr<Node>> children;
};

// Post-order DFS: children serialized before the parent so child offsets are known.
uint32_t Serialize(const Node& n, std::string& out, size_t region_base) {
  std::vector<std::pair<uint8_t, uint32_t>> child_offs;
  child_offs.reserve(n.children.size());
  for (const auto& [b, ch] : n.children)
    child_offs.emplace_back(b, Serialize(*ch, out, region_base));
  const uint32_t my_off = static_cast<uint32_t>(out.size() - region_base);
  out.push_back(static_cast<char>(n.has_value ? 1 : 0));
  if (n.has_value) {
    PutU32(out, n.entry.postings_off);
    PutU32(out, n.entry.count);
  }
  PutU16(out, static_cast<uint16_t>(child_offs.size()));
  for (const auto& [b, off] : child_offs) {
    out.push_back(static_cast<char>(b));
    PutU32(out, off);
  }
  return my_off;
}
}  // namespace

void TrieWriter::AppendTo(std::string& out) const {
  const size_t region_base = out.size();
  PutU32(out, 0);  // root_off placeholder
  Node root;
  for (const auto& [term, entry] : terms_) {  // prefix sharing via incremental insert
    Node* cur = &root;
    for (unsigned char b : term) {
      auto& slot = cur->children[b];
      if (!slot) slot = std::make_unique<Node>();
      cur = slot.get();
    }
    cur->has_value = true;
    cur->entry = entry;
  }
  const uint32_t root_off = Serialize(root, out, region_base);
  PatchU32(out, region_base, root_off);
}

bool TrieLookup(const char* region, std::string_view term, TrieEntry& e) {
  const char* p = region + GetU32(region);  // root node
  for (unsigned char b : term) {
    // Skip this node's value to reach its child list.
    const bool has_value = (*p & 1) != 0;
    const char* q = p + 1 + (has_value ? 8 : 0);
    const uint16_t nch = GetU16(q);
    q += 2;
    // Linear scan of the sorted (byte, off) child list; fan-out is small.
    const char* found = nullptr;
    for (uint16_t i = 0; i < nch; ++i) {
      const uint8_t cb = static_cast<uint8_t>(q[i * 5]);
      if (cb == b) { found = q + i * 5 + 1; break; }
      if (cb > b) break;
    }
    if (!found) return false;
    p = region + GetU32(found);
  }
  if ((*p & 1) == 0) return false;
  e.postings_off = GetU32(p + 1);
  e.count = GetU32(p + 5);
  return true;
}

}  // namespace experiment::sai
