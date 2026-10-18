#pragma once
#include "NativeResources.h"
#include "SubmissionIndex.h"

namespace flora::dx12 {
// Native execution context. Initialization submissions are executed from their
// indexed recordings; unsupported work fails at its original record boundary.
class NativeReplay final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    NativeReplay(NativeDevice &, const StreamDirectory &, uint64_t allocationLimit, const CancelCheck &cancelled = {});
    ~NativeReplay();
    void initializeTransfers();
    void replayTo(uint64_t execution, bool after, bool analysisMarkers = false);
    nlohmann::json readTexture(uint64_t object, UINT subresource, const std::filesystem::path &output);
    nlohmann::json inspectBuffer(uint64_t creationRecord, uint64_t offset, uint64_t length);
    nlohmann::json verifyInitializedTexture(uint64_t copyRecord);
    nlohmann::json information() const;
};
}
