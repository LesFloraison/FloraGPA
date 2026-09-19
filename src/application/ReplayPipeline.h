#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
// Executes the selected boundary once. Draw inspection occurs inside scoped resource edits.
nlohmann::json inspectReplayPipeline(const Frame &frame, Replay &replay, bool experimentApplied = false,
                                     const std::function<void(Id, size_t, size_t)> &progress = {});
} // namespace flora
