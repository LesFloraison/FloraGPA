#include "QueryInspection.h"
#include "application/ApiCommands.h"
#include "application/QueryHistory.h"

namespace flora {
std::shared_ptr<const QueryInspection> QueryInspection::prepare(std::shared_ptr<const Frame> frame,
                                                               const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (!frame) throw std::invalid_argument("Query inspection requires a capture");
    auto result = std::shared_ptr<QueryInspection>(new QueryInspection(std::move(frame)));
    QueryHistory history(*result->frame_);
    for (const auto &[id, entry] : result->frame_->entries()) {
        checkCancellation(cancelled);
        if (entry.category != 7 || !QueryHistory::observes(entry.type)) continue;
        auto row = inspectCommand(*result->frame_, id);
        checkCancellation(cancelled);
        history.apply(row);
        if (row.contains("query_result")) result->rows_.emplace(id, std::move(row));
    }
    checkCancellation(cancelled);
    return result;
}
const nlohmann::json *QueryInspection::find(Id event) const {
    const auto found = rows_.find(event);
    return found == rows_.end() ? nullptr : &found->second;
}
}
