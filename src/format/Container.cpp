#include "Container.h"
#include "Wire.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
namespace flora::format {
void Container::close() noexcept {
    if (storage_) {
        storage_.reset();
        data_ = nullptr;
    }
    if (data_)
        UnmapViewOfFile(data_);
    if (mapping_)
        CloseHandle(mapping_);
    if (file_ && file_ != INVALID_HANDLE_VALUE)
        CloseHandle(file_);
    data_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
}
Container::Container(const std::filesystem::path &path, const Checkpoint &checkpoint) : path_(path) {
    try {
        if (checkpoint) checkpoint();
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open capture (Windows error " + std::to_string(GetLastError()) +
                                     ")");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_, &length) || length.QuadPart < HEADER_SIZE)
            throw std::runtime_error("Capture header is truncated");
        size_ = static_cast<uint64_t>(length.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_)
            throw std::runtime_error("Cannot map capture");
        data_ = static_cast<const uint8_t *>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_)
            throw std::runtime_error("Cannot read capture mapping");
        Reader header({data_, HEADER_SIZE});
        if (header.read<uint32_t>() != 0x41504749 || header.read<uint32_t>() != HEADER_SIZE ||
            header.read<uint32_t>() != 3)
            throw std::runtime_error("Unsupported IGPA header");
        auto count = header.read<uint32_t>();
        if (std::memcmp(data_ + API_OFFSET, "DX11", 4))
            throw std::runtime_error("Not a DX11 capture");
        Reader tail({data_ + INDEX_OFFSET, 0x34});
        auto table = tail.read<uint64_t>();
        if (count > MAX_ENTRIES || table < HEADER_SIZE || table > size_ || uint64_t(count) * ENTRY_SIZE > size_ - table)
            throw std::runtime_error("Invalid entry table bounds");
        tail.skip(0x120 - 0xfc);
        width_ = tail.read<uint32_t>();
        height_ = tail.read<uint32_t>();
        Reader entries({data_ + table, size_t(count) * ENTRY_SIZE});
        for (uint32_t i = 0; i < count; ++i) {
            if (i % 1024 == 0)
                if (checkpoint) checkpoint();
            Entry e{entries.read<Id>(),      entries.read<uint64_t>(), entries.read<uint32_t>(),
                    entries.read<uint8_t>(), entries.read<uint8_t>(),  entries.read<uint16_t>()};
            if (e.offset < HEADER_SIZE || e.offset > table || e.size > table - e.offset ||
                !entries_.emplace(e.id, e).second)
                throw std::runtime_error("Invalid or duplicate entry " + std::to_string(e.id));
            entryOrder_.push_back(e.id);
        }
        if (checkpoint) checkpoint();
        // Transfer the mapping only after parsing succeeds. Shared ownership lets
        // an overlay outlive its source Frame without remapping or copying bytes.
        auto file = file_, mapping = mapping_;
        const auto *mapped = data_;
        file_ = mapping_ = nullptr;
        data_ = nullptr;
        storage_ = std::shared_ptr<const uint8_t>(mapped, [file, mapping](const uint8_t *data) {
            UnmapViewOfFile(data);
            CloseHandle(mapping);
            CloseHandle(file);
        });
        data_ = mapped;
    } catch (...) {
        close();
        throw;
    }
}
const Entry &Container::entry(Id id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) throw std::runtime_error("Missing capture entry " + std::to_string(id));
    return it->second;
}
Bytes Container::payload(Id id, int category, int type) const {
    const auto &e = entry(id);
    if ((category >= 0 && e.category != category) || (type >= 0 && e.type != type))
        throw std::runtime_error("Unexpected record type for " + std::to_string(id));
    return {data_ + e.offset, e.size};
}
}
