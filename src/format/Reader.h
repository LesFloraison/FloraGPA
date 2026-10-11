#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
namespace flora::format {
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
}
