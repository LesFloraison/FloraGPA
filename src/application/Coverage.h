#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct CoverageOptions {
    bool depthTest = true;
    std::string mode = "fragment", target = "auto";
    std::optional<uint32_t> layer;
};
struct CoverageResult {
    nlohmann::json report;
    Image mask, after, overlay;
};
// Owns a prefix replay through the selected Draw. The normal replay command
// submits the original work exactly once; helpers use isolated output storage.
CoverageResult captureCoverage(Replay &replay, Id event, const CoverageOptions &options = {});
void exportCoverage(const CoverageResult &result, const std::filesystem::path &directory);
} // namespace flora
