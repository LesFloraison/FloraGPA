#include "SetterEdits.h"
#include "ApiCommands.h"
#include "core/Contexts.h"
namespace flora {
namespace {
PredicateCommand command(const Frame &frame, Id event) {
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || !isEditableSetter(entry.type))
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
bool isEditableSetter(uint16_t type) { return predicateOperation(type) == PredicateOperation::Set; }
nlohmann::json capturedSetter(const Frame &frame, Id event) {
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
} // namespace flora
