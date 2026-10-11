#pragma once
#include "capture_model.hpp"
#include <functional>
#include <memory>
namespace flora::capture {
// Candidate resource waits only. A recorded prefix cannot prove completion of
// unrecorded work before the frame, nor materialize an ordinary GPA query object.
struct CopyReadDependency {
    uint64_t sequence=0;
    ObjectId source=0,destination=0;
};
struct EventDependencyPrefix {
    std::vector<CopyReadDependency> copies;
    uint64_t uncovered_commands=0;
    std::optional<uint64_t> first_uncovered_sequence;
    std::string first_uncovered_reason;
};
enum class EventObservationScope { pending, failed, unissued, preframe, deferred, foreign_context, recorded_prefix, preframe_completed };
struct EventDependencyObservation {
    uint64_t sequence=0,generation=0;
    ObjectId query=0;
    EventObservationScope scope=EventObservationScope::unissued;
    std::optional<uint64_t> end_sequence;
    std::shared_ptr<const EventDependencyPrefix> prefix;
};
struct EventDependencyReport {
    uint64_t ends=0;
    std::vector<EventDependencyObservation> observations;
    // Deliberately no "complete query" flag: this API reports only the scope
    // of observed dependencies, never permission to drop or encode EVENT calls.
};
struct EventDependencyOptions {
    size_t entry_limit=1000000; // All retained prefix edges and observations.
    std::function<bool()> cancelled;
};
EventDependencyReport analyze_event_dependencies(const FrozenFrame&,const EventDependencyOptions& = {});
}
