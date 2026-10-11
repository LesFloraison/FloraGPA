#pragma once
#include "format/Wire.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace flora::format {
using ByteBuffer = std::vector<std::uint8_t>;
using Header = std::array<std::uint8_t, HEADER_SIZE>;

// These values are file data, never pointers to native graphics objects.
struct RecordView {
    std::uint64_t id;
    std::uint8_t category;
    std::uint16_t type;
    const std::uint8_t* data;
    std::size_t size;
    std::uint8_t flags = 0;
};

// Filesystem-only library. Does not include or link D3D, capture or replay.
// Every failure leaves the destination absent (or its existing contents intact).
class ContainerWriter final {
public:
    ContainerWriter(const std::filesystem::path& destination, const Header& header);
    ~ContainerWriter();
    ContainerWriter(const ContainerWriter&) = delete;
    ContainerWriter& operator=(const ContainerWriter&) = delete;
    void append(const RecordView& record);
    void finish(const ByteBuffer& preview_bmp = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace flora::format
