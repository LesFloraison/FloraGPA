#include "Dxbc.h"
#include <bit>
namespace flora {
DxbcParts readDxbcParts(Bytes bytes) {
    Reader r(bytes);
    if (r.read<uint32_t>() != 0x43425844)
        throw std::runtime_error("Not DXBC");
    r.skip(16);
    if (r.read<uint32_t>() != 1 || r.read<uint32_t>() != bytes.size())
        throw std::runtime_error("Invalid DXBC container bounds");
    auto count = r.read<uint32_t>();
    if (count > 256)
        throw std::runtime_error("DXBC chunk count exceeds limit");
    DxbcParts out;
    for (uint32_t i = 0; i < count; ++i) {
        auto offset = r.read<uint32_t>();
        if (offset < 32 + count * 4 || offset > bytes.size())
            throw std::runtime_error("Invalid DXBC chunk offset");
        Reader part(bytes.subspan(offset));
        auto tag = part.read<uint32_t>(), size = part.read<uint32_t>();
        out.emplace_back(tag, part.take(size));
    }
    return out;
}
static void append(std::vector<uint8_t> &out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(value >> (i * 8)));
}
// Retail DXBC uses MD5 compression with container-specific length padding.
static std::array<uint32_t, 4> checksum(Bytes data) {
    constexpr uint32_t constants[]{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    constexpr int shifts[][4]{{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    auto left = data.size() % 64, whole = data.size() - left;
    std::vector<uint8_t> padded(data.begin(), data.begin() + whole);
    if (left < 56)
        append(padded, uint32_t(data.size() * 8));
    padded.insert(padded.end(), data.begin() + whole, data.end());
    padded.push_back(128);
    if (left >= 56) {
        padded.resize(whole + 64);
        append(padded, uint32_t(data.size() * 8));
        padded.resize(whole + 124);
    } else
        padded.resize(whole + 60);
    append(padded, uint32_t(data.size() * 2) | 1);
    std::array<uint32_t, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    Reader r(padded);
    while (r.remaining()) {
        auto words = r.array<uint32_t, 16>();
        auto [a, b, c, d] = state;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t f, g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = 7 * i % 16;
            }
            auto next = b + std::rotl(a + f + constants[i] + words[g], shifts[i / 16][i % 4]);
            a = d;
            d = c;
            c = b;
            b = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
    return state;
}
std::vector<uint8_t> addEmptyInputSignature(Bytes bytes) {
    auto parts = readDxbcParts(bytes);
    for (const auto &[tag, body] : parts)
        if (tag == 0x4e475349 || tag == 0x52444853 || tag == 0x58454853)
            return {bytes.begin(), bytes.end()};
    const std::array<uint8_t, 8> empty{0, 0, 0, 0, 8, 0, 0, 0};
    parts.emplace_back(0x4e475349, empty);
    std::vector<uint8_t> out(32 + parts.size() * 4);
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(out.data() + offset, &value, 4); };
    put(0, 0x43425844);
    put(20, 1);
    put(28, uint32_t(parts.size()));
    for (size_t i = 0; i < parts.size(); ++i) {
        put(32 + i * 4, uint32_t(out.size()));
        append(out, parts[i].first);
        append(out, uint32_t(parts[i].second.size()));
        out.insert(out.end(), parts[i].second.begin(), parts[i].second.end());
    }
    if (out.size() > UINT32_MAX)
        throw std::runtime_error("DXBC container exceeds uint32 size");
    put(24, uint32_t(out.size()));
    auto hash = checksum(Bytes(out).subspan(20));
    std::memcpy(out.data() + 4, hash.data(), 16);
    return out;
}
} // namespace flora
