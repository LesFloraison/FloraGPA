#pragma once
#include "NativeArtifacts.h"
#include "PostTransform.h"
namespace flora {
struct NativeMesh {
    PostTransformGeometry geometry;
    nlohmann::json tables;
    NativeArtifactBytes artifacts;
};
// Reads assembled VS/DS/GS results; hull phases retain their separate representation.
// The byte budget bounds the retained artifact payload, not JSON parsing overhead.
NativeMesh readNativeMesh(const nlohmann::json &job, const nlohmann::json &result, const QString &directory,
                          uint64_t maximumBytes, const CancelCheck &cancelled = {});
} // namespace flora
