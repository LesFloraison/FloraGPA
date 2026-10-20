#include "InputAttribute.h"
#include <qfloat16.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace flora {
namespace {
void require(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }
uint32_t word(const uint8_t *data, uint32_t bytes) {
    uint32_t value{}; memcpy(&value, data, bytes); return value;
}
int32_t signedWord(uint32_t value, uint32_t bytes) {
    if (bytes == 1) return int8_t(value);
    if (bytes == 2) return int16_t(value);
    return std::bit_cast<int32_t>(value);
}
float miniFloat(uint32_t value, uint32_t fractionBits) {
    const auto exponent = value >> fractionBits, fraction = value & ((1U << fractionBits) - 1);
    if (exponent == 31) return fraction ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    return exponent ? std::ldexp(float((1U << fractionBits) + fraction), int(exponent) - 15 - int(fractionBits))
                    : std::ldexp(float(fraction), -14 - int(fractionBits));
}
}
InputAttributeFormat inputAttributeFormat(const ResourceFormat &f) {
    require(!f.SRGBCorrected(), "sRGB input attributes are unsupported");
    const auto output = f.compType == CompType::UInt || f.compType == CompType::SInt ? f.compType : CompType::Float;
    if (f.type == ResourceFormatType::R10G10B10A2) {
        require(f.compCount == 4 && (f.compType == CompType::UNorm || f.compType == CompType::UInt),
                "DX12 R10G10B10A2 input requires UNorm or UInt components");
        return {4, output};
    }
    if (f.type == ResourceFormatType::R11G11B10) {
        require(f.compCount == 3 && f.compType == CompType::Float, "DX12 R11G11B10 input requires float components");
        return {4, CompType::Float};
    }
    require(f.type == ResourceFormatType::Regular && f.compCount >= 1 && f.compCount <= 4 &&
            (f.compByteWidth == 1 || f.compByteWidth == 2 || f.compByteWidth == 4),
            "Unsupported input attribute storage format");
    require((f.compType == CompType::Float && f.compByteWidth >= 2) || f.compType == CompType::UInt ||
            f.compType == CompType::SInt || f.compType == CompType::UNorm || f.compType == CompType::SNorm,
            "Unsupported input attribute component interpretation");
    return {uint32_t(f.compCount) * f.compByteWidth, output};
}
uint32_t appendInputAttributeOffset(uint32_t previousEnd, const ResourceFormat &f) {
    const auto bytes = inputAttributeFormat(f).bytes;
    const uint32_t alignment = bytes == 1 ? 1 : bytes == 2 ? 2 : 4;
    require(previousEnd <= UINT32_MAX - (alignment - 1), "Append-aligned input offset overflows");
    return (previousEnd + alignment - 1) & ~(alignment - 1);
}
std::array<uint32_t, 4> inputAttributeValue(std::span<const uint8_t> data, const ResourceFormat &f) {
    const auto format = inputAttributeFormat(f);
    require(data.size() >= format.bytes, "Truncated input attribute");
    std::array<uint32_t, 4> out{0, 0, 0, format.outputType == CompType::Float ? std::bit_cast<uint32_t>(1.0f) : 1U};
    if (f.type == ResourceFormatType::R10G10B10A2) {
        const auto packed = word(data.data(), 4);
        out = {packed & 1023U, (packed >> 10) & 1023U, (packed >> 20) & 1023U, packed >> 30};
        if (f.compType == CompType::UNorm)
            for (uint32_t c = 0; c < 4; ++c) out[c] = std::bit_cast<uint32_t>(float(double(out[c]) / (c == 3 ? 3.0 : 1023.0)));
    } else if (f.type == ResourceFormatType::R11G11B10) {
        const auto packed = word(data.data(), 4);
        out[0] = std::bit_cast<uint32_t>(miniFloat(packed & 2047, 6));
        out[1] = std::bit_cast<uint32_t>(miniFloat((packed >> 11) & 2047, 6));
        out[2] = std::bit_cast<uint32_t>(miniFloat(packed >> 22, 5));
    } else {
        for (uint32_t c = 0; c < f.compCount; ++c) {
            const auto source = data.data() + c * f.compByteWidth;
            const auto u = word(source, f.compByteWidth);
            if (f.compType == CompType::UInt) { out[c] = u; continue; }
            const auto s = signedWord(u, f.compByteWidth);
            if (f.compType == CompType::SInt) { out[c] = std::bit_cast<uint32_t>(s); continue; }
            float value{};
            if (f.compType == CompType::Float) {
                if (f.compByteWidth == 2) { qfloat16 half; memcpy(&half, source, 2); value = float(half); }
                else { out[c] = u; continue; } // Preserve float32 NaN payloads and signed zero.
            } else if (f.compType == CompType::UNorm)
                value = float(double(u) / double(f.compByteWidth == 4 ? UINT32_MAX : (1U << (f.compByteWidth * 8)) - 1));
            else
                value = std::max(-1.0f, float(double(s) / double(f.compByteWidth == 4 ? INT32_MAX : (1U << (f.compByteWidth * 8 - 1)) - 1)));
            out[c] = std::bit_cast<uint32_t>(value);
        }
    }
    if (f.BGRAOrder()) std::swap(out[0], out[2]);
    return out;
}
}
