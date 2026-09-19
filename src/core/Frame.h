#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace flora {
using Id = uint64_t;
using Bytes = std::span<const uint8_t>;
class Reader {
    Bytes bytes_;
    size_t pos_ = 0;

  public:
    explicit Reader(Bytes bytes) : bytes_(bytes) {}
    size_t position() const { return pos_; }
    size_t remaining() const { return bytes_.size() - pos_; }
    Bytes take(size_t n) {
        if (n > remaining())
            throw std::runtime_error("Truncated capture record at byte " + std::to_string(pos_));
        auto out = bytes_.subspan(pos_, n);
        pos_ += n;
        return out;
    }
    void skip(size_t n) { take(n); }
    template <class T> T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        auto b = take(sizeof(T));
        std::memcpy(&value, b.data(), sizeof(T));
        return value;
    }
    template <class T, size_t N> std::array<T, N> array() {
        std::array<T, N> out{};
        for (auto &value : out)
            value = read<T>();
        return out;
    }
    bool flag() {
        auto x = read<uint8_t>();
        if (x > 1)
            throw std::runtime_error("Invalid optional field flag");
        return x != 0;
    }
    void end() const {
        if (remaining())
            throw std::runtime_error("Unexpected trailing capture bytes");
    }
};
struct Entry {
    Id id;
    uint64_t offset;
    uint32_t size;
    uint8_t flags, category;
    uint16_t type;
};
struct Resource {
    Id id{}, original{}, device{}, data{};
    uint16_t type{};
    std::vector<uint32_t> desc;
};
struct TextureInfo {
    uint32_t width{}, height{}, depth{}, mips{}, layers{}, format{}, samples{}, dimension{};
};
struct Stage {
    std::array<Id, 14> cb;
    std::array<Id, 16> samplers;
    Id shader;
    std::array<Id, 128> srv;
    std::array<Id, 256> classes;
    uint32_t classCount;
};
struct State {
    Id ib{}, layout{};
    uint32_t ibFormat{}, ibOffset{}, topology{};
    std::array<Id, 32> vb{};
    std::array<uint32_t, 32> strides{}, offsets{};
    std::array<Stage, 6> stages{}; // VS HS DS GS PS CS
    std::array<Id, 4> so{};
    std::array<uint32_t, 4> soOffsets{}, soCounts{};
    uint32_t soCount{};
    Id scissors{}, rasterizer{}, viewports{}, blend{}, depthState{}, dsv{}, predicate{};
    std::array<float, 4> blendFactor{};
    uint32_t sampleMask{}, stencilRef{};
    std::array<Id, 8> rtv{}, csUav{};
    uint32_t omStart{}, rtCount{}, csStart{}, csCount{}, predicateValue{};
    std::array<uint32_t, 8> omCounts{}, csCounts{};
    std::array<Id, 56> omExtended{}, csExtended{};
    std::array<uint32_t, 56> omExtendedCounts{}, csExtendedCounts{};
};
struct Event {
    Id id{}, state{}, context{};
    uint16_t type{};
    std::vector<uint32_t> args;
    Id argumentBuffer{};
};
class Frame {
    void *file_ = nullptr;
    void *mapping_ = nullptr;
    const uint8_t *data_ = nullptr;
    uint64_t size_ = 0;
    std::map<Id, Entry> entries_;
    std::filesystem::path path_;
    uint32_t width_{}, height_{};
    mutable std::once_flag hashOnce_;
    mutable std::string hash_;
    void close() noexcept;

  public:
    explicit Frame(const std::filesystem::path &path);
    ~Frame() { close(); }
    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
    const auto &entries() const { return entries_; }
    const auto &path() const { return path_; }
    uint64_t size() const { return size_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::string &sha256() const;
    const Entry &entry(Id id) const;
    Bytes payload(Id id, int category = -1, int type = -1) const;
    Bytes data(Id id) const;
    Bytes shader(Id id) const;
    Resource resource(Id id) const;
    Event event(Id id) const;
    State state(Id id) const;
    std::vector<std::pair<size_t, Bytes>> updates(Id id, size_t resourceSize) const;
};
bool isDraw(uint16_t type);
std::string sha256(Bytes bytes);
std::string commandName(uint16_t type);
std::string resourceName(uint16_t type);
TextureInfo textureInfo(const Resource &resource);
std::pair<uint32_t, uint32_t> pitches(uint32_t width, uint32_t height, uint32_t format);
} // namespace flora
