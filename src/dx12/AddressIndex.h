#pragma once
#include "StreamDirectory.h"
#include <map>
#include <vector>

namespace flora::dx12 {
struct AddressReference { uint64_t object{}, creation{}, offset{}, available{}; };
// Captured GPU addresses remain opaque 64-bit integers. Resolution identifies
// a resource lifetime and byte offset; native addresses are queried separately.
class AddressIndex final {
    struct Range { uint64_t base{}, end{}, query{}, object{}, creation{}, prefixEnd{}; };
    const StreamDirectory &directory_;
    std::map<uint64_t, std::vector<uint64_t>> creations_;
    std::vector<Range> ranges_;
    uint64_t creation(uint64_t object, uint64_t at) const;
  public:
    explicit AddressIndex(const StreamDirectory &, const CancelCheck &cancelled = {});
    AddressReference resolve(uint64_t address, uint64_t atRecord, uint64_t bytes = 1) const;
    std::vector<AddressReference> candidates(uint64_t address, uint64_t atRecord, uint64_t bytes = 1) const;
    size_t rangeCount() const { return ranges_.size(); }
};
}
