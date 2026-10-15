#pragma once
#include "Frame.h"
#include <set>
namespace flora {
struct QueryCompletion {
    Id link{}, context{}, resource{}, end{};
    int32_t result{};
    bool hasData{}, ordinary{};
    uint32_t word{}, size{}, flags{};
    std::string error, missing;
};
bool isQueryGetData(uint16_t type);
QueryCompletion readQueryGetData(Bytes bytes);
// API observations for descriptor-identified EVENT and timestamp queries.
// TIMESTAMP_DISJOINT also permits Begin. The legacy name is retained for callers.
// These have no materialized predicate and do not prove GPU completion.
std::set<Id> auditEventEndObservations(const Frame &, const CancelCheck &cancelled = {});
std::map<Id, uint32_t> auditOrdinaryQueryOperations(const Frame &, const CancelCheck &cancelled = {});
using QueryCompletionAudit = std::map<Id, QueryCompletion>;
QueryCompletionAudit auditQueryCompletions(const Frame &frame, const CancelCheck &cancelled = {});
const QueryCompletion &requireQueryCompletion(const QueryCompletionAudit &audit, Id event);
} // namespace flora
