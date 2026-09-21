#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
std::map<std::string, uint64_t> compatibleReplayCounts(const Replay &replay, Id start, Id end,
                                                       std::map<std::string, uint64_t> counts);
nlohmann::json gpuStatisticsReport(const Replay &replay);
void exportGpuStatistics(const nlohmann::json &report, const std::filesystem::path &directory);
} // namespace flora
