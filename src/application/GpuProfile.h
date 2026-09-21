#pragma once
#include "replay/Replay.h"
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
struct GpuProfileRequest {
    std::optional<Id> start, end;
    unsigned samples = 5, warmup = 1;
    bool includeWrites = false;
};
struct GpuProfileSelection {
    Id start{}, end{};
    std::vector<Id> events;
};
GpuProfileRequest gpuProfileRequest(const nlohmann::json &value);
GpuProfileSelection gpuProfileSelection(const Frame &frame, const GpuProfileRequest &request);
nlohmann::json timingDistribution(std::vector<double> values);
nlohmann::json profileTiming(uint64_t frequency, bool disjoint, uint64_t start, uint64_t end, uint64_t origin,
                             uint64_t limit);
nlohmann::json profileGpu(Replay &replay, const Frame &capture, const GpuProfileRequest &request,
                          const QString &directory);
void exportGpuProfile(const nlohmann::json &report, const QString &directory);
} // namespace flora
