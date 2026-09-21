#pragma once
#include "replay/Replay.h"
#include <algorithm>
namespace flora {
// Rebinding a capture restores its low slots. Keep the native high slots too,
// starting after restored RTVs so OM does not reject an overlapping UAV range.
class QuadHighUavs {
    ID3D11DeviceContext *context;
    UINT start;
    bool active;
    std::array<Com<ID3D11UnorderedAccessView>, 56> held;

  public:
    QuadHighUavs(ID3D11DeviceContext *c, const State &state)
        : context(c), start(std::min({state.rtCount, state.omStart, 8u})) {
        Com<ID3D11Device> device;
        context->GetDevice(&device);
        active = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1;
        if (active) {
            std::array<ID3D11UnorderedAccessView *, 56> pointers{};
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 8, 56, pointers.data());
            for (size_t i = 0; i < held.size(); ++i)
                held[i].Attach(pointers[i]);
        }
    }
    ~QuadHighUavs() {
        if (!active)
            return;
        std::array<ID3D11UnorderedAccessView *, 64> pointers{};
        std::array<Com<ID3D11UnorderedAccessView>, 8> lower;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, pointers.data());
        for (size_t i = 0; i < lower.size(); ++i)
            lower[i].Attach(pointers[i]);
        for (size_t i = 0; i < held.size(); ++i)
            pointers[i + 8] = held[i].Get();
        context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                                                           nullptr, nullptr, start, 64 - start,
                                                           pointers.data() + start, nullptr);
    }
    QuadHighUavs(const QuadHighUavs &) = delete;
    QuadHighUavs &operator=(const QuadHighUavs &) = delete;
};
} // namespace flora
