#pragma once
#include "Frame.h"
#include <optional>
namespace flora {
struct PredicateDescriptor {
    Id id{}, original{}, device{};
    uint32_t type{}, flags{};
};
enum class PredicateOperation { Begin, End, Set };
struct PredicateBinding {
    Id resource{};
    uint32_t value{};
    bool operator==(const PredicateBinding &) const = default;
};
struct PredicateCommand {
    PredicateOperation operation;
    Id context{}, resource{};
    uint32_t value{};
};
PredicateDescriptor readPredicate(const Frame &frame, Id id);
// GPA's 0x96 family also stores BOOL stream queries created with CreateQuery.
// Those resources support Begin/End/GetData, never SetPredication.
bool isStreamOverflowQuery(uint32_t type);
std::optional<PredicateOperation> predicateOperation(uint16_t type);
PredicateCommand readPredicateCommand(uint16_t type, Bytes payload);
} // namespace flora
