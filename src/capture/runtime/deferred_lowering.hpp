#pragma once
#include "deferred_history.hpp"
#include <set>
namespace flora::capture {
struct DeferredExpansion {
    std::vector<RecordedCommand> commands;
    std::map<ObjectId,FrozenObject> definitions;
    std::set<ObjectId> resources;
    size_t payload_bytes=0;
};
// CPU-only lowering. No native execution, readback, GPA or replay helper.
DeferredExpansion lower_deferred(const DeferredCommandList&,const DeferredOrigin&,ObjectId immediate_context,
    size_t command_limit,size_t payload_limit);
}
