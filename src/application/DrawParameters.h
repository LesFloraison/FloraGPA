#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct ResolvedDraw {
    Event direct;
    nlohmann::json parameters, indirect;
};
// Resolve from current native argument storage, after the event's input edits.
ResolvedDraw resolveDrawParameters(Replay &replay, const Event &event);
} // namespace flora
