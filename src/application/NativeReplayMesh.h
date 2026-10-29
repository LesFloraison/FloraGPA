#pragma once
#include "NativeArtifacts.h"
#include "ReplayMesh.h"
namespace flora {
struct PostTransformGeometry;
struct NativeReplayMesh {
    std::string contextId;
    ReplayMesh positions;
    nlohmann::json tables, preview;
    NativeArtifactBytes files;
};
// Raw inspection remains available when a position presentation is unavailable.
nlohmann::json nativeReplayMeshPresentationStatus(const nlohmann::json &report);
NativeReplayMesh presentNativeReplayMesh(const PostTransformGeometry &, const CancelCheck & = {});
// Validates retained native artifacts against their raw vertex storage. Position
// exports preserve the existing mesh view's CSV and perspective-divide behavior.
// Input identities are sorted and compacted only when every output byte matches;
// the retained presentation maps compact vertices back to original output rows.
NativeReplayMesh nativeReplayMesh(const nlohmann::json &job, const nlohmann::json &result,
                                  const NativeArtifactBytes &, const CancelCheck & = {});
} // namespace flora
