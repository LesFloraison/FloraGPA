#include "ReplayMesh.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace flora {
namespace {
uint64_t number(const nlohmann::json &value, uint64_t maximum) {
    if (!(value.is_number_unsigned() || value.is_number_integer()) || value < 0 || value > maximum)
        throw std::runtime_error("Invalid replay mesh integer");
    return value.get<uint64_t>();
}
} // namespace
std::string replayMeshFloat(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    char buffer[64];
    const auto converted =
        std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    if (converted.ec != std::errc{})
        throw std::runtime_error("Cannot format replay mesh value");
    std::string repr(buffer, converted.ptr);
    const auto e = repr.find('e');
    const int exponent = std::stoi(repr.substr(e + 1));
    if (exponent < -4 || exponent >= 16)
        return repr;
    const bool negative = repr[0] == '-';
    auto digits = repr.substr(negative ? 1 : 0, e - (negative ? 1 : 0));
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    const int point = exponent + 1;
    std::string result = negative ? "-" : "";
    if (point <= 0)
        return result + "0." + std::string(size_t(-point), '0') + digits;
    if (size_t(point) >= digits.size())
        return result + digits + std::string(size_t(point) - digits.size(), '0') + ".0";
    return result + digits.substr(0, point) + "." + digits.substr(point);
}
ReplayMesh ReplayMesh::decode(const nlohmann::json &m, std::span<const uint8_t> raw,
                              std::span<const uint8_t> ib) {
    ReplayMesh result;
    const auto &format = m.at("format");
    if (format.at("compType") != 1 || format.at("compByteWidth") != 4)
        throw std::runtime_error("Post-shader position is not float32; binary export is available");
    result.components = uint32_t(number(format.at("compCount"), 4));
    const auto stride = number(m.at("vertexByteStride"), UINT32_MAX);
    if (!stride)
        throw std::runtime_error("Post-shader vertex stride is zero");
    const auto count = raw.size() / stride;
    result.positions.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i) {
        const auto offset = i * stride;
        if (result.components * sizeof(float) > raw.size() - offset)
            throw std::runtime_error("Post-shader position exceeds vertex storage");
        std::array<double, 4> position{};
        for (uint32_t c = 0; c < result.components; ++c) {
            float value{};
            std::memcpy(&value, raw.data() + offset + c * sizeof(float), sizeof(float));
            position[c] = value;
        }
        result.positions.push_back(position);
    }
    result.indexCount = number(m.at("numIndices"), UINT32_MAX);
    const bool indexed = m.at("indexResourceId") != "ResourceId::0";
    const auto width = number(m.at("indexByteStride"), UINT32_MAX);
    const auto &base = m.at("baseVertex");
    if (!base.is_number_integer() || base < INT32_MIN || base > INT32_MAX)
        throw std::runtime_error("Invalid replay mesh base vertex");
    const auto baseVertex = base.get<int64_t>();
    if (indexed && (width != 1 && width != 2 && width != 4))
        throw std::runtime_error("Unsupported replay mesh index width");
    if (indexed && ib.size() != result.indexCount * width)
        throw std::runtime_error("Post-shader index storage is truncated or oversized");
    auto index = [&](uint64_t i) -> int64_t {
        if (!indexed)
            return int64_t(i);
        uint32_t value{};
        std::memcpy(&value, ib.data() + i * width, size_t(width));
        return int64_t(value) + baseVertex;
    };
    auto face = [&](int64_t a, int64_t b, int64_t c) {
        ++result.candidateFaceCount;
        if (a >= 0 && b >= 0 && c >= 0 && uint64_t(a) < count && uint64_t(b) < count && uint64_t(c) < count)
            result.faces.push_back({uint64_t(a), uint64_t(b), uint64_t(c)});
    };
    const auto topology = number(m.at("topology"), UINT32_MAX);
    if (topology == 5) {
        for (uint64_t i = 0; i + 2 < result.indexCount; i += 3)
            face(index(i), index(i + 1), index(i + 2));
    } else if (topology == 6) {
        for (uint64_t i = 0; i + 2 < result.indexCount; ++i) {
            auto a = index(i), b = index(i + 1), c = index(i + 2);
            if (i % 2)
                std::swap(a, b);
            if (a != b && a != c && b != c)
                face(a, b, c);
        }
    }
    return result;
}
std::string ReplayMesh::csv() const {
    std::string out = "vertex";
    for (uint32_t c = 0; c < components; ++c) {
        out += ',';
        out += "xyzw"[c];
    }
    out += "\r\n";
    for (size_t i = 0; i < positions.size(); ++i) {
        out += std::to_string(i);
        for (uint32_t c = 0; c < components; ++c)
            out += ',' + replayMeshFloat(positions[i][c]);
        out += "\r\n";
    }
    return out;
}
std::string ReplayMesh::obj() const {
    std::string out = "# Post-shader positions. Perspective divide applied when w is available.\r\n";
    for (const auto &p : positions) {
        const auto w = components > 3 && p[3] != 0 ? p[3] : 1;
        out += "v ";
        for (uint32_t c = 0; c < (std::min)(components, 3U); ++c) {
            if (c)
                out += ' ';
            out += replayMeshFloat(p[c] / w);
        }
        out += "\r\n";
    }
    for (const auto &f : faces)
        out += "f " + std::to_string(f[0] + 1) + ' ' + std::to_string(f[1] + 1) + ' ' +
               std::to_string(f[2] + 1) + "\r\n";
    return out;
}
} // namespace flora
