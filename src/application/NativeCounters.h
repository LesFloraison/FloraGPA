#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
// Generic D3D11 counters at draw/dispatch and resource-write boundaries in one replay.
nlohmann::json collectNativeEventCounters(Replay &replay, const CancelCheck &cancelled = {});
void exportNativeEventCounters(const nlohmann::json &report, const std::filesystem::path &directory);
// Validate the project-owned counter schema and expand exact values for presentation.
nlohmann::json nativeCounterRows(const nlohmann::json &report);
} // namespace flora
