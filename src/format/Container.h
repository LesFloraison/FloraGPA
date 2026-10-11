#pragma once
#include "Reader.h"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace flora::format {
// Immutable mapped bytes and checked container structure only. Interpretation
// and replay caches belong to the consumer. A checkpoint may throw to cancel.
class Container final {
    void *file_ = nullptr, *mapping_ = nullptr;
    const uint8_t *data_ = nullptr;
    std::shared_ptr<const uint8_t> storage_;
    uint64_t size_ = 0;
    std::map<Id, Entry> entries_;
    std::vector<Id> entryOrder_;
    std::filesystem::path path_;
    uint32_t width_ = 0, height_ = 0;
    void close() noexcept;
  public:
    using Checkpoint = std::function<void()>;
    explicit Container(const std::filesystem::path &path, const Checkpoint &checkpoint = {});
    ~Container() { close(); }
    Container(const Container &) = delete;
    Container &operator=(const Container &) = delete;
    const auto &entries() const { return entries_; }
    const auto &entryOrder() const { return entryOrder_; }
    const auto &path() const { return path_; }
    uint64_t size() const { return size_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    Bytes bytes() const { return {data_, size_t(size_)}; }
    const Entry &entry(Id id) const;
    Bytes payload(Id id, int category = -1, int type = -1) const;
};
}
