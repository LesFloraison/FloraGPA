#pragma once
#include "NativeResources.h"
#include "SubmissionIndex.h"
#include "ShaderReplacement.h"
#include "ResourcePatch.h"
#include <set>

namespace flora::dx12 {
// Native execution context. Initialization submissions are executed from their
// indexed recordings; unsupported work fails at its original record boundary.
class NativeReplay final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    NativeReplay(NativeDevice &, const StreamDirectory &, uint64_t allocationLimit, const CancelCheck &cancelled = {});
    ~NativeReplay();
    // Explicit incomplete replay; never reconstructs historical query values.
    // Isolated readbacks reject ordinary consumers and unknown CPU reads.
    void setIsolateMissingQueries(bool enabled);
    void setDisabledExecutions(std::set<uint64_t> executions);
    // Explicit experiment: insert a global UAV barrier before selected direct
    // Draw/Dispatch instances. Does not repair or modify the original capture.
    void setUavBarriersBefore(std::set<uint64_t> executions);
    void setShaderReplacements(std::vector<ShaderReplacement>);
    void setResourcePatches(std::vector<ResourcePatch>);
    // Plan before initialization to restore lists recorded in the keyframe at
    // their original CPU boundaries, without executing their frame work early.
    void prepareExecution(uint64_t execution, bool after, bool analysisMarkers = false, bool performance = false);
    nlohmann::json performanceResult() const;
    void initializeTransfers();
    void replayTo(uint64_t execution, bool after, bool analysisMarkers = false);
    nlohmann::json readTexture(uint64_t object, UINT subresource, const std::filesystem::path &output, uint64_t expectedCreation = 0);
    nlohmann::json inspectBuffer(uint64_t creationRecord, uint64_t offset, uint64_t length,
                                const std::filesystem::path &output = {});
    nlohmann::json verifyInitializedTexture(uint64_t copyRecord);
    nlohmann::json information() const;
};
}
