#include "global_sabi_factory.h"

#include <cstdio>
#include <cstdlib>

#include "global_sabi_builder.h"

namespace experiment::global_bins {

GlobalSABIFactory::GlobalSABIFactory(const bit_lsm::BitLSMOptions& options,
                                     std::shared_ptr<const BinPolicy> policy)
    : options_(options),
      schema_(bit_lsm::SABISchema::FromOptions(options)),
      policy_(std::move(policy)) {
  if (!policy_) {
    fprintf(stderr, "[GlobalSABIFactory] no bin policy\n");
    abort();
  }
  rocksdb::Status s = CheckBinPolicy(*policy_, schema_);
  if (!s.ok()) {
    fprintf(stderr, "[GlobalSABIFactory] %s\n", s.ToString().c_str());
    abort();
  }
}

rocksdb::UserDefinedIndexBuilder* GlobalSABIFactory::NewBuilder() const {
  return new GlobalSABIBuilder(
      schema_, std::make_unique<bit_lsm::ValueLayoutExtractor>(options_),
      policy_);
}

std::unique_ptr<rocksdb::UserDefinedIndexReader> GlobalSABIFactory::NewReader(
    rocksdb::Slice& index_block) const {
  return reader_factory_.NewReader(index_block);
}

rocksdb::Status GlobalSABIFactory::NewReader(
    const rocksdb::UserDefinedIndexOption& option, rocksdb::Slice& index_block,
    std::unique_ptr<rocksdb::UserDefinedIndexReader>& reader) const {
  return reader_factory_.NewReader(option, index_block, reader);
}

}  // namespace experiment::global_bins
