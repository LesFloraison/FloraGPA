#pragma once
#include "MappedFile.h"
#include "core/Cancellation.h"
#include <map>
#include <vector>

namespace flora::dx12 {
struct StreamPacket {
    uint64_t type{}, timestamp{}, flags{}, offset{}, size{};
    uint32_t file{};
    bool initialization = false;
};
struct StreamFile {
    std::shared_ptr<const MappedFile> storage;
    uint64_t thread{};
};
class StreamDirectory final {
    std::filesystem::path path_;
    std::vector<StreamFile> files_;
    std::shared_ptr<const MappedFile> resources_;
    std::vector<StreamPacket> packets_;
    uint32_t width_{}, height_{};
    size_t initializationCount_{};
    void packets(uint32_t file, uint64_t start, uint64_t end, bool initialization,
                 const CancelCheck &cancelled);
    void metadata(const CancelCheck &cancelled);
  public:
    explicit StreamDirectory(const std::filesystem::path &path, const CancelCheck &cancelled = {});
    const auto &path() const { return path_; }
    const auto &files() const { return files_; }
    const auto &packets() const { return packets_; }
    const MappedFile &resources() const { return *resources_; }
    size_t initializationCount() const { return initializationCount_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    format::Bytes payload(const StreamPacket &packet) const;
};
std::string streamPacketName(uint64_t type);
bool streamPacketWork(uint64_t type);
}
