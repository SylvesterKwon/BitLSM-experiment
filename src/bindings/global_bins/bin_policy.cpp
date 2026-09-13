#include "bin_policy.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace experiment::global_bins {

namespace {

constexpr char kMagic[8] = {'G', 'B', 'I', 'N', 'P', 'O', 'L', '1'};

void PutPod(std::string* out, const auto& v) {
  out->append(reinterpret_cast<const char*>(&v), sizeof(v));
}

void PutString(std::string* out, const std::string& s) {
  PutPod(out, static_cast<uint32_t>(s.size()));
  out->append(s);
}

// Bounds-checked cursor: a policy file is plain input, not a checksummed
// block, so every read is checked.
struct Cursor {
  const char* p;
  const char* end;

  bool Pod(auto* v) {
    if (static_cast<size_t>(end - p) < sizeof(*v)) return false;
    std::memcpy(v, p, sizeof(*v));
    p += sizeof(*v);
    return true;
  }
  bool String(std::string* s) {
    uint32_t len;
    if (!Pod(&len) || static_cast<size_t>(end - p) < len) return false;
    s->assign(p, len);
    p += len;
    return true;
  }
  bool Bytes(bit_lsm::BytesList* list) {
    uint32_t count;
    if (!Pod(&count)) return false;
    if (static_cast<size_t>(end - p) < uint64_t{count} * sizeof(uint32_t))
      return false;
    list->ends.resize(count);
    std::memcpy(list->ends.data(), p, count * sizeof(uint32_t));
    p += count * sizeof(uint32_t);
    uint32_t prev = 0;
    for (uint32_t e : list->ends) {
      if (e < prev) return false;
      prev = e;
    }
    if (static_cast<size_t>(end - p) < prev) return false;
    list->arena.assign(p, prev);
    p += prev;
    return true;
  }
};

}  // namespace

rocksdb::Status SaveBinPolicy(const BinPolicy& policy,
                              const std::string& path) {
  const uint32_t attr_num = static_cast<uint32_t>(policy.index_types.size());
  if (policy.bitmap_nums.size() != attr_num ||
      policy.binning_policy.size() != attr_num)
    return rocksdb::Status::InvalidArgument("inconsistent bin policy");

  std::string out(kMagic, sizeof(kMagic));
  PutPod(&out, policy.rho);
  PutPod(&out, policy.rows);
  PutString(&out, policy.source);
  PutPod(&out, policy.source_bytes);
  PutPod(&out, attr_num);
  for (uint32_t i = 0; i < attr_num; ++i) {
    PutPod(&out, static_cast<uint8_t>(policy.index_types[i]));
    PutPod(&out, policy.bitmap_nums[i]);
    if (policy.index_types[i] == bit_lsm::IndexType::kRange) {
      std::get<bit_lsm::BytesList>(policy.binning_policy[i]).Serialize(&out);
    } else {
      const auto& entries =
          std::get<std::vector<std::pair<std::string, uint32_t>>>(
              policy.binning_policy[i]);
      PutPod(&out, static_cast<uint32_t>(entries.size()));
      for (const auto& [value, bin] : entries) {
        PutString(&out, value);
        PutPod(&out, bin);
      }
    }
  }
  out.append(kMagic, sizeof(kMagic));

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!f) return rocksdb::Status::IOError("cannot write bin policy: " + path);
  return rocksdb::Status::OK();
}

rocksdb::Status LoadBinPolicy(const std::string& path, BinPolicy* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return rocksdb::Status::IOError("cannot open bin policy: " + path);
  const std::string data((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  const auto corrupt = [&path](const char* what) {
    return rocksdb::Status::Corruption("bin policy " + path + ": " + what);
  };
  if (data.size() < 2 * sizeof(kMagic) ||
      std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0 ||
      std::memcmp(data.data() + data.size() - sizeof(kMagic), kMagic,
                  sizeof(kMagic)) != 0)
    return corrupt("bad magic");

  Cursor c{data.data() + sizeof(kMagic),
           data.data() + data.size() - sizeof(kMagic)};
  BinPolicy p;
  uint32_t attr_num;
  if (!c.Pod(&p.rho) || !c.Pod(&p.rows) || !c.String(&p.source) ||
      !c.Pod(&p.source_bytes) || !c.Pod(&attr_num))
    return corrupt("truncated header");
  for (uint32_t i = 0; i < attr_num; ++i) {
    uint8_t type;
    uint32_t bins;
    if (!c.Pod(&type) || !c.Pod(&bins)) return corrupt("truncated attr");
    if (type == static_cast<uint8_t>(bit_lsm::IndexType::kRange)) {
      bit_lsm::BytesList bounds;
      if (!c.Bytes(&bounds)) return corrupt("truncated boundaries");
      if (bounds.size() != uint64_t{bins} + 1)
        return corrupt("boundary count is not bins + 1");
      p.binning_policy.emplace_back(std::move(bounds));
    } else if (type == static_cast<uint8_t>(bit_lsm::IndexType::kEquality)) {
      uint32_t count;
      if (!c.Pod(&count)) return corrupt("truncated entry count");
      std::vector<std::pair<std::string, uint32_t>> entries(count);
      for (auto& [value, bin] : entries) {
        if (!c.String(&value) || !c.Pod(&bin))
          return corrupt("truncated entry");
        if (bin >= bins) return corrupt("entry bin out of range");
      }
      p.binning_policy.emplace_back(std::move(entries));
    } else {
      return corrupt("unknown index type");
    }
    p.index_types.push_back(static_cast<bit_lsm::IndexType>(type));
    p.bitmap_nums.push_back(bins);
  }
  if (c.p != c.end) return corrupt("trailing bytes");
  *out = std::move(p);
  return rocksdb::Status::OK();
}

rocksdb::Status CheckBinPolicy(const BinPolicy& policy,
                               const bit_lsm::SABISchema& schema) {
  if (policy.rho != schema.rho)
    return rocksdb::Status::InvalidArgument(
        "bin policy was built for rho=" + std::to_string(policy.rho) +
        ", the DB is opened with rho=" + std::to_string(schema.rho));
  if (policy.index_types != schema.index_types)
    return rocksdb::Status::InvalidArgument(
        "bin policy attributes do not match the schema");
  return rocksdb::Status::OK();
}

}  // namespace experiment::global_bins
