#pragma once
#include "Frame.h"
namespace flora {
struct ClassRecord {
    Id id{}, linkage{}, namesData{};
    bool instance{};
    // D3D11_CLASS_INSTANCE_DESC fields in captured order.
    std::array<uint32_t, 8> desc{};
    std::string instanceName, typeName;
};
struct ClassIdentityAudit {
    std::map<Id, Id> aliases;
    std::map<Id, std::string> errors;
};
ClassIdentityAudit auditClassIdentities(const Frame &);
Id canonicalClassLinkage(const ClassIdentityAudit &, Id);
ClassRecord readClassRecord(const Frame &frame, Id id);
Id shaderClassLinkage(const Frame &frame, Id shader);
} // namespace flora
