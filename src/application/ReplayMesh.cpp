#include "ReplayMesh.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace flora {
namespace {

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

std::string ReplayMesh::csv() const {
    componentOffsets();
    std::string out = "vertex";
    for (uint32_t c = 0; c < components; ++c) {
        out += ',';
        out += "xyzw"[componentChannels[c]];
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
    const auto offsets = componentOffsets();
    if (!canExportObj())
        throw std::runtime_error("OBJ requires X, Y and Z for sparse position outputs; export CSV or JSON");
    std::string out = "# Post-shader positions. Perspective divide applied when w is available.\r\n";
    for (const auto &p : positions) {
        const auto w = offsets[3] >= 0 && p[offsets[3]] != 0 ? p[offsets[3]] : 1;
        out += "v ";
        for (uint32_t c = 0; c < (std::min)(components, 3U); ++c) {
            if (c)
                out += ' ';
            out += replayMeshFloat(p[offsets[c]] / w);
        }
        out += "\r\n";
    }
    for (const auto &f : faces)
        out += "f " + std::to_string(f[0] + 1) + ' ' + std::to_string(f[1] + 1) + ' ' +
               std::to_string(f[2] + 1) + "\r\n";
    return out;
}
std::array<int, 4> ReplayMesh::componentOffsets() const {
    if (components > 4)
        throw std::runtime_error("Invalid position component count");
    std::array<int, 4> offsets{-1, -1, -1, -1};
    for (uint32_t i = 0; i < components; ++i) {
        const auto channel = componentChannels[i];
        if (channel > 3 || offsets[channel] >= 0)
            throw std::runtime_error("Invalid or duplicate position channel");
        offsets[channel] = int(i);
    }
    return offsets;
}
bool ReplayMesh::canExportObj() const {
    const auto offsets = componentOffsets();
    if (offsets[0] >= 0 && offsets[1] >= 0 && offsets[2] >= 0)
        return true;
    // Preserve existing contiguous-prefix exports, including short positions.
    for (uint32_t i = 0; i < components; ++i)
        if (componentChannels[i] != i)
            return false;
    return true;
}
std::optional<std::array<double, 3>>
ReplayMesh::projectedPosition(const std::array<double, 4> &position) const {
    const auto offsets = componentOffsets();
    if (offsets[0] < 0 || offsets[1] < 0 || offsets[2] < 0)
        return std::nullopt;
    const auto w = offsets[3] >= 0 && position[offsets[3]] != 0 ? position[offsets[3]] : 1;
    std::array<double, 3> result{};
    for (unsigned c = 0; c < 3; ++c) {
        result[c] = position[offsets[c]] / w;
        if (!std::isfinite(result[c]))
            return std::nullopt;
    }
    return result;
}
} // namespace flora
