#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace flora::capture {
inline constexpr uint32_t default_resource_limit_mib = 1024;
inline constexpr uint32_t max_resource_limit_mib = 4096;
// Bounds recorded initial resources and command payloads, not process RSS or
// encoder copies. Individual resource/format limits remain independently checked.
inline size_t resource_limit_bytes(uint64_t mib) {
    if (!mib || mib > max_resource_limit_mib)
        throw std::invalid_argument("Resource limit must be 1..4096 MiB");
    return size_t(mib) * 1024 * 1024;
}
}
