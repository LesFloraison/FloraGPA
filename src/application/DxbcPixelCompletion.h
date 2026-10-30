#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
#include <optional>
namespace flora {
struct PixelCompletionShader {
    std::vector<uint8_t> bytecode;
    nlohmann::json identity;
};
// Single-sample outputs only: defer coverage bit zero to a return-time discard.
// Retaining an all-one oMask export preserves suppression of alpha-to-coverage.
PixelCompletionShader normalizeSingleSamplePixelCoverage(Bytes original,
                                                         uint64_t maxBytes = 256ull * 1024 * 1024,
                                                         const CancelCheck &cancelled = {});
// Add an otherwise unused RGB output to a depth/coverage-only PS. The caller
// must instrument its returns and bind a private diagnostic RTV before use.
// Original depth, coverage and executable instructions are retained.
PixelCompletionShader addPixelCompletionTarget(Bytes original, uint32_t target,
                                               uint64_t maxBytes = 256ull * 1024 * 1024,
                                               const CancelCheck &cancelled = {});
// Retains the original PS executable and emits a constant RGB marker before
// returns. Alpha, depth, coverage, resources and input declarations are retained.
// The selected existing target must be private, unblended and RGB-writable.
// A surviving marker proves neither invocation order nor original resource writes.
PixelCompletionShader instrumentPixelCompletion(Bytes original, uint32_t target,
                                                std::array<uint32_t, 3> markerBits,
                                                uint64_t maxBytes = 256ull * 1024 * 1024,
                                                const CancelCheck &cancelled = {});
struct PixelPrimitiveMask {
    uint32_t mask = UINT32_MAX, value = 0;
    bool notEqual = false;
};
struct PixelPrimitiveInput {
    uint32_t reg, component;
};
// Returning fragments write the marker when their actual SV_PrimitiveID matches
// the masked comparison, otherwise zero RGB. notEqual complements the entire
// comparison, not a selected bit. Both paths retain alpha and all original work.
// The caller must establish a valid primitive-ID producer (rasterizer or GS).
PixelCompletionShader instrumentPixelPrimitiveMask(Bytes original, uint32_t target,
                                                   PixelPrimitiveMask selection,
                                                   uint64_t maxBytes = 256ull * 1024 * 1024,
                                                   const CancelCheck &cancelled = {},
                                                   std::optional<PixelPrimitiveInput> producer = {});
// The same predicate executable with a distinct transport contract: clear the
// private blendable target, MAX RGB and replace alpha. Requires a float output.
PixelCompletionShader instrumentPixelPrimitivePresence(Bytes original, uint32_t target,
                                                       PixelPrimitiveMask selection,
                                                       uint64_t maxBytes = 256ull * 1024 * 1024,
                                                       const CancelCheck &cancelled = {},
                                                       std::optional<PixelPrimitiveInput> producer = {});
} // namespace flora
