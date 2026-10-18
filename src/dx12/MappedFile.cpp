#include "MappedFile.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace flora::dx12 {
MappedFile::MappedFile(const std::filesystem::path &path) : path_(path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot open DX12 stream file: " + path.filename().string());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        throw std::runtime_error("Cannot read DX12 stream file size");
    }
    size_ = uint64_t(size.QuadPart);
    if (!size_) { CloseHandle(file); return; }
    const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        throw std::runtime_error("Cannot map DX12 stream file");
    }
    const auto data = static_cast<const uint8_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data) {
        CloseHandle(mapping); CloseHandle(file);
        throw std::runtime_error("Cannot read DX12 stream mapping");
    }
    storage_ = std::shared_ptr<const uint8_t>(data, [file, mapping](const uint8_t *p) {
        UnmapViewOfFile(p); CloseHandle(mapping); CloseHandle(file);
    });
}
format::Bytes MappedFile::bytes(uint64_t offset, uint64_t length) const {
    if (offset > size_ || length > size_ - offset)
        throw std::runtime_error("Truncated DX12 file " + path_.filename().string() +
                                 " at byte " + std::to_string(offset));
    if (!length) return {};
    return {storage_.get() + offset, size_t(length)};
}
}
