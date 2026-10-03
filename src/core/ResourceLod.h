#pragma once
#include "Frame.h"
#include <set>
namespace flora {
struct ResourceLodRecord {
    Id link{}, context{}, resource{};
    float value{};
    bool setter{};
};
bool isResourceLodRecord(uint16_t type);
bool hasResourceLodClamp(const Resource &resource);
ResourceLodRecord readResourceLod(uint16_t type, Bytes payload);
void validateResourceLod(const Frame &frame, const ResourceLodRecord &record);
struct ResourceLodAudit {
    struct Initial {
        float value{};
        Id observation{};
    };
    struct Issue {
        Id event{}, resource{};
        std::string reason;
    };
    std::map<Id, ResourceLodRecord> records;
    std::set<Id> clamped;
    std::map<Id, Initial> initial;
    std::map<Id, Id> creations;
    std::vector<Issue> issues;
};
ResourceLodAudit auditResourceLod(const Frame &frame);
// GPU-access operands of the supported original stream. Binding alone is not access.
std::set<Id> resourceLodAccesses(const Frame &frame, const Entry &entry, Bytes payload = {});
} // namespace flora
