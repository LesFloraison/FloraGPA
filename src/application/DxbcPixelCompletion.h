#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
struct PixelCompletionShader {
    std::vector<uint8_t> bytecode;
    nlohmann::json identity;
};
// Retains the original PS executable and emits a constant RGB marker before
// returns. Alpha, depth, coverage, resources and input declarations are retained.
// The selected existing target must be private, unblended and RGB-writable.
// A surviving marker proves neither invocation order nor original resource writes.
PixelCompletionShader instrumentPixelCompletion(Bytes original, uint32_t target,
                                                std::array<uint32_t, 3> markerBits,
                                                uint64_t maxBytes = 256ull * 1024 * 1024,
                                                const CancelCheck &cancelled = {});
} // namespace flora
