#include "NativePixelCandidates.h"
#include "HlslCompilation.h"
#include <algorithm>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <set>

namespace flora {
using Json = nlohmann::json;
namespace {
constexpr const char *ordinalSemantic = "FLORA_CANDIDATE_ORDINAL";
uint32_t ordinalRegister(Bytes code) {
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "Reflect candidate observation");
    D3D11_SHADER_DESC desc{};
    check(reflection->GetDesc(&desc), "Describe candidate observation");
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC field{};
        check(reflection->GetInputParameterDesc(i, &field), "Read candidate signature");
        if (!_stricmp(field.SemanticName, ordinalSemantic) && field.Mask == 3)
            return field.Register;
    }
    throw std::runtime_error("Candidate ordinal signature is missing");
}
} // namespace
NativePixelCandidates captureNativePixelCandidates(ID3D11Device *device, bool warp,
                                                   const dxbc_exec::PixelRasterState &state,
                                                   const PostTransformGeometry &geometry,
                                                   const NativePixelCandidateSelection &selection,
                                                   const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const auto &meta = geometry.report;
    const auto primitiveIds = validatedPixelPrimitiveIds(geometry);
    const auto instances = validatedPixelGeometryInstances(geometry);
    const auto stride = meta.at("stride").get<uint32_t>();
    const auto factor = meta.at("vertices_per_primitive").get<uint32_t>();
    const auto count = meta.at("primitives").get<uint64_t>();
    const auto first = selection.firstPrimitive, selected = selection.primitiveCount;
    if (!device || !stride || stride > UINT32_MAX - 8 || factor < 1 || factor > 3 || !selected ||
        first >= count || selected > count - first || count > UINT64_MAX / factor ||
        meta.at("vertices") != count * factor || count * factor > geometry.bytes.size() / stride ||
        geometry.bytes.size() != count * factor * stride ||
        meta.value("geometry_stage", std::string()) != "final_pre_rasterization" ||
        meta.value("statistics_only", false) || selected > UINT32_MAX / factor || !selection.maxRecords ||
        selection.maxRecords > 4096 || !selection.timeoutMs || selection.timeoutMs > 60000)
        throw std::invalid_argument("Invalid native pixel candidate geometry or selection");
    // Account for temporary tagged geometry and the largest possible bridge.
    const uint64_t vertices = selected * factor;
    const uint64_t temporaryStride = uint64_t(stride) + 8 + 512;
    if (vertices > selection.maxBytes / temporaryStride || vertices > SIZE_MAX / temporaryStride)
        throw std::runtime_error("Native pixel candidate geometry budget exceeded");
    const auto temporaryBytes = vertices * temporaryStride;
    bool layerOutput = false;
    for (const auto &attribute : meta.at("attributes")) {
        const auto offset = attribute.at("offset").get<uint32_t>();
        const auto components = attribute.at("component_count").get<uint32_t>();
        if (offset > stride || components > (stride - offset) / 4)
            throw std::runtime_error("Native pixel candidate source attribute exceeds original stride");
        if (!_stricmp(attribute.at("semantic").get_ref<const std::string &>().c_str(), ordinalSemantic))
            throw std::runtime_error("Native pixel candidate semantic collision");
        layerOutput |= attribute.at("system_value") == 4;
    }
    PostTransformGeometry tagged;
    tagged.report = meta;
    // Tagged bytes are a different geometry object; original IDs stay in the
    // validated sidecar and are joined after reading candidate ordinals.
    tagged.report.erase("primitive_identity");
    tagged.report.erase("geometry_instances");
    tagged.report["stride"] = stride + 8;
    tagged.report["vertices"] = vertices;
    tagged.report["primitives"] = selected;
    tagged.report["attributes"].push_back({{"semantic", ordinalSemantic},
                                           {"index", 0},
                                           {"system_value", 0},
                                           {"component_type", 1},
                                           {"offset", stride},
                                           {"component_count", 2},
                                           {"components", {0, 1}}});
    tagged.bytes.resize(size_t(vertices) * (stride + 8));
    for (uint64_t i = 0; i < vertices; ++i) {
        if (i % 1024 == 0)
            checkCancellation(cancelled);
        auto *target = tagged.bytes.data() + size_t(i) * (stride + 8);
        std::memcpy(target, geometry.bytes.data() + size_t(first * factor + i) * stride, stride);
        const uint64_t ordinal = first + i / factor;
        const std::array<uint32_t, 2> words{uint32_t(ordinal), uint32_t(ordinal >> 32)};
        std::memcpy(target + stride, words.data(), 8);
    }
    const auto code =
        compileHlsl(
            "uint2 main(nointerpolation uint2 ordinal:FLORA_CANDIDATE_ORDINAL):SV_Target{return ordinal;}",
            "ps_5_0", "main", "native-pixel-candidate.hlsl")
            .bytecode;
    const auto pixel = dxbc_exec::pixelInputShader(
        code, ordinalRegister(code),
        dxbc_exec::PixelInputRecordSelection{selection.x, selection.y, selection.sample, selection.layer,
                                             selection.maxRecords, layerOutput});
    auto bridge = dxbc_exec::pixelGeometryRangeBridge(tagged, pixel, 0, selected);
    auto observations = dxbc_exec::capturePixelRasterInputs(device, warp, state, bridge, pixel,
                                                            selection.maxBytes - temporaryBytes, cancelled,
                                                            selection.timeoutMs);
    auto rows = Json::array();
    std::set<uint64_t> unique;
    for (size_t index = 0; index < observations.records.size(); ++index) {
        const auto &words = observations.records[index].words;
        const auto lane = words[4][2] + 2 * words[4][3];
        const auto ordinal = uint64_t(words[lane][0]) | uint64_t(words[lane][1]) << 32;
        if (ordinal < first || ordinal - first >= selected)
            throw std::runtime_error("Native pixel candidate ordinal is outside the selected range");
        unique.insert(ordinal);
        const auto instance = instances.instance(ordinal);
        rows.push_back({{"primitive_ordinal", ordinal},
                        {"observation_index", index},
                        {"selected_lane", lane},
                        {"quad_coverage", words[6]},
                        {"original_primitive_id",
                         primitiveIds.empty() ? Json(nullptr) : Json(primitiveIds[size_t(ordinal)])},
                        {"instance", nullptr},
                        {"geometry_instance", instance ? Json(*instance) : Json(nullptr)},
                        {"original_fragment_eligibility", nullptr}});
    }
    NativePixelCandidates result;
    result.report = {{"schema", 1},
                     {"kind", "native_pixel_candidates"},
                     {"geometry_sha256", sha256(geometry.bytes, cancelled)},
                     {"geometry_metadata", meta},
                     {"backend", "native-pixel-candidates/1"},
                     {"scope", "captured_geometry_pre_depth_coverage"},
                     {"first_primitive", first},
                     {"primitive_count", selected},
                     {"x", selection.x},
                     {"y", selection.y},
                     {"sample", selection.sample},
                     {"sample_selection_domain", "raster_coverage"},
                     {"layer", selection.layer},
                     {"observations", rows},
                     {"unique_primitive_ordinals", unique},
                     {"raster_passes", 1},
                     {"temporary_geometry_bytes_upper_bound", temporaryBytes},
                     {"original_fragment_eligibility", nullptr},
                     {"original_pixel_shader_executed", false},
                     {"original_depth_stencil_executed", false},
                     {"original_predicate_executed", false},
                     {"original_resources_accessed", false},
                     {"order", "unspecified"},
                     {"capture", observations.identity}};
    result.observations = std::move(observations);
    return result;
}
} // namespace flora
