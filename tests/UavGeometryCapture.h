#pragma once
#include "HullCapture.h"
namespace flora::testing {
inline Capture hullUavReadCapture() {
    auto capture = hullCapture();
    std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 7005; });
    const auto code = compileClassProgram(
        "struct V{float4 p:SV_Position;};"
        "struct C{float edges[3]:SV_TessFactor;float inside:SV_InsideTessFactor;uint tag:ATTR;};"
        "RWStructuredBuffer<uint> dst:register(u1);"
        "[domain(\"tri\")]float4 main(C c,float3 loc:SV_DomainLocation,const OutputPatch<V,3> p)"
        ":SV_Position{return p[0].p*loc.x+p[1].p*loc.y+p[2].p*loc.z+float4(dst[0],0,0,0);}",
        "ds_5_0");
    auto payload = statePack(uint64_t(code.size()));
    payload.insert(payload.end(), code.begin(), code.end());
    append(payload, Id(0));
    capture.add(7005, 9, 0x81, payload);
    return capture;
}
inline Capture geometryUavEmissionCapture() {
    auto capture = hullCapture();
    const auto code =
        compileClassProgram("struct V{float4 p:SV_Position;};RWStructuredBuffer<uint> dst:register(u1);"
                            "[maxvertexcount(9)]void main(point V v[1],inout TriangleStream<V> output){"
                            "InterlockedAdd(dst[0],1);uint n=uint(v[0].p.y)+1;"
                            "for(uint j=0;j<n;++j){for(uint i=0;i<3;++i){V o;o.p=v[0].p;"
                            "o.p.x=float(j*3+i);output.Append(o);}output.RestartStrip();}}",
                            "gs_5_0");
    capture.add(8000, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(8001)));
    auto payload = statePack(uint64_t(code.size()));
    payload.insert(payload.end(), code.begin(), code.end());
    append(payload, Id(0));
    capture.add(8001, 9, 0x81, payload);
    std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 199 || entry.id == 200; });
    State state{};
    state.sampleMask = UINT32_MAX;
    state.topology = 1;
    state.stages[0].shader = 7000;
    state.stages[3].shader = 8000;
    state.viewports = 22;
    state.omStart = 1;
    state.rtCount = 2;
    state.rtv[0] = 21;
    state.rtv[1] = 12;
    capture.add(199, 3, 3, snapshot(state));
    capture.add(200, 7, 0x3c, statePack(Id(199), Id(0), Id(1), 1u, 3u, 0u, 0u));
    return capture;
}
} // namespace flora::testing
