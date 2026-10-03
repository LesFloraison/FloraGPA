#pragma once
#include "Predication.h"
namespace flora {
struct PredicateCreationRecord {
    Id link{}, device{}, resource{};
    int32_t result{};
    bool hasDescriptor{};
    uint32_t type{}, flags{};
    std::string error;
};
struct PredicateCreationAudit {
    std::map<Id, PredicateCreationRecord> records;
    std::map<Id, Id> creationEvents;
};
PredicateCreationRecord readPredicateCreation(Bytes);
PredicateCreationAudit auditPredicateCreations(const Frame &);
const PredicateCreationRecord &requirePredicateCreation(const PredicateCreationAudit &, Id);
} // namespace flora
