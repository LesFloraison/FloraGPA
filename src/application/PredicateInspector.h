#pragma once
#include "core/Predication.h"
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json describePredicate(const Frame &frame, Id id);
nlohmann::json inspectPredicate(const Frame &frame, Replay &replay, Id id);
} // namespace flora
