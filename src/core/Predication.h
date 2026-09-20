#pragma once
#include "Frame.h"
#include <optional>
namespace flora {
struct PredicateDescriptor {
    Id id{}, original{}, device{};
    uint32_t type{}, flags{};
};
enum class PredicateOperation { Begin, End, Set };
struct PredicateCommand {
    PredicateOperation operation;
    Id context{}, resource{};
    uint32_t value{};
};
PredicateDescriptor readPredicate(const Frame &frame, Id id);
std::optional<PredicateOperation> predicateOperation(uint16_t type);
PredicateCommand readPredicateCommand(uint16_t type, Bytes payload);
} // namespace flora
