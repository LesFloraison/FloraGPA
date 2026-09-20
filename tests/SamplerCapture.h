#pragma once
#include "DepthStencilCapture.h"
namespace flora::testing {
inline Capture samplerCapture() {
    auto c = depthStencilCapture();
    c.entries.erase(
        std::remove_if(c.entries.begin(), c.entries.end(),
                       [](const auto &e) { return e.id == 32 || e.id == 33 || e.id == 990 || e.id == 1990; }),
        c.entries.end());
    const auto code = compileClassProgram("Texture2D<float4> t:register(t0); SamplerState s:register(s2); "
                                          "float4 main():SV_Target{return t.SampleLevel(s,float2(-1,-1),0);}",
                                          "ps_5_0");
    c.add(32, 5, 0x92, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(33)));
    auto raw = statePack(uint64_t(code.size()));
    raw.insert(raw.end(), code.begin(), code.end());
    append(raw, Id(0));
    c.add(33, 9, 0x81, raw);
    c.add(730, 5, 0x85, statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, 2u, 1u, 0u, 0u, 8u, 0u, 0u, Id(731)));
    c.add(731, 9, 1, statePack(16u, 0.f, 0.f, 0.f, 0.f));
    c.add(732, 5, 0x8c, statePack(Id(0), Id(0), Id(730), 2u, 4u, 0u, 1u, 0u, 0u));
    for (Id id : {Id(740), Id(741)}) {
        D3D11_SAMPLER_DESC d{};
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
        d.MaxAnisotropy = 1;
        d.BorderColor[id == 740 ? 0 : 1] = .375f;
        d.BorderColor[3] = 1;
        c.add(id, 5, 0x88, statePack(Id(0), Id(0), d));
    }
    c.add(850, 7, 0x34e8, statePack(Id(0), Id(1), 2u, 1u, uint8_t(1), Id(740)));
    c.add(900, 7, 0x34e8, statePack(Id(0), Id(1), 2u, 1u, uint8_t(1), Id(0)));
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = 1;
    s.rtv[0] = 21;
    s.rasterizer = 724;
    s.viewports = 22;
    s.stages[0].shader = 30;
    s.stages[4].shader = 32;
    s.stages[4].srv[0] = 732;
    c.add(990, 3, 3, snapshot(s));
    c.add(1990, 3, 3, snapshot(s));
    return c;
}
} // namespace flora::testing
