#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace flora {
// DXGI R10G10B10A2_UNORM storage is retained in captures. This conversion is
// only for the 8-bit display/export boundary; it does not tone-map HDR content.
constexpr std::array<std::uint8_t, 4> unpack_rgb10a2(std::uint32_t pixel) noexcept {
    return {std::uint8_t(((pixel & 1023u) * 255u + 511u) / 1023u),
            std::uint8_t((((pixel >> 10) & 1023u) * 255u + 511u) / 1023u),
            std::uint8_t((((pixel >> 20) & 1023u) * 255u + 511u) / 1023u), std::uint8_t((pixel >> 30) * 85u)};
}
// R11G11B10_FLOAT contains unsigned 5-bit exponents (bias 15) and 6/6/5
// mantissa bits. Keep the decoded HDR values distinct from display conversion.
inline std::array<float, 3> unpack_r11g11b10(std::uint32_t pixel) noexcept {
    const auto component = [](std::uint32_t bits, unsigned mantissaBits) {
        const auto exponent = bits >> mantissaBits;
        const auto fraction = bits & ((1U << mantissaBits) - 1U);
        if (exponent == 31)
            return fraction ? std::numeric_limits<float>::quiet_NaN()
                            : std::numeric_limits<float>::infinity();
        return exponent ? std::ldexp(float((1U << mantissaBits) + fraction),
                                     int(exponent) - 15 - int(mantissaBits))
                        : std::ldexp(float(fraction), -14 - int(mantissaBits));
    };
    return {component(pixel & 2047U, 6), component((pixel >> 11) & 2047U, 6), component(pixel >> 22, 5)};
}
inline std::array<std::uint8_t, 4> display_r11g11b10(std::uint32_t pixel) noexcept {
    const auto values = unpack_r11g11b10(pixel);
    std::array<std::uint8_t, 4> result{0, 0, 0, 255};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float value = values[i];
        // Display-only linear clamp, no exposure or transfer function. NaN is
        // black, positive infinity/HDR overflow is white; storage stays intact.
        result[i] = !(value > 0) ? 0 : value >= 1 ? 255 : std::uint8_t(std::lround(value * 255.0f));
    }
    return result;
}
} // namespace flora
