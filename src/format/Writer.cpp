#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "format/Writer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace flora::format {
namespace {
[[noreturn]] void win_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
void put(std::uint8_t* target, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) target[i] = static_cast<std::uint8_t>(value >> (8*i));
}
std::uint64_t get(const std::uint8_t* source, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::uint64_t(source[i]) << (8*i);
    return value;
}
}

struct ContainerWriter::Impl {
    std::filesystem::path destination, temporary;
    Header header;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::uint64_t offset = 0;
    std::map<std::uint64_t, std::array<std::uint8_t, ENTRY_SIZE>> entries;
    bool committed = false;
    bool failed = false;
    void write(const void* buffer, std::size_t count) {
        auto bytes = static_cast<const std::uint8_t*>(buffer);
        while (count) {
            DWORD part = static_cast<DWORD>(std::min<std::size_t>(count, 16*1024*1024)), written = 0;
            if (!WriteFile(file, bytes, part, &written, nullptr)) win_error("Write capture");
            if (written != part) throw std::runtime_error("Short capture write");
            bytes += written; count -= written; offset += written;
        }
    }
    ~Impl() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
    }
};

ContainerWriter::ContainerWriter(const std::filesystem::path& destination, const Header& header)
    : impl_(std::make_unique<Impl>()) {
    if (std::memcmp(header.data(), "IGPA", 4) || get(header.data()+4,4) != HEADER_SIZE ||
        get(header.data()+8,4) != VERSION || std::memcmp(header.data()+API_OFFSET,"DX11",4))
        throw std::invalid_argument("Unsupported IGPA writer header");
    impl_->destination = std::filesystem::absolute(destination);
    impl_->header = header;
    if (std::filesystem::exists(impl_->destination)) throw std::runtime_error("Capture destination exists");
    std::random_device random;
    for (unsigned attempt=0; attempt<32; ++attempt) {
        impl_->temporary = impl_->destination;
        impl_->temporary += L"." + std::to_wstring(random()) + L".partial";
        impl_->file = CreateFileW(impl_->temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (impl_->file != INVALID_HANDLE_VALUE) break;
        DWORD error=GetLastError();
        // Never delete a colliding temporary file owned by somebody else.
        impl_->temporary.clear();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            SetLastError(error); win_error("Create capture temporary");
        }
    }
    if (impl_->file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot allocate capture temporary");
    impl_->write(header.data(), header.size());
}
ContainerWriter::~ContainerWriter() = default;

void ContainerWriter::append(const RecordView& record) {
    if (impl_->committed || impl_->failed) throw std::logic_error("Writer is not writable");
    try {
        if (record.size > std::numeric_limits<std::uint32_t>::max() ||
            impl_->entries.size() >= MAX_ENTRIES || (record.size && !record.data))
            throw std::invalid_argument("Invalid capture record size");
        const auto encoded=encode_entry({record.id,impl_->offset,static_cast<std::uint32_t>(record.size),
                                         record.flags,record.category,record.type});
        if (!impl_->entries.emplace(record.id,encoded).second) throw std::invalid_argument("Duplicate record ID");
        impl_->write(record.data,record.size);
    } catch (...) { impl_->failed=true; throw; }
}

void ContainerWriter::finish(const ByteBuffer& preview_bmp) {
    if (impl_->committed || impl_->failed) throw std::logic_error("Writer is not writable");
    try {
        if (preview_bmp.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("Preview exceeds wire limit");
        const auto table = impl_->offset;
        for (const auto& entry : impl_->entries) impl_->write(entry.second.data(),entry.second.size());
        const auto preview = impl_->offset;
        impl_->write(preview_bmp.data(),preview_bmp.size());
        auto& h = impl_->header;
        put(h.data()+ENTRY_COUNT_OFFSET,impl_->entries.size(),4);
        put(h.data()+INDEX_OFFSET,table,8);
        // This writer uses direct records, without the optional pooled-data area.
        put(h.data()+0x100,table,8); put(h.data()+0x108,0,4); put(h.data()+0x10c,0,8);
        put(h.data()+PREVIEW_OFFSET,preview,8); put(h.data()+PREVIEW_OFFSET+8,preview_bmp.size(),4);
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(impl_->file,zero,nullptr,FILE_BEGIN)) win_error("Seek capture header");
        impl_->write(h.data(),h.size());
        if (!FlushFileBuffers(impl_->file)) win_error("Flush capture");
        if (!CloseHandle(impl_->file)) win_error("Close capture");
        impl_->file=INVALID_HANDLE_VALUE;
        // No REPLACE_EXISTING: a concurrent creator's destination must survive.
        if (!MoveFileExW(impl_->temporary.c_str(),impl_->destination.c_str(),MOVEFILE_WRITE_THROUGH))
            win_error("Publish capture without overwrite");
        impl_->temporary.clear(); impl_->committed=true;
    } catch (...) { impl_->failed=true; throw; }
}
} // namespace flora::format
