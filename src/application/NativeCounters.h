#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
// Generic D3D11 counters at each draw/dispatch submission boundary in one replay.
nlohmann::json collectNativeEventCounters(Replay &replay, const CancelCheck &cancelled = {});
void exportNativeEventCounters(const nlohmann::json &report, const std::filesystem::path &directory);
} // namespace flora
