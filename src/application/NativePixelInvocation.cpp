#include "NativePixelInvocation.h"
#include "HlslCompilation.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <chrono>
#include <set>
namespace flora {
nlohmann::json captureNativePixelSampleDepth(ID3D11Device *device, bool warp,
                                             const dxbc_exec::PixelRasterState &raster,
                                             const PostTransformGeometry &geometry, uint64_t ordinal,
                                             dxbc_exec::PixelInputRecordSelection selection,
                                             const NativePixelInvocationLimits &limits,
                                             const CancelCheck &cancelled) {
    using namespace dxbc_exec;
    if (!device || !raster.target.depth || !limits.maxBytes || !limits.timeoutMs || limits.timeoutMs > 60000)
        throw std::invalid_argument("Invalid native sample-depth observation");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs);
    const CancelCheck bounded = [&] {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native sample-depth deadline");
        return false;
    };
    checkCancellation(bounded);
    const auto shader = compileHlsl("float4 main(sample noperspective float4 position:SV_Position,"
                                    "uint index:SV_SampleIndex):SV_Target{return position+float(index);}",
                                    "ps_5_0")
                            .bytecode;
    const auto fields = pixelShaderInputSignature(shader);
    const auto position =
        std::find_if(fields.begin(), fields.end(), [](const auto &f) { return f.system == 1; });
    if (position == fields.end())
        throw std::runtime_error("Native sample-depth probe has no position input");
    selection.maxRecords = 2;
    selection.layerInput =
        std::any_of(geometry.report.at("attributes").begin(), geometry.report.at("attributes").end(),
                    [](const auto &a) { return a.at("system_value") == 4; });
    const auto pixel = pixelInputShader(shader, position->reg, selection);
    if (!pixel.sampleFrequency)
        throw std::runtime_error("Native sample-depth probe lost sample interpolation");
    const auto bridge = pixelGeometryBridge(geometry, pixel, ordinal);
    const uint64_t sourceBytes = geometry.bytes.size() + geometry.primitiveIds.size() * sizeof(uint32_t) +
                                 shader.size() + bridge.vertices.size();
    if (sourceBytes >= limits.maxBytes)
        throw std::runtime_error("Native sample-depth source budget exceeded");
    auto observed = capturePixelRasterInputs(device, warp, raster, bridge, pixel,
                                             limits.maxBytes - sourceBytes, bounded, limits.timeoutMs);
    if (observed.records.size() != 1)
        throw std::runtime_error("Native sample-depth observation is not unique");
    const auto &record = observed.records.front().words;
    const auto lane = record[4][2] + 2 * record[4][3];
    if (lane >= 4 || record[8][1] != selection.sample || !(record[6][lane] & (1u << selection.sample)))
        throw std::runtime_error("Native sample-depth selection differs");
    checkCancellation(bounded);
    return {{"schema", 1},
            {"kind", "native_pixel_sample_depth"},
            {"backend", "native-pixel-sample-depth/1"},
            {"scope", "isolated_primitive_sample_position"},
            {"position_interpolation", "linear_noperspective_sample"},
            {"original_fragment_eligibility", nullptr},
            {"geometry_sha256", sha256(geometry.bytes, bounded)},
            {"primitive_ordinal", ordinal},
            {"sample", selection.sample},
            {"depth_bits", record[8][2]},
            {"position_bits", record[lane]},
            {"front_face", record[8][3] != 0},
            {"coverage_mask", record[6][lane]},
            {"observation", std::move(observed.identity)}};
}
dxbc_exec::PixelTraceOptions
captureNativePixelInvocationInputs(ID3D11Device *device, bool warp, const dxbc_exec::PixelRasterState &raster,
                                   const PostTransformGeometry &geometry, Bytes shader, uint64_t ordinal,
                                   dxbc_exec::PixelInputRecordSelection selection,
                                   const NativePixelInvocationLimits &limits, const CancelCheck &cancelled) {
    using namespace dxbc_exec;
    if (!device || !limits.maxBytes || !limits.timeoutMs || limits.timeoutMs > 60000)
        throw std::invalid_argument("Invalid native pixel invocation limits");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs);
    const CancelCheck bounded = [&] {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native pixel invocation deadline");
        return false;
    };
    checkCancellation(bounded);
    const Program program(shader);
    if (program.stage() != 0)
        throw std::invalid_argument("Native pixel invocation requires a pixel shader");
    std::set<uint32_t> registers;
    for (const auto &instruction : program.instructions())
        if (instruction.opcode == 95 || (instruction.opcode >= 98 && instruction.opcode <= 100)) {
            const auto &row = instruction.words;
            if (row.size() < 2)
                throw std::runtime_error("Truncated native pixel input declaration");
            const auto kind = (row[1] >> 12) & 255;
            if (kind == 35 && instruction.opcode == 95)
                continue;
            if (kind != 1 || instruction.opcode == 95)
                throw std::runtime_error("Unsupported special native pixel input");
            const auto index = (row[1] & 0x80000000u) ? 3u : 2u;
            if (row.size() <= index || row[index] >= 32)
                throw std::runtime_error("Invalid native pixel input register");
            registers.insert(row[index]);
        }
    if (registers.empty())
        registers.insert(UINT32_MAX);
    // Two records distinguish a unique quad from an ambiguous rasterization;
    // no arbitrary first invocation is selected after an overflow or duplicate.
    selection.maxRecords = 2;
    selection.layerInput =
        std::any_of(geometry.report.at("attributes").begin(), geometry.report.at("attributes").end(),
                    [](const auto &a) { return a.at("system_value") == 4; });
    uint64_t retained = geometry.bytes.size() + geometry.primitiveIds.size() * sizeof(uint32_t);
    std::map<uint32_t, PixelRasterInputs> passes;
    for (auto reg : registers) {
        checkCancellation(bounded);
        if (retained >= limits.maxBytes)
            throw std::runtime_error("Native pixel invocation payload budget exceeded");
        const auto pixel = reg == UINT32_MAX ? pixelCoverageShader(shader, selection)
                                             : pixelInputShader(shader, reg, selection);
        const auto bridge = pixelGeometryBridge(geometry, pixel, ordinal);
        if (bridge.vertices.size() > (limits.maxBytes - retained) / 2)
            throw std::runtime_error("Native pixel invocation bridge budget exceeded");
        auto observed = capturePixelRasterInputs(device, warp, raster, bridge, pixel,
                                                 limits.maxBytes - retained - bridge.vertices.size(), bounded,
                                                 limits.timeoutMs);
        const uint64_t bytes = observed.storage.size() + observed.records.size() * sizeof(PixelInputRecord) +
                               observed.identity.dump().size();
        if (bytes > limits.maxBytes - retained)
            throw std::runtime_error("Native pixel invocation observation budget exceeded");
        retained += bytes;
        passes.emplace(reg, std::move(observed));
    }
    auto result = pixelTraceInputs(shader, passes);
    checkCancellation(bounded);
    const auto ids = validatedPixelPrimitiveIds(geometry);
    result.inputProvenance["geometry_sha256"] = sha256(geometry.bytes, bounded);
    result.inputProvenance["primitive_ordinal"] = ordinal;
    result.inputProvenance["original_primitive_id"] =
        ids.empty() ? nlohmann::json(nullptr) : nlohmann::json(ids[size_t(ordinal)]);
    result.inputProvenance["original_fragment_eligibility"] = nullptr;
    return result;
}
} // namespace flora
