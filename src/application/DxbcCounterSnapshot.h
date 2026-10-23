#pragma once
#include "DxbcExecution.h"

namespace flora {
class Replay;
namespace dxbc_exec {
struct CounterSnapshot {
    uint64_t event = 0;
    std::map<uint64_t, Counter> counters;
    std::map<Register, uint64_t> bindings;
    std::map<uint64_t, std::string> unavailable;
};
// Requires a successful replay stopped before the selected work event. Event
// experiment edits are scoped by Replay; the returned CPU state is independent.
CounterSnapshot snapshotCounters(Replay &, uint64_t event, const CancelCheck &cancelled = {});
} // namespace dxbc_exec
} // namespace flora
