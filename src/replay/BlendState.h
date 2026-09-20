#pragma once
#include "Replay.h"
namespace flora {
inline D3D11_BLEND_DESC1 defaultBlend() {
    D3D11_BLEND_DESC1 d{};
    for (auto &rt : d.RenderTarget) {
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.LogicOp = D3D11_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = 15;
    }
    return d;
}
inline D3D11_BLEND_DESC1 decodeBlend(Bytes bytes, bool extended) {
    Reader r(bytes);
    auto d = defaultBlend();
    if (extended)
        d = r.read<D3D11_BLEND_DESC1>();
    else {
        auto base = r.read<D3D11_BLEND_DESC>();
        d.AlphaToCoverageEnable = base.AlphaToCoverageEnable;
        d.IndependentBlendEnable = base.IndependentBlendEnable;
        for (size_t i = 0; i < 8; ++i) {
            auto &v = base.RenderTarget[i];
            d.RenderTarget[i] = {v.BlendEnable,       FALSE,
                                 v.SrcBlend,          v.DestBlend,
                                 v.BlendOp,           v.SrcBlendAlpha,
                                 v.DestBlendAlpha,    v.BlendOpAlpha,
                                 D3D11_LOGIC_OP_NOOP, v.RenderTargetWriteMask};
        }
    }
    r.end();
    // The recovered decoder treats captured BOOLs as truth values before creation.
    d.AlphaToCoverageEnable = d.AlphaToCoverageEnable != FALSE;
    d.IndependentBlendEnable = d.IndependentBlendEnable != FALSE;
    for (auto &rt : d.RenderTarget) {
        rt.BlendEnable = rt.BlendEnable != FALSE;
        rt.LogicOpEnable = rt.LogicOpEnable != FALSE;
    }
    return d;
}
inline bool usesLogic(const D3D11_BLEND_DESC1 &d) {
    for (size_t i = 0; i < (d.IndependentBlendEnable ? 8u : 1u); ++i)
        if (d.RenderTarget[i].LogicOpEnable)
            return true;
    return false;
}
} // namespace flora
