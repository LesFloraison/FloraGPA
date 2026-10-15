#pragma once
#include <array>
#include <cstdint>

namespace flora {
// DXGI R10G10B10A2_UNORM storage is retained in captures. This conversion is
// only for the 8-bit display/export boundary; it does not tone-map HDR content.
constexpr std::array<std::uint8_t,4> unpack_rgb10a2(std::uint32_t pixel) noexcept {
    return {std::uint8_t(((pixel&1023u)*255u+511u)/1023u),
        std::uint8_t((((pixel>>10)&1023u)*255u+511u)/1023u),
        std::uint8_t((((pixel>>20)&1023u)*255u+511u)/1023u),
        std::uint8_t((pixel>>30)*85u)};
}
}
