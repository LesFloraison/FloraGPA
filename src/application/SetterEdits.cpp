#include "SetterEdits.h"
#include "ApiCommands.h"
#include "OutputEdits.h"
#include "core/Contexts.h"
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
    return predicateOperation(type) == PredicateOperation::Set || samplerSetterStage(type).has_value() ||
           srvSetterStage(type).has_value() || isSrvOutputCommand(type);
}
nlohmann::json capturedSetter(const Frame &frame, Id event) {
    const auto &entry = frame.entry(event);
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
PredicateBinding validatePredicateSetter(const Frame &frame, Id event, const nlohmann::json &values) {
    command(frame, event);
    if (!values.is_object() || values.size() != 2 || !values.contains("predicate") ||
        !values.contains("predicate_value"))
        throw std::runtime_error("Provide all and only the selected setter arguments");
    PredicateBinding result{integer(values["predicate"], UINT64_MAX),
                            uint32_t(integer(values["predicate_value"], UINT32_MAX))};
    if (result.resource)
        readPredicate(frame, result.resource);
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
