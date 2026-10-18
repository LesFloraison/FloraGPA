#pragma once
#include "format/Reader.h"
#include <filesystem>
#include <memory>

namespace flora::dx12 {
// Immutable, read-only storage. Mapping does not copy multi-gigabyte resources.
class MappedFile final {
    std::shared_ptr<const uint8_t> storage_;
    uint64_t size_{};
    std::filesystem::path path_;
  public:
    explicit MappedFile(const std::filesystem::path &path);
    uint64_t size() const { return size_; }
    const auto &path() const { return path_; }
    format::Bytes bytes(uint64_t offset, uint64_t length) const;
};
}
