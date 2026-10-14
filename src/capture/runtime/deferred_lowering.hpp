#pragma once
#include "deferred_history.hpp"
#include <set>
namespace flora::capture {
struct DeferredExpansion {
    std::vector<RecordedCommand> commands;
    std::map<ObjectId,FrozenObject> definitions;
    std::set<ObjectId> resources;
    // View -> first expanded command needing its immediate pre-submission value.
    // No command is encodable until the runtime resolves these dependencies.
    std::map<ObjectId,size_t> initial_counter_uses;
    size_t payload_bytes=0;
};
// CPU-only lowering. No native execution, readback, GPA or replay helper.
DeferredExpansion lower_deferred(const DeferredCommandList&,const DeferredOrigin&,ObjectId immediate_context,
    size_t command_limit,size_t payload_limit,bool allow_counter_snapshots=false);
void complete_deferred_counters(DeferredExpansion&,const std::map<ObjectId,uint32_t>& values);
}
