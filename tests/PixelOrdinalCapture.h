#pragma once
#include "BlendCapture.h"
namespace flora::testing {
inline void addOrdinalCode(Capture &capture, Id id, const std::string &source, const char *profile) {
    std::erase_if(capture.entries, [&](const auto &e) { return e.id == id; });
    const auto code = compileClassProgram(source, profile);
    auto raw = statePack(uint64_t(code.size()));
    raw.insert(raw.end(), code.begin(), code.end());
    append(raw, Id(0));
    capture.add(id, 9, 0x81, raw);
}
inline Capture pixelOrdinalCapture(uint32_t layout, uint32_t samples, uint32_t mode, uint32_t targets) {
    auto capture = blendCapture(targets, false, false, samples);
    const std::string input = "struct V{float4 p:SV_Position;nointerpolation uint4 identity:TEXCOORD0;};";
    addOrdinalCode(capture, 11,
                   input +
                       "V main(uint v:SV_VertexID,uint i:SV_InstanceID){V o;uint k=v%3;"
                       "o.p=float4(k==2?3:-1,k==1?3:-1,0,1);o.identity=uint4(" +
                       (layout == 1 ? std::string("i") : std::string("v/3")) + ",i,0,0);return o;}",
                   "vs_5_0");
    std::string pixel = input + "struct O{";
    for (uint32_t i = 0; i < targets; ++i)
        pixel += "float4 c" + std::to_string(i) + ":SV_Target" + std::to_string(i) + ";";
    if (mode == 3)
        pixel += "uint coverage:SV_Coverage;";
    pixel += "};";
    if (mode == 2)
        pixel += "[earlydepthstencil]";
    pixel += "O main(V v,uint p:SV_PrimitiveID,uint s:SV_SampleIndex){O o;";
    if (mode == 1 || mode == 2)
        pixel += "if(v.identity.x&1)discard;";
    if (mode == 3)
        pixel += "o.coverage=((v.identity.x&1)==(s&1))?0xffffffff:0;";
    for (uint32_t i = 0; i < targets; ++i)
        pixel += "o.c" + std::to_string(i) + "=float4(v.identity.x+1,p,v.identity.y,s);";
    addOrdinalCode(capture, 13, pixel + "return o;}", "ps_5_0");
    if (layout == 2) {
        capture.add(60, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(61)));
        addOrdinalCode(
            capture, 61,
            input +
                "struct G{float4 p:SV_Position;nointerpolation uint4 identity:TEXCOORD0;uint "
                "primitive:SV_PrimitiveID;};"
                "[maxvertexcount(6)]void main(triangle V v[3],inout TriangleStream<G> output){"
                "for(uint j=0;j<2;++j){for(uint k=0;k<3;++k){G o;o.p=v[k].p;o.identity=v[k].identity;"
                "o.identity.x=2*v[k].identity.x+j;o.primitive=7;output.Append(o);}output.RestartStrip();}}",
            "gs_5_0");
    }
    std::erase_if(capture.entries, [](const auto &e) { return e.id == 90 || e.id == 190 || e.id == 100; });
    State state{};
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.rtCount = targets;
    state.blend = 64;
    state.rasterizer = 52;
    state.viewports = 50;
    state.stages[0].shader = 10;
    state.stages[3].shader = layout == 2 ? 60 : 0;
    state.stages[4].shader = 12;
    for (uint32_t i = 0; i < targets; ++i)
        state.rtv[i] = 1002 + 4 * i;
    capture.add(90, 3, 3, snapshot(state));
    capture.add(190, 3, 3, snapshot(state));
    if (layout == 1)
        capture.add(100, 7, 0x3c, statePack(Id(90), Id(0), Id(1), 3u, 260u, 0u, 0u));
    else
        capture.add(100, 7, 0x37, statePack(Id(90), Id(0), Id(1), layout == 2 ? 390u : 780u, 0u));
    return capture;
}
} // namespace flora::testing
