#pragma once
#include "NativeArtifacts.h"
#include "ReplayMesh.h"
namespace flora {
struct NativeReplayMesh {
    std::string contextId;
    ReplayMesh positions;
    nlohmann::json tables, preview;
    NativeArtifactBytes files;
};
// Validates retained native artifacts against their raw vertex storage. Position
// exports preserve the existing mesh view's CSV and perspective-divide behavior.
NativeReplayMesh nativeReplayMesh(const nlohmann::json &job, const nlohmann::json &result,
                                  const NativeArtifactBytes &, const CancelCheck & = {});
} // namespace flora
