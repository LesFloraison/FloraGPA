#pragma once
#include "ClassLinkage.h"
namespace flora {
struct ClassCreationRecord {
    uint16_t type{};
    Id link{}, owner{}, resource{}, canonicalOwner{}, canonicalResource{};
    int32_t result{};
    std::array<uint32_t, 4> arguments{};
    ClassRecord saved;
    std::string error, note;
};
struct ClassCreationAudit {
    ClassIdentityAudit identities;
    std::map<Id, ClassCreationRecord> records;
    std::map<Id, Id> creationEvents;
};
bool isClassCreation(uint16_t);
ClassCreationRecord readClassCreation(uint16_t, Bytes);
ClassCreationAudit auditClassCreations(const Frame &, const CancelCheck &cancelled = {});
const ClassCreationRecord &requireClassCreation(const ClassCreationAudit &, Id);
} // namespace flora
