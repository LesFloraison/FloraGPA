#pragma once
#include "QuadResources.h"
namespace flora {
struct QuadPreparedDepth {
    Resource resource;
    Com<ID3D11Resource> object;
    Com<ID3D11DepthStencilView> view;
    nlohmann::json metadata;
    std::string digest(Replay &replay) const;
};
// Operates inside the selected event's bound input/experiment scope. The
// callback submits the original geometry and may rebind private graphics UAVs.
class QuadDepth {
    Replay &r;

  public:
    explicit QuadDepth(Replay &replay) : r(replay) {}
    // For callbacks using copyStreamOutput=false: preserve stale hardware SO
    // destinations through Replay's shared private-sink workaround.
    void suspendStreamOutput();
    QuadPreparedDepth prepare(const Event &event, const State &state, const QuadTarget &target,
                              const std::function<uint64_t()> &submit, bool copyStreamOutput = true);
};
} // namespace flora
