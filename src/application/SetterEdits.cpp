#include "SetterEdits.h"
#include "core/NormalizedPredication.h"
#include "ApiCommands.h"
#include "OutputEdits.h"
#include "RasterizerEdits.h"
#include "core/Contexts.h"
#include <cmath>
namespace flora {
namespace {
PredicateCommand command(const Frame &frame, Id event) {
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || predicateOperation(entry.type) != PredicateOperation::Set)
        throw std::runtime_error("Select a supported state setter");
    if (inspectCommand(frame, event).at("status") != "decoded")
        throw std::runtime_error("Setter requires a complete recovered command layout");
    auto result = readPredicateCommand(entry.type, frame.payload(event));
    requireImmediateContext(frame, result.context);
    return result;
}
uint64_t integer(const nlohmann::json &value, uint64_t max) {
    if ((!value.is_number_unsigned() && (!value.is_number_integer() || value.get<int64_t>() < 0)) ||
        value.get<uint64_t>() > max)
        throw std::runtime_error("Setter integer is outside its unsigned range");
    return value.get<uint64_t>();
}
} // namespace
bool isEditableSetter(uint16_t type) {
    return constantBufferStage(type).has_value() || isIaSetter(type) ||
           predicateOperation(type) == PredicateOperation::Set || samplerSetterStage(type).has_value() ||
           srvSetterStage(type).has_value() || isSrvOutputCommand(type) || isPipelineSetter(type);
}
nlohmann::json capturedSetter(const Frame &frame, Id event) {
    const auto &entry = frame.entry(event);
    if (entry.category == 7 && constantBufferStage(entry.type)) {
        if (inspectCommand(frame, event).at("status") != "decoded")
            throw std::runtime_error("Setter requires a complete recovered command layout");
        auto binding = readConstantBufferSetter(entry.type, frame.payload(event));
        requireImmediateContext(frame, binding.context);
        return constantBufferSetterValues(binding);
    }
    if (entry.category == 7 && isIaSetter(entry.type)) {
        if (inspectCommand(frame, event).at("status") != "decoded")
            throw std::runtime_error("Setter requires a complete recovered command layout");
        auto binding = readIaSetter(entry.type, frame.payload(event));
        requireImmediateContext(frame, binding.context);
        return iaSetterValues(binding);
    }
    if (entry.category == 7 && isPipelineSetter(entry.type)) {
        if (inspectCommand(frame, event).at("status") != "decoded")
            throw std::runtime_error("Setter requires a complete recovered command layout");
        auto binding = readPipelineSetter(entry.type, frame.payload(event));
        requireImmediateContext(frame, binding.context);
        return pipelineSetterValues(binding);
    }
    if (entry.category == 7 && isSrvOutputCommand(entry.type))
        return capturedOutputSetter(frame, event);
    if (entry.category == 7 && samplerSetterStage(entry.type)) {
        if (inspectCommand(frame, event).at("status") != "decoded")
            throw std::runtime_error("Setter requires a complete recovered command layout");
        const auto captured = readSamplerCommand(frame.payload(event));
        requireImmediateContext(frame, captured.context);
        return {{"start_slot", captured.binding.start}, {"samplers", captured.binding.resources}};
    }
    if (entry.category == 7 && srvSetterStage(entry.type)) {
        const auto captured = readSrvCommand(frame.payload(event));
        requireImmediateContext(frame, captured.context);
        return {{"start_slot", captured.binding.start}, {"views", captured.binding.views}};
    }
    auto captured = command(frame, event);
    return {{"predicate", captured.resource}, {"predicate_value", captured.value}};
}
nlohmann::json constantBufferSetterValues(const ConstantBufferBinding &b) {
    nlohmann::json values{{"start_slot", b.start}, {"buffers", b.buffers}};
    if (b.type >= 0x24f && b.type <= 0x254) {
        values["first_constants"] = b.first ? nlohmann::json(*b.first) : nlohmann::json(nullptr);
        values["constant_counts"] = b.counts ? nlohmann::json(*b.counts) : nlohmann::json(nullptr);
    }
    return values;
}
ConstantBufferBinding validateConstantBufferSetter(const Frame &frame, Id event,
                                                   const nlohmann::json &values) {
    auto type = frame.entry(event).type;
    if (!constantBufferStage(type))
        throw std::runtime_error("Select a constant-buffer setter");
    auto original = capturedSetter(frame, event);
    if (!values.is_object() || values.size() != original.size())
        throw std::runtime_error("Provide all and only the selected setter arguments");
    for (const auto &[key, value] : original.items())
        if (!values.contains(key))
            throw std::runtime_error("Missing constant-buffer setter argument");
    auto b = readConstantBufferSetter(type, frame.payload(event));
    b.start = uint32_t(integer(values.at("start_slot"), 13));
    auto array = [&]<class T>(const char *key, uint64_t maximum) {
        const auto &a = values.at(key);
        if (!a.is_array() || a.size() > 14 - b.start)
            throw std::runtime_error("CB array exceeds slot range");
        std::vector<T> result;
        for (const auto &v : a)
            result.push_back(T(integer(v, maximum)));
        return result;
    };
    b.buffers = array.operator()<Id>("buffers", UINT64_MAX);
    if (type >= 0x24f && type <= 0x254) {
        b.first = values.at("first_constants").is_null()
                      ? std::nullopt
                      : std::optional(array.operator()<uint32_t>("first_constants", UINT32_MAX));
        b.counts = values.at("constant_counts").is_null()
                       ? std::nullopt
                       : std::optional(array.operator()<uint32_t>("constant_counts", 4096));
    }
    validateConstantBufferBinding(frame, b);
    return b;
}
nlohmann::json iaSetterValues(const IaBinding &b) {
    if (b.type == 0x34ef)
        return {{"input_layout", b.resource}};
    if (b.type == 0x34f1)
        return {{"ib", b.resource}, {"ib_format", b.format}, {"ib_offset", b.offset}};
    return {{"start_slot", b.start}, {"buffers", b.buffers}, {"strides", b.strides}, {"offsets", b.offsets}};
}
IaBinding validateIaSetter(const Frame &frame, Id event, const nlohmann::json &values) {
    auto type = frame.entry(event).type;
    if (!isIaSetter(type))
        throw std::runtime_error("Select an IA setter");
    auto original = capturedSetter(frame, event);
    if (!values.is_object() || values.size() != original.size())
        throw std::runtime_error("Provide all and only the selected setter arguments");
    for (auto &[key, value] : original.items())
        if (!values.contains(key))
            throw std::runtime_error("Missing IA setter argument");
    auto b = readIaSetter(type, frame.payload(event));
    if (type == 0x34ef)
        b.resource = integer(values.at("input_layout"), UINT64_MAX);
    else if (type == 0x34f1) {
        b.resource = integer(values.at("ib"), UINT64_MAX);
        b.format = uint32_t(integer(values.at("ib_format"), UINT32_MAX));
        b.offset = uint32_t(integer(values.at("ib_offset"), UINT32_MAX));
    } else {
        b.start = uint32_t(integer(values.at("start_slot"), 31));
        auto array = [&]<class T>(const char *key, std::vector<T> &target, uint64_t maximum) {
            const auto &a = values.at(key);
            if (!a.is_array() || a.size() > 32 - b.start)
                throw std::runtime_error("IA array exceeds slot range");
            target.clear();
            for (const auto &v : a)
                target.push_back(T(integer(v, maximum)));
        };
        array("buffers", b.buffers, UINT64_MAX);
        array("strides", b.strides, UINT32_MAX);
        array("offsets", b.offsets, UINT32_MAX);
    }
    validateIaBinding(frame, b);
    return b;
}
nlohmann::json pipelineSetterValues(const PipelineBinding &binding) {
    const auto &s = binding.values;
    if (auto stage = shaderSetterStage(binding.type)) {
        const auto &v = s.stages[*stage];
        if (v.classCount > v.classes.size())
            throw std::runtime_error("Shader class count exceeds 256");
        return {{"shader", v.shader},
                {"class_instances", std::vector<Id>(v.classes.begin(), v.classes.begin() + v.classCount)}};
    }
    switch (binding.type - 0x34de) {
    case 24:
        return {{"topology", s.topology}};
    case 35:
        return {{"blend", s.blend}, {"blend_factor", s.blendFactor}, {"sample_mask", s.sampleMask}};
    case 36:
        return {{"depth_state", s.depthState}, {"stencil_ref", s.stencilRef}};
    case 43:
        return {{"rasterizer", s.rasterizer}};
    case 44:
        return {{"viewports_values", *s.viewportValues}};
    case 45:
        return {{"scissors_values", *s.scissorValues}};
    default:
        throw std::runtime_error("Not a pipeline setter");
    }
}
PipelineBinding validatePipelineSetter(const Frame &frame, Id event, const nlohmann::json &values) {
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || !isPipelineSetter(entry.type))
        throw std::runtime_error("Select a pipeline setter");
    const auto original = capturedSetter(frame, event);
    if (!values.is_object() || values.size() != original.size())
        throw std::runtime_error("Provide all and only the selected setter arguments");
    for (auto it = original.begin(); it != original.end(); ++it)
        if (!values.contains(it.key()))
            throw std::runtime_error("Missing setter argument");
    auto result = readPipelineSetter(entry.type, frame.payload(event));
    auto &s = result.values;
    if (auto stage = shaderSetterStage(entry.type)) {
        auto &v = s.stages[*stage];
        v.shader = integer(values.at("shader"), UINT64_MAX);
        const auto &classes = values.at("class_instances");
        if (!classes.is_array() || classes.size() > v.classes.size())
            throw std::runtime_error("Provide at most 256 class instances");
        v.classes.fill(0);
        v.classCount = uint32_t(classes.size());
        for (size_t i = 0; i < classes.size(); ++i)
            v.classes[i] = integer(classes[i], UINT64_MAX);
    }
    switch (entry.type - 0x34de) {
    case 24:
        s.topology = uint32_t(integer(values.at("topology"), 64));
        break;
    case 35:
        s.blend = integer(values.at("blend"), UINT64_MAX);
        s.sampleMask = uint32_t(integer(values.at("sample_mask"), UINT32_MAX));
        if (!values.at("blend_factor").is_array() || values.at("blend_factor").size() != 4)
            throw std::runtime_error("Blend factor requires four floats");
        for (size_t i = 0; i < 4; ++i) {
            const auto &value = values.at("blend_factor")[i];
            if (!value.is_number() || !std::isfinite(value.get<float>()))
                throw std::runtime_error("Blend factor requires finite float32");
            s.blendFactor[i] = value.get<float>();
        }
        break;
    case 36:
        s.depthState = integer(values.at("depth_state"), UINT64_MAX);
        s.stencilRef = uint32_t(integer(values.at("stencil_ref"), UINT32_MAX));
        break;
    case 43:
        s.rasterizer = integer(values.at("rasterizer"), UINT64_MAX);
        break;
    case 44:
        s.viewportValues = normalizePipeline({{"viewports", values.at("viewports_values")}})
                               .at("viewports")
                               .get<std::vector<std::array<float, 6>>>();
        break;
    case 45:
        s.scissorValues = normalizePipeline({{"scissors", values.at("scissors_values")}})
                              .at("scissors")
                              .get<std::vector<std::array<int32_t, 4>>>();
        break;
    }
    validatePipelineBinding(frame, result);
    return result;
}
PredicateBinding validatePredicateSetter(const Frame &frame, Id event, const nlohmann::json &values) {
    command(frame, event);
    if (!values.is_object() || values.size() != 2 || !values.contains("predicate") ||
        !values.contains("predicate_value"))
        throw std::runtime_error("Provide all and only the selected setter arguments");
    PredicateBinding result{integer(values["predicate"], UINT64_MAX),
                            uint32_t(integer(values["predicate_value"], UINT32_MAX))};
    if (result.resource) {
        const auto proofs = !frame.entries().contains(result.resource)
                                ? auditNormalizedPredication(frame)
                                : std::map<Id, NormalizedPredicateProof>{};
        const auto proof = proofs.find(event);
        if (proof == proofs.end() || proof->second.resource != result.resource)
            if (isStreamOverflowQuery(readPredicate(frame, result.resource).type))
                throw std::runtime_error("Stream overflow query cannot be bound as a predicate");
    }
    return result;
}
SamplerBinding validateSamplerSetter(const Frame &frame, Id event, const nlohmann::json &values) {
    if (frame.entry(event).category != 7 || !samplerSetterStage(frame.entry(event).type))
        throw std::runtime_error("Select a sampler setter");
    capturedSetter(frame, event);
    if (!values.is_object() || values.size() != 2 || !values.contains("start_slot") ||
        !values.contains("samplers") || !values.at("samplers").is_array())
        throw std::runtime_error("Provide start_slot and a samplers array");
    SamplerBinding result;
    result.start = uint32_t(integer(values.at("start_slot"), 15));
    if (values.at("samplers").size() > 16 - result.start)
        throw std::runtime_error("Sampler range exceeds 16 slots");
    for (const auto &value : values.at("samplers")) {
        auto id = integer(value, UINT64_MAX);
        validateSamplerResource(frame, id);
        result.resources.push_back(id);
    }
    return result;
}
SrvBinding validateSrvSetter(const Frame &frame, Id event, const nlohmann::json &values) {
    if (frame.entry(event).category != 7 || !srvSetterStage(frame.entry(event).type))
        throw std::runtime_error("Select a shader-resource view setter");
    capturedSetter(frame, event);
    if (!values.is_object() || values.size() != 2 || !values.contains("start_slot") ||
        !values.contains("views") || !values.at("views").is_array())
        throw std::runtime_error("Provide start_slot and a views array");
    SrvBinding result;
    result.start = uint32_t(integer(values.at("start_slot"), 127));
    if (values.at("views").size() > 128 - result.start)
        throw std::runtime_error("SRV range exceeds 128 slots");
    for (const auto &value : values.at("views")) {
        auto id = integer(value, UINT64_MAX);
        validateSrvResource(frame, id);
        result.views.push_back(id);
    }
    return result;
}
} // namespace flora
