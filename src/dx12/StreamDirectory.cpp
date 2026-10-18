#include "StreamDirectory.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_map>

namespace flora::dx12 {
using format::Reader;
namespace {
uint64_t number(std::string_view value) {
    uint64_t out{};
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("Invalid DX12 thread filename");
    return out;
}
const std::unordered_map<uint64_t, std::string> names{
#include "StreamNames.inc"
};
}
std::string streamPacketName(uint64_t type) {
    auto it = names.find(type);
    return it == names.end() ? "Unknown stream packet " + std::to_string(type) : it->second;
}
bool streamPacketWork(uint64_t type) {
    switch (type) {
    case 0x1ae3c190d8b1eULL: case 0x1ae3c86b1c3b9ULL:
    case 0x1ae3cef0e6c17ULL: case 0x1ae3ccddf9f8cULL: return true;
    default: return false;
    }
}
StreamDirectory::StreamDirectory(const std::filesystem::path &path, const CancelCheck &cancelled)
    : path_(std::filesystem::canonical(path)) {
    checkCancellation(cancelled);
    if (!std::filesystem::is_directory(path_) || !std::filesystem::is_directory(path_ / "D3D12"))
        throw std::runtime_error("Not a DX12 single-frame directory");
    files_.push_back({std::make_shared<MappedFile>(path_ / "metadata"), 0});
    resources_ = std::make_shared<MappedFile>(path_ / "resources");
    metadata(cancelled);
    initializationCount_ = packets_.size();
    std::map<uint64_t, std::pair<uint64_t, std::filesystem::path>> threads;
    for (const auto &entry : std::filesystem::directory_iterator(path_)) {
        checkCancellation(cancelled);
        const auto name = entry.path().filename().string();
        if (!name.starts_with("thread-")) continue;
        const auto suffix = std::string_view(name).substr(7);
        const auto dash = suffix.find('-');
        if (dash == std::string_view::npos || !entry.is_regular_file())
            throw std::runtime_error("Invalid DX12 thread stream");
        const auto thread = number(suffix.substr(0, dash)), ordinal = number(suffix.substr(dash + 1));
        if (!threads.emplace(ordinal, std::pair{thread, entry.path()}).second)
            throw std::runtime_error("Duplicate DX12 thread stream ordinal");
    }
    if (threads.empty()) throw std::runtime_error("DX12 directory contains no thread streams");
    for (const auto &[ordinal, entry] : threads) {
        (void)ordinal;
        auto file = std::make_shared<MappedFile>(entry.second);
        if (files_.size() >= UINT32_MAX) throw std::runtime_error("Too many DX12 stream files");
        const auto index = uint32_t(files_.size());
        files_.push_back({file, entry.first});
        packets(index, 0, file->size(), false, cancelled);
    }
    // CPU capture order only. GPU order requires submissions, lists and fences.
    std::stable_sort(packets_.begin() + initializationCount_, packets_.end(),
                     [](const auto &a, const auto &b) { return a.timestamp < b.timestamp; });
    checkCancellation(cancelled);
}
void StreamDirectory::packets(uint32_t file, uint64_t start, uint64_t end, bool initialization,
                              const CancelCheck &cancelled) {
    const auto &mapped = *files_.at(file).storage;
    mapped.bytes(start, end >= start ? end - start : UINT64_MAX);
    for (auto offset = start; offset < end;) {
        if (packets_.size() % 1024 == 0) checkCancellation(cancelled);
        if (end - offset < 32)
            throw std::runtime_error("Truncated DX12 packet header in " + mapped.path().filename().string() +
                                     " at byte " + std::to_string(offset));
        Reader reader(mapped.bytes(offset, 32));
        StreamPacket packet{reader.read<uint64_t>(), reader.read<uint64_t>(), reader.read<uint64_t>(),
                            offset, reader.read<uint64_t>(), file, initialization};
        if (packet.size > end - offset - 32)
            throw std::runtime_error("Truncated DX12 packet payload in " + mapped.path().filename().string() +
                                     " at byte " + std::to_string(offset));
        packets_.push_back(packet);
        offset += 32 + packet.size;
    }
}
void StreamDirectory::metadata(const CancelCheck &cancelled) {
    const auto &mapped = *files_[0].storage;
    Reader header(mapped.bytes(0, 0x288));
    if (header.read<uint64_t>() != 10 || header.read<uint64_t>() != 0x415047 ||
        header.read<uint32_t>() != 3)
        throw std::runtime_error("Unsupported DX12 stream metadata header");
    width_ = header.read<uint32_t>(); height_ = header.read<uint32_t>();
    for (uint64_t offset = 0; offset < mapped.size();) {
        checkCancellation(cancelled);
        Reader r(mapped.bytes(offset, mapped.size() - offset));
        const auto type = r.read<uint64_t>();
        uint64_t length = 0;
        switch (type) {
        case 10:
            if (offset != 0) throw std::runtime_error("Duplicate DX12 metadata header");
            length = 0x288; break;
        case 9: length = 0x418; break;
        case 2: length = 24; break;
        case 5: length = 40; break;
        case 6:
            r.skip(8); length = uint64_t(r.read<uint32_t>()) + 20; break;
        case 8: {
            r.skip(8);
            const auto count = r.read<uint32_t>();
            if (count > r.remaining() / 12) throw std::runtime_error("Truncated DX12 version records");
            for (uint64_t i = 0; i < uint64_t(count) * 3; ++i) {
                checkCancellation(cancelled);
                r.skip(r.read<uint32_t>());
            }
            length = r.position(); break;
        }
        case 3: {
            r.skip(16);
            const auto end = r.read<uint64_t>();
            const auto version = r.read<uint32_t>(); r.skip(4);
            if (version > 3) throw std::runtime_error("Unsupported DX12 keyframe version");
            uint64_t storedEnd = 0;
            if (version >= 1) r.skip(8);
            if (version >= 2) r.skip(8);
            if (version >= 3) storedEnd = r.read<uint64_t>();
            if (storedEnd) throw std::runtime_error("Alternate DX12 keyframe storage is not implemented");
            const auto start = offset + r.position();
            if (end < start || end > mapped.size()) throw std::runtime_error("Invalid DX12 keyframe bounds");
            packets(0, start, end, true, cancelled);
            length = end - offset; break;
        }
        default:
            throw std::runtime_error("Unsupported DX12 metadata record " + std::to_string(type) +
                                     " at byte " + std::to_string(offset));
        }
        mapped.bytes(offset, length);
        offset += length;
    }
}
format::Bytes StreamDirectory::payload(const StreamPacket &packet) const {
    return files_.at(packet.file).storage->bytes(packet.offset + 32, packet.size);
}
}
