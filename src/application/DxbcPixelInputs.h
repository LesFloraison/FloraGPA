#pragma once
#include "DxbcExecution.h"

namespace flora::dxbc_exec {
struct PixelInputBinding {
    std::string semantic;
    uint32_t index, system, type, reg, mask, used, precision;
};
// Validated original SM4/SM5 input signature, preserving hardware register,
// component placement, used mask, system value and minimum precision.
std::vector<PixelInputBinding> pixelShaderInputSignature(Bytes original);
struct PixelInputRecordSelection {
    uint32_t x = 0, y = 0, sample = 0, layer = 0, maxRecords = 32;
    bool layerInput = false;
};
struct PixelAttributeEvaluation {
    // DXBC eval_snapped, eval_sample_index or eval_centroid. Snapped offsets
    // use the signed low four bits; sample indices are checked against raster samples.
    uint32_t opcode = 205;
    std::array<uint32_t, 2> location{};
};
struct PixelInputShader {
    std::vector<uint8_t> bytecode;
    std::string originalShaderSha256;
    uint32_t reg = 0, mask = 0;
    bool sampleFrequency = false;
    // Implicit sample-frequency shaders retain their original trigger. A
    // private integer RTV associates records with storage samples without
    // adding SV_SampleIndex to a shader that did not declare it.
    bool sampleRecordTarget = false;
    // When present, a private u1 raw buffer stores records, with an optional
    // integer sample-index RTV. The header retains the allocation count,
    // selected record index and an opaque frequency-trigger bit union.
    // Records start at byte 16 and span 144 bytes.
    // The private observation shader never needs a free application UAV slot.
    std::optional<PixelInputRecordSelection> records;
    std::optional<PixelAttributeEvaluation> evaluation;
    uint32_t attributeConstantMask = 0;
    // Compact signature consumed by a private pre-raster bridge. D3D11 links
    // hardware registers as well as semantics: do not bind this shader against
    // the unmodified original producer. Rasterizer-generated values also occur
    // here; they do not require matching producer attributes.
    std::vector<PixelInputBinding> inputSignature;
    // UINT MRTs 0..3 hold the four quad lanes. MRT 4 contains pixel/role,
    // 5 sample indices, 6 coverage and 7 raw orientation diagnostics.
    // Only mask components are observations; other components are padding.
    Value decode(const std::array<uint32_t, 4> &) const;
};
// Replaces the body with an input observation using MRTs or private records. Compacts only the
// selected register, retaining its original signature, interpolation and
// minimum precision. This does not establish original fragment eligibility,
// run original side effects, or preserve original depth/discard behavior.
// The generated SM5 helper requires a feature-level-11 device.
PixelInputShader pixelInputShader(Bytes original, uint32_t inputRegister,
                                  std::optional<PixelInputRecordSelection> records = {},
                                  std::optional<PixelAttributeEvaluation> evaluation = {});
// Observes quad coverage and selection without inventing a source register.
// reg is UINT32_MAX and mask is zero; ordinary input declarations are not copied.
PixelInputShader pixelCoverageShader(Bytes original, const PixelInputRecordSelection &records);
} // namespace flora::dxbc_exec
