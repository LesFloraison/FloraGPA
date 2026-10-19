#include "PipelineState.h"
#include "dx12/CapturedObjectMarker.h"
#include <QCryptographicHash>
#include <QByteArrayView>
#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string_view>

namespace flora {
namespace {
using Json = nlohmann::json;
Json real(float value) {
    if (std::isfinite(value))
        return double(value);
    return std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
}
template <typename E, size_t N> Json named(E value, const char *const (&names)[N]) {
    const auto index = uint32_t(value);
    return {{"enum", index}, {"name", index < N ? names[index] : "Unknown"}};
}
Json comparison(CompareFunction value) {
    return named(value,
                 {"Never", "Always", "Less", "LessEqual", "Greater", "GreaterEqual", "Equal", "NotEqual"});
}
Json stencil(const StencilFace &s) {
    const auto op = [](StencilOperation value) {
        return named(value, {"Keep", "Zero", "Replace", "IncrementSaturate", "DecrementSaturate",
                             "IncrementWrap", "DecrementWrap", "Invert"});
    };
    return {{"fail", op(s.failOperation)}, {"depth_fail", op(s.depthFailOperation)},
            {"pass", op(s.passOperation)}, {"function", comparison(s.function)},
            {"reference", s.reference},    {"compare_mask", s.compareMask},
            {"write_mask", s.writeMask}};
}
Json blendEquation(const BlendEquation &b) {
    const auto factor = [](BlendMultiplier value) {
        return named(value,
                     {"Zero", "One", "SourceColor", "InverseSourceColor", "DestinationColor",
                      "InverseDestinationColor", "SourceAlpha", "InverseSourceAlpha", "DestinationAlpha",
                      "InverseDestinationAlpha", "SourceAlphaSaturate", "BlendFactorRGB",
                      "InverseBlendFactorRGB", "BlendFactorAlpha", "InverseBlendFactorAlpha", "Source1Color",
                      "InverseSource1Color", "Source1Alpha", "InverseSource1Alpha"});
    };
    return {{"source", factor(b.source)},
            {"destination", factor(b.destination)},
            {"operation", named(b.operation, {"Add", "Subtract", "ReverseSubtract", "Minimum", "Maximum"})}};
}
std::string idText(ResourceId id) {
    uint64_t value{};
    static_assert(sizeof(id) == sizeof(value));
    memcpy(&value, &id, sizeof(value));
    return "ResourceId::" + std::to_string(value);
}
bool decimal(std::string_view text) {
    uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return value && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           std::to_string(value) == text;
}
const char *categoryName(DescriptorCategory category) {
    switch (category) {
    case DescriptorCategory::ConstantBlock:
        return "CBV";
    case DescriptorCategory::Sampler:
        return "Sampler";
    case DescriptorCategory::ReadOnlyResource:
        return "SRV";
    case DescriptorCategory::ReadWriteResource:
        return "UAV";
    default:
        return "Unknown";
    }
}
Json sampler(const SamplerDescriptor &s) {
    return {{"type_enum", uint32_t(s.type)},
            {"address_u_enum", uint32_t(s.addressU)},
            {"address_v_enum", uint32_t(s.addressV)},
            {"address_w_enum", uint32_t(s.addressW)},
            {"compare_enum", uint32_t(s.compareFunction)},
            {"min_lod", s.minLOD},
            {"max_lod", s.maxLOD},
            {"mip_bias", s.mipBias},
            {"max_anisotropy", s.maxAnisotropy},
            {"filter",
             {{"minify_enum", uint32_t(s.filter.minify)},
              {"magnify_enum", uint32_t(s.filter.magnify)},
              {"mip_enum", uint32_t(s.filter.mip)},
              {"function_enum", uint32_t(s.filter.filter)}}},
            {"border_color_type_enum", uint32_t(s.borderColorType)},
            {"srgb_border", s.srgbBorder},
            {"border_color_float",
             {s.borderColorValue.floatValue[0], s.borderColorValue.floatValue[1],
              s.borderColorValue.floatValue[2], s.borderColorValue.floatValue[3]}},
            {"border_color_uint",
             {s.borderColorValue.uintValue[0], s.borderColorValue.uintValue[1],
              s.borderColorValue.uintValue[2], s.borderColorValue.uintValue[3]}}};
}
} // namespace
Json readDx12Pipeline(IReplayController &controller) {
    const auto *state = controller.GetD3D12PipelineState();
    if (!state)
        throw std::runtime_error("DX12 pipeline state is unavailable");
    std::map<ResourceId, std::string> names;
    for (const auto &resource : controller.GetResources())
        names.emplace(resource.resourceId, resource.name.c_str());
    auto reference = [&](ResourceId id) -> Json {
        const auto it = names.find(id);
        const std::string name = it == names.end() ? std::string{} : it->second;
        Json result{{"rdc_id", idText(id)},
                    {"name", name},
                    {"resource_creation", nullptr},
                    {"resource_object", nullptr},
                    {"mapping_status", id == ResourceId() ? "unbound" : "unmapped"}};
        constexpr std::string_view prefix = "Flora DX12 resource ";
        if (name.starts_with(prefix)) {
            const std::string_view tail(name.data() + prefix.size(), name.size() - prefix.size());
            const auto split = tail.find(" creation ");
            if (split != std::string_view::npos && decimal(tail.substr(0, split)) &&
                decimal(tail.substr(split + 10))) {
                result["resource_object"] = std::string(tail.substr(0, split));
                result["resource_creation"] = std::string(tail.substr(split + 10));
                result["mapping_status"] = "capture_resource_marker";
            }
        }
        if (const auto marker = dx12::parseCapturedObjectMarker(name)) {
            result["mapping_status"] = "capture_object_marker";
            result["object_kind"] = marker->kind == dx12::CapturedObjectKind::Pipeline ? "pipeline" : "root_signature";
            result["captured_object"] = std::to_string(marker->object);
            result["object_creation"] = std::to_string(marker->creation);
        }
        return result;
    };
    auto descriptor = [&](const Descriptor &d) -> Json {
        return {{"resource", reference(d.resource)},
                {"secondary", reference(d.secondary)},
                {"view", reference(d.view)},
                {"type_enum", uint32_t(d.type)},
                {"category", categoryName(CategoryForDescriptorType(d.type))},
                {"flags", uint32_t(d.flags)},
                {"byte_offset", std::to_string(d.byteOffset)},
                {"byte_size", std::to_string(d.byteSize)},
                {"first_mip", d.firstMip},
                {"mip_count", d.numMips},
                {"first_slice", d.firstSlice},
                {"slice_count", d.numSlices},
                {"element_byte_size", d.elementByteSize},
                {"format",
                 {{"type_enum", uint32_t(d.format.type)},
                  {"component_type_enum", uint32_t(d.format.compType)},
                  {"component_count", d.format.compCount},
                  {"component_byte_width", d.format.compByteWidth},
                  {"srgb", d.format.SRGBCorrected()}}}};
    };
    Json rows = Json::array();
    auto add = [&](const std::string &section, const std::string &binding, Json resource, Json details) {
        rows.push_back({{"section", section},
                        {"binding", binding},
                        {"resource", std::move(resource)},
                        {"details", std::move(details)}});
    };
    const auto pipelineReference = reference(state->pipelineResourceId);
    add("Pipeline", "PSO", pipelineReference, Json::object());
    add("Pipeline", "Root signature", reference(state->rootSignature.resourceId), Json::object());
    struct Stage {
        const char *name;
        const D3D12Pipe::Shader *shader;
    };
    const Stage stages[]{{"VS", &state->vertexShader}, {"HS", &state->hullShader},
                         {"DS", &state->domainShader}, {"GS", &state->geometryShader},
                         {"PS", &state->pixelShader},  {"CS", &state->computeShader},
                         {"AS", &state->ampShader},    {"MS", &state->meshShader}};
    for (const auto &stage : stages) {
        if (stage.shader->resourceId == ResourceId())
            continue;
        const auto *reflection = stage.shader->reflection;
        auto shaderReference = reference(stage.shader->resourceId);
        if (pipelineReference.value("mapping_status", "") == "capture_object_marker" &&
            pipelineReference.at("object_kind") == "pipeline") {
            shaderReference["mapping_status"] = "capture_pipeline_stage";
            shaderReference["pipeline"] = pipelineReference;
            shaderReference["shader_stage"] = stage.name;
        }
        Json digest;
        if (reflection && !reflection->rawBytes.empty())
            digest = QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char *>(reflection->rawBytes.data()),
                               qsizetype(reflection->rawBytes.size())),
                QCryptographicHash::Sha256).toHex().toStdString();
        add(stage.name, "Shader", std::move(shaderReference),
            {{"reflection_available", reflection != nullptr},
             {"entry_point", reflection ? Json(reflection->entryPoint.c_str()) : Json(nullptr)},
             {"bytecode_sha256", digest},
             {"bytecode_bytes", reflection ? Json(std::to_string(reflection->rawBytes.size())) : Json(nullptr)},
             {"bytecode_scope", "Currently bound replay shader, including any experiment replacement; not a claim of original bytecode equality"}});
    }
    for (size_t i = 0; i < state->descriptorHeaps.size(); ++i)
        add("Heaps", std::to_string(i), reference(state->descriptorHeaps[i]), Json::object());
    for (size_t i = 0; i < state->rootSignature.parameters.size(); ++i) {
        const auto &p = state->rootSignature.parameters[i];
        Json ranges = Json::array(), words = Json::array();
        for (const auto &r : p.tableRanges)
            ranges.push_back({{"category", categoryName(r.category)},
                              {"space", r.space},
                              {"base_register", r.baseRegister},
                              {"count", r.count},
                              {"unbounded", r.count == UINT32_MAX},
                              {"table_byte_offset", r.tableByteOffset},
                              {"appended", r.appended}});
        for (size_t offset = 0; offset + 4 <= p.constants.size(); offset += 4) {
            uint32_t word{};
            memcpy(&word, p.constants.data() + offset, 4);
            words.push_back(word);
        }
        add("Root", std::to_string(i), reference(p.descriptor.resource),
            {{"visibility_mask", uint32_t(p.visibility)},
             {"space", p.space},
             {"register", p.reg},
             {"constants_u32", words},
             {"constant_bytes", p.constants.size()},
             {"descriptor", descriptor(p.descriptor)},
             {"heap", reference(p.heap)},
             {"heap_byte_offset", p.heapByteOffset},
             {"table_ranges", ranges}});
    }
    for (const auto &s : state->rootSignature.staticSamplers)
        add("Root", "Static sampler s" + std::to_string(s.reg) + " space" + std::to_string(s.space),
            reference(ResourceId()),
            {{"visibility_mask", uint32_t(s.visibility)}, {"sampler", sampler(s.descriptor)}});

    const auto &accesses = controller.GetDescriptorAccess();
    // Never expand entire descriptor tables (including UINT_MAX unbounded ranges).
    // Descriptor accesses describe potential shader bindings, not observed GPU memory accesses.
    constexpr size_t maxAccesses = 16384;
    if (accesses.size() > maxAccesses)
        throw std::runtime_error("Pipeline descriptor access limit exceeded (16384)");
    for (const auto &access : accesses) {
        const auto category = CategoryForDescriptorType(access.type);
        const Stage *stage = nullptr;
        for (const auto &candidate : stages)
            if (candidate.shader->stage == access.stage) {
                stage = &candidate;
                break;
            }
        Json binding = nullptr;
        if (stage && stage->shader->reflection && access.index != DescriptorAccess::NoShaderBinding) {
            const auto &r = *stage->shader->reflection;
            auto describe = [&](const auto &list) {
                if (access.index < list.size()) {
                    const auto &b = list[access.index];
                    binding = {{"name", b.name.c_str()},
                               {"register", b.fixedBindNumber},
                               {"space", b.fixedBindSetOrSpace},
                               {"array_size", b.bindArraySize}};
                }
            };
            switch (category) {
            case DescriptorCategory::ConstantBlock:
                describe(r.constantBlocks);
                break;
            case DescriptorCategory::Sampler:
                describe(r.samplers);
                break;
            case DescriptorCategory::ReadOnlyResource:
                describe(r.readOnlyResources);
                break;
            case DescriptorCategory::ReadWriteResource:
                describe(r.readWriteResources);
                break;
            default:
                break;
            }
        }
        rdcarray<DescriptorRange> ranges;
        ranges.push_back(DescriptorRange(access));
        Json detail{{"reflection_binding", binding},
                    {"array_element", access.arrayElement},
                    {"reflection_index", access.index},
                    {"stage_enum", uint32_t(access.stage)},
                    {"statically_unused", access.staticallyUnused},
                    {"evidence", "potential_shader_binding"},
                    {"store", reference(access.descriptorStore)},
                    {"store_byte_offset", access.byteOffset},
                    {"store_descriptor_size", access.byteSize}};
        Json resource = reference(ResourceId());
        if (category == DescriptorCategory::Sampler) {
            const auto values = controller.GetSamplerDescriptors(access.descriptorStore, ranges);
            if (values.size() != 1)
                throw std::runtime_error("Incomplete sampler descriptor response");
            detail["sampler"] = sampler(values[0]);
        } else {
            const auto values = controller.GetDescriptors(access.descriptorStore, ranges);
            if (values.size() != 1)
                throw std::runtime_error("Incomplete descriptor response");
            detail["descriptor"] = descriptor(values[0]);
            resource = reference(values[0].resource);
        }
        const std::string label = binding.is_null() ? "direct / unknown binding"
                                                    : binding.at("name").get<std::string>() + " reg" +
                                                          binding.at("register").dump() + " space" +
                                                          binding.at("space").dump();
        add(stage ? stage->name : "Unknown stage",
            std::string(categoryName(category)) + " " + label + " [" + std::to_string(access.arrayElement) +
                "]",
            std::move(resource), std::move(detail));
    }
    auto buffer = [&](const auto &b) -> Json {
        return {{"byte_offset", std::to_string(b.byteOffset)},
                {"byte_size", std::to_string(b.byteSize)},
                {"byte_stride", b.byteStride}};
    };
    add("IA", "Topology", reference(ResourceId()),
        {{"topology_enum", uint32_t(state->inputAssembly.topology)},
         {"index_strip_cut", state->inputAssembly.indexStripCutValue}});
    for (size_t i = 0; i < state->inputAssembly.vertexBuffers.size(); ++i) {
        const auto &b = state->inputAssembly.vertexBuffers[i];
        if (b.resourceId != ResourceId())
            add("IA", "VB " + std::to_string(i), reference(b.resourceId), buffer(b));
    }
    const auto &ib = state->inputAssembly.indexBuffer;
    add("IA", "Index buffer", reference(ib.resourceId), buffer(ib));
    for (size_t i = 0; i < state->outputMerger.renderTargets.size(); ++i) {
        const auto &rt = state->outputMerger.renderTargets[i];
        add("OM", "RTV " + std::to_string(i), reference(rt.resource), descriptor(rt));
    }
    add("OM", "DSV", reference(state->outputMerger.depthTarget.resource),
        {{"descriptor", descriptor(state->outputMerger.depthTarget)},
         {"depth_read_only", state->outputMerger.depthReadOnly},
         {"stencil_read_only", state->outputMerger.stencilReadOnly}});
    const bool graphics = state->computeShader.resourceId == ResourceId();
    if (graphics) {
        for (size_t i = 0; i < state->inputAssembly.layouts.size(); ++i) {
            const auto &l = state->inputAssembly.layouts[i];
            const auto resource = l.inputSlot < state->inputAssembly.vertexBuffers.size()
                                      ? state->inputAssembly.vertexBuffers[l.inputSlot].resourceId
                                      : ResourceId();
            add("IA",
                "Input " + std::to_string(i) + " " + l.semanticName.c_str() + std::to_string(l.semanticIndex),
                reference(resource),
                {{"semantic", l.semanticName.c_str()},
                 {"semantic_index", l.semanticIndex},
                 {"input_slot", l.inputSlot},
                 {"byte_offset", l.byteOffset},
                 {"append_aligned", l.byteOffset == D3D12Pipe::Layout::TightlyPacked},
                 {"per_instance", l.perInstance},
                 {"instance_step_rate", l.instanceDataStepRate},
                 {"format",
                  {{"type_enum", uint32_t(l.format.type)},
                   {"component_type_enum", uint32_t(l.format.compType)},
                   {"component_count", l.format.compCount},
                   {"component_byte_width", l.format.compByteWidth},
                   {"srgb", l.format.SRGBCorrected()}}}});
        }
        const auto &blend = state->outputMerger.blendState;
        add("OM", "Blend state", reference(ResourceId()),
            {{"alpha_to_coverage", blend.alphaToCoverage},
             {"independent_blend", blend.independentBlend},
             {"blend_factor",
              {real(blend.blendFactor[0]), real(blend.blendFactor[1]), real(blend.blendFactor[2]),
               real(blend.blendFactor[3])}}});
        for (size_t i = 0; i < blend.blends.size(); ++i) {
            const auto &b = blend.blends[i];
            add("OM", "Blend target " + std::to_string(i), reference(ResourceId()),
                {{"enabled", b.enabled},
                 {"color", blendEquation(b.colorBlend)},
                 {"alpha", blendEquation(b.alphaBlend)},
                 {"logic_enabled", b.logicOperationEnabled},
                 {"logic_operation_enum", uint32_t(b.logicOperation)},
                 {"write_mask", uint32_t(b.writeMask)},
                 {"applies_to", blend.independentBlend ? "same_index_target"
                                : i == 0               ? "all_targets"
                                                       : "ignored_independent_blend_disabled"}});
        }
        const auto &depth = state->outputMerger.depthStencilState;
        add("OM", "Depth / stencil", reference(ResourceId()),
            {{"depth_enabled", depth.depthEnable},
             {"depth_writes", depth.depthWrites},
             {"depth_function", comparison(depth.depthFunction)},
             {"bounds_enabled", depth.depthBoundsEnable},
             {"min_depth_bounds", real(depth.minDepthBounds)},
             {"max_depth_bounds", real(depth.maxDepthBounds)},
             {"stencil_enabled", depth.stencilEnable},
             {"front", stencil(depth.frontFace)},
             {"back", stencil(depth.backFace)}});
        const auto &raster = state->rasterizer.state;
        add("Rasterizer", "State", reference(ResourceId()),
            {{"fill_mode_enum", uint32_t(raster.fillMode)},
             {"cull_mode_enum", uint32_t(raster.cullMode)},
             {"front_counter_clockwise", raster.frontCCW},
             {"depth_bias", real(raster.depthBias)},
             {"depth_bias_clamp", real(raster.depthBiasClamp)},
             {"slope_scaled_depth_bias", real(raster.slopeScaledDepthBias)},
             {"depth_clip", raster.depthClip},
             {"line_raster_mode_enum", uint32_t(raster.lineRasterMode)},
             {"forced_sample_count", raster.forcedSampleCount},
             {"conservative_rasterization_enum", uint32_t(raster.conservativeRasterization)},
             {"sample_mask", state->rasterizer.sampleMask}});
        add("Rasterizer", "Shading rate", reference(raster.shadingRateImage),
            {{"base_rate", {raster.baseShadingRate.first, raster.baseShadingRate.second}},
             {"combiner_enums",
              {uint32_t(raster.shadingRateCombiners.first), uint32_t(raster.shadingRateCombiners.second)}}});
        for (size_t i = 0; i < state->rasterizer.viewports.size(); ++i) {
            const auto &v = state->rasterizer.viewports[i];
            add("Rasterizer", "Viewport " + std::to_string(i), reference(ResourceId()),
                {{"enabled", v.enabled},
                 {"x", real(v.x)},
                 {"y", real(v.y)},
                 {"width", real(v.width)},
                 {"height", real(v.height)},
                 {"min_depth", real(v.minDepth)},
                 {"max_depth", real(v.maxDepth)}});
        }
        for (size_t i = 0; i < state->rasterizer.scissors.size(); ++i) {
            const auto &s = state->rasterizer.scissors[i];
            add("Rasterizer", "Scissor " + std::to_string(i), reference(ResourceId()),
                {{"enabled", s.enabled}, {"x", s.x}, {"y", s.y}, {"width", s.width}, {"height", s.height}});
        }
        add("Stream output", "Rasterized stream", reference(ResourceId()),
            {{"enabled", state->streamOut.rasterizedStream != D3D12Pipe::StreamOut::NoRasterization},
             {"stream", state->streamOut.rasterizedStream}});
        for (size_t i = 0; i < state->streamOut.outputs.size(); ++i) {
            const auto &b = state->streamOut.outputs[i];
            add("Stream output", "Buffer " + std::to_string(i), reference(b.resourceId),
                {{"byte_offset", std::to_string(b.byteOffset)},
                 {"byte_size", std::to_string(b.byteSize)},
                 {"written_count_resource", reference(b.writtenCountResourceId)},
                 {"written_count_byte_offset", std::to_string(b.writtenCountByteOffset)}});
        }
    }
    add("Predication", "Predicate", reference(state->predication.resourceId),
        {{"byte_offset", std::to_string(state->predication.offset)},
         {"skip_if_zero", state->predication.skipIfZero}});
    return {{"pipeline_rows", rows},
            {"partial", true},
            {"pipeline_scope",
             {{"source", "RenderDoc 1.45 D3D12 pipeline at selected executed action"},
              {"descriptor_evidence", "potential shader bindings; not measured GPU accesses"},
              {"descriptor_access_limit", maxAccesses},
              {"descriptor_access_count", accesses.size()},
              {"graphics_state_applicable", graphics},
              {"shader_identity", "Stage ownership comes from the currently bound PSO, not a global shader-ID lookup; current bytecode digests require comparison with the capture or experiment"},
              {"gaps",
               {"Stream-output declarations and PSO sample description not exposed by this adapter",
                "Unaccessed descriptor table entries",
                "PSO/root-signature and stage ownership require capture object markers; older RDCs remain unmapped"}}}}};
}
} // namespace flora
