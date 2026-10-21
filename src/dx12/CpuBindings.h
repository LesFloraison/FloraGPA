#pragma once
#include "SubmissionIndex.h"
#include <memory>
#include <nlohmann/json.hpp>
namespace flora::dx12 {
// Immutable CPU index. No device, replay controller or native resource creation.
// Command-list generations and descriptor revisions retain separate histories.
class CpuBindings final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    CpuBindings(const StreamDirectory &, const SubmissionIndex &, const CancelCheck & = {});
    ~CpuBindings();
    nlohmann::json snapshot(uint64_t execution) const;
};
}
