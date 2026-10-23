#pragma once
#include "Replay.h"
namespace flora {
class NativeSample final {
    Com<ID3D11DeviceContext> context_;
    std::array<Com<ID3D11Query>, 8> queries_;
    Com<ID3D11Query> disjoint_, start_, end_, completion_;
    bool active_{}, ended_{};

  public:
    NativeSample(ID3D11Device *device, ID3D11DeviceContext *context);
    ~NativeSample();
    void begin();
    void end();
    // A preceding sample's completion can cover this sample when both have ended
    // on the same immediate context. Query reads still retain their own deadline.
    NativeStatistics result(bool synchronize = true);
};
} // namespace flora
