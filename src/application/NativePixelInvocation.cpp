#include "NativePixelInvocation.h"
#include "HlslCompilation.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <chrono>
#include <set>
namespace flora {
namespace {
struct PixelAttributeContext {
    Com<ID3D11Device> device;
    bool warp;
    dxbc_exec::PixelRasterState raster;
    std::vector<uint8_t> shader;
    dxbc_exec::PixelInputRecordSelection selection;
    NativePixelInvocationLimits limits;
    CancelCheck bounded;
    std::map<uint32_t, dxbc_exec::PixelGeometryBridge> bridges;
    std::map<uint32_t, std::array<dxbc_exec::Value, 4>> original;
    dxbc_exec::PixelInputRecord anchor;
    nlohmann::json identity;
    std::shared_ptr<nlohmann::json> observations = std::make_shared<nlohmann::json>(nlohmann::json::array());
    uint64_t retained = 0;
    std::map<dxbc_exec::AttributeRequest, std::array<dxbc_exec::Value, 4>> cache;

    std::array<dxbc_exec::Value, 4> evaluate(dxbc_exec::AttributeRequest request) {
        using namespace dxbc_exec;
        checkCancellation(bounded);
        if (request.opcode == 203)
            for (auto &value : request.location)
                value &= 15;
        if (const auto found = cache.find(request); found != cache.end())
            return found->second;
        const auto found = bridges.find(request.inputRegister);
        if (found == bridges.end())
            throw std::runtime_error("Pixel attribute register has no original geometry bridge");
        const auto pixel =
            pixelInputShader(shader, request.inputRegister, selection,
                             PixelAttributeEvaluation{request.opcode, request.location, request.precision});
        const auto samples = raster.rasterizer.ForcedSampleCount ? raster.rasterizer.ForcedSampleCount
                                                                 : raster.target.samples.Count;
        std::array<Value, 4> values{};
        nlohmann::json observation;
        auto observe = [&](const PixelInputShader &helper) {
            if (retained >= limits.maxBytes)
                throw std::runtime_error("Pixel attribute observation budget exceeded");
            auto observed = capturePixelRasterInputs(device.Get(), warp, raster, found->second, helper,
                                                     limits.maxBytes - retained, bounded, limits.timeoutMs);
            if (observed.records.size() != 1)
                throw std::runtime_error("Pixel attribute observation is not unique");
            for (unsigned i = 4; i < 9; ++i)
                if (observed.records[0].words[i] != anchor.words[i])
                    throw std::runtime_error("Pixel attribute observation changed quad identity");
            for (const auto *key : {"adapter_luid", "adapter_vendor", "adapter_device", "feature_level",
                                    "warp", "x", "y", "sample", "layer", "raster_samples", "target_samples",
                                    "raster_state", "shader_sha256", "sample_frequency"})
                if (observed.identity.at(key) != identity.at(key))
                    throw std::runtime_error("Pixel attribute observation changed original raster identity");
            for (unsigned lane = 0; lane < 4; ++lane)
                values[lane] = helper.decode(observed.records[0].words[lane]);
            return observed.identity;
        };
        if (request.opcode == 204 && request.location[0] >= samples &&
            pixel.attributeConstantMask != pixel.mask) {
            values = original.at(request.inputRegister);
            observation = {{"backend", "undefined_attribute_sample_index"},
                           {"raster_samples", samples},
                           {"private_draws", 0}};
            if (pixel.attributeConstantMask && (request.precision[0] || request.precision[1])) {
                // Observe the narrowed constant components, without executing an
                // undefined sample request for the interpolated components.
                const auto constants = pixelInputShader(shader, request.inputRegister, selection,
                                                        PixelAttributeEvaluation{205, {}, request.precision});
                auto evidence = observe(constants);
                observation["private_draws"] = evidence.at("private_draws");
                observation["constant_observation"] = std::move(evidence);
            }
            for (unsigned lane = 0; lane < 4; ++lane) {
                values[lane].valid &= uint8_t(pixel.attributeConstantMask);
                for (unsigned c = 0; c < 4; ++c)
                    if (!(values[lane].valid & (1u << c)))
                        values[lane].bits[c] = 0;
            }
        } else {
            observation = observe(pixel);
        }
        observation["request"] = {{"opcode", request.opcode},
                                  {"input_register", request.inputRegister},
                                  {"location", request.location},
                                  {"precision", request.precision}};
        observation["quad_values"] = nlohmann::json::array();
        for (const auto &value : values)
            observation["quad_values"].push_back({{"bits", value.bits}, {"valid_mask", value.valid}});
        const auto bytes = observation.dump().size() + sizeof(request) + sizeof(values);
        if (bytes > limits.maxBytes - retained)
            throw std::runtime_error("Pixel attribute cache budget exceeded");
        checkCancellation(bounded);
        retained += bytes;
        observations->push_back(std::move(observation));
        cache.emplace(request, values);
        return values;
    }
};
} // namespace
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
    const CancelCheck bounded = [cancelled, deadline] {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native pixel invocation deadline");
        return false;
    };
    checkCancellation(bounded);
    const Program program(shader);
    if (program.stage() != 0)
        throw std::invalid_argument("Native pixel invocation requires a pixel shader");
    std::shared_ptr<PixelAttributeContext> attributes;
    if (std::any_of(
            program.instructions().begin(), program.instructions().end(),
            [](const auto &instruction) { return instruction.opcode >= 203 && instruction.opcode <= 205; })) {
        attributes = std::make_shared<PixelAttributeContext>();
        attributes->device = device;
        attributes->warp = warp;
        attributes->raster = raster;
        attributes->shader.assign(shader.begin(), shader.end());
        attributes->limits = limits;
        attributes->bounded = bounded;
    }
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
        auto bridge = pixelGeometryBridge(geometry, pixel, ordinal);
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
        if (attributes) {
            const auto bridgeBytes = bridge.vertices.size() + bridge.vertexShader.size() +
                                     bridge.geometryShader.size() + bridge.identity.dump().size();
            if (bridgeBytes > limits.maxBytes - retained)
                throw std::runtime_error("Pixel attribute bridge budget exceeded");
            retained += bridgeBytes;
            attributes->retained += bridgeBytes;
            attributes->bridges.emplace(reg, std::move(bridge));
            if (observed.records.size() == 1)
                for (unsigned lane = 0; lane < 4; ++lane)
                    attributes->original[reg][lane] = pixel.decode(observed.records[0].words[lane]);
        }
        passes.emplace(reg, std::move(observed));
    }
    auto result = pixelTraceInputs(shader, passes);
    if (attributes) {
        attributes->selection = selection;
        attributes->anchor = passes.begin()->second.records.front();
        attributes->identity = passes.begin()->second.identity;
        attributes->retained += shader.size() + attributes->identity.dump().size() +
                                attributes->original.size() * (sizeof(uint32_t) + 4 * sizeof(Value));
        if (attributes->retained >= limits.maxBytes)
            throw std::runtime_error("Pixel attribute context budget exceeded");
        result.attributeRetainedBytes = attributes->retained;
        result.attributeObservations = attributes->observations;
        result.evaluateAttribute = [attributes](const AttributeRequest &request) {
            return attributes->evaluate(request);
        };
    }
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
