#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json gpuStatisticsReport(const Replay &replay);
void exportGpuStatistics(const nlohmann::json &report, const std::filesystem::path &directory);
} // namespace flora
