#pragma once
#include "Replay.h"
namespace flora {
// WARP can copy the wrong adjacent array mip while its RTV remains bound.
// Detach only OM render targets; KEEP preserves UAVs and their hidden counters.
class InspectionCopyBindings {
    ID3D11DeviceContext *context_;
    std::array<Com<ID3D11RenderTargetView>, 8> targets_;
    std::array<ID3D11RenderTargetView *, 8> pointers_{};
    Com<ID3D11DepthStencilView> depth_;
    UINT count_ = 0;
    bool detached_ = false;

  public:
    InspectionCopyBindings(ID3D11DeviceContext *context, bool warp) : context_(context) {
        if (!warp)
            return;
        context_->OMGetRenderTargets(8, pointers_.data(), &depth_);
        for (UINT i = 0; i < 8; ++i) {
            targets_[i].Attach(pointers_[i]);
            if (pointers_[i])
                count_ = i + 1;
        }
        detached_ = count_ || depth_;
        if (detached_)
            context_->OMSetRenderTargetsAndUnorderedAccessViews(
                0, nullptr, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    }
    ~InspectionCopyBindings() {
        if (detached_)
            context_->OMSetRenderTargetsAndUnorderedAccessViews(count_, pointers_.data(), depth_.Get(), 0,
                                                                D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                                nullptr);
    }
    InspectionCopyBindings(const InspectionCopyBindings &) = delete;
    InspectionCopyBindings &operator=(const InspectionCopyBindings &) = delete;
};
} // namespace flora
