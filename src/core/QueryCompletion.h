#pragma once
#include "Frame.h"
namespace flora {
struct QueryCompletion {
    Id link{}, context{}, resource{}, end{};
    int32_t result{};
    bool hasData{};
    uint32_t word{}, size{}, flags{};
    std::string error, missing;
};
bool isQueryGetData(uint16_t type);
QueryCompletion readQueryGetData(Bytes bytes);
using QueryCompletionAudit = std::map<Id, QueryCompletion>;
QueryCompletionAudit auditQueryCompletions(const Frame &frame, const CancelCheck &cancelled = {});
const QueryCompletion &requireQueryCompletion(const QueryCompletionAudit &audit, Id event);
} // namespace flora
