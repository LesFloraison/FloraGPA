#pragma once
#include "ClassCapture.h"
namespace flora::testing {
inline Capture hullCapture(bool explicitPhase = true) {
    auto c = graphicsCounterCapture(true);
    c.entries.erase(
        std::remove_if(c.entries.begin(), c.entries.end(),
                       [](const auto &e) { return e.category == 3 || (e.category == 7 && e.id != 190); }),
        c.entries.end());
    const std::string
        v = "struct V{float4 p:SV_Position;};",
        constants = "struct C{float edges[3]:SV_TessFactor;float inside:SV_InsideTessFactor;uint tag:ATTR;};";
    auto shader = [&](Id id, uint16_t type, const std::string &text, const char *profile) {
        auto bytes = compileClassProgram(text, profile);
        c.add(id, 5, type, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), id + 1));
        auto raw = statePack(uint64_t(bytes.size()));
        raw.insert(raw.end(), bytes.begin(), bytes.end());
        append(raw, Id(0));
        c.add(id + 1, 9, 0x81, raw);
    };
    shader(7000, 0x90,
           v + "V main(uint id:SV_VertexID,uint inst:SV_InstanceID){V o;o.p=float4(id,inst,0,1);return o;}",
           "vs_5_0");
    shader(
        7002, 0x95,
        v + constants +
            "RWStructuredBuffer<uint> dst:register(u1);C patch(InputPatch<V,3> p,uint pid:SV_PrimitiveID){C "
            "c;c.edges[0]=c.edges[1]=c.edges[2]=c.inside=1;c.tag=42+pid+uint(p[0].p.y)*10;dst."
            "IncrementCounter();dst[pid+uint(p[0].p.y)*2]=c.tag;return "
            "c;}[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")]["
            "outputcontrolpoints(3)][patchconstantfunc(\"patch\")]V main(InputPatch<V,3> p,uint "
            "id:SV_OutputControlPointID,uint pid:SV_PrimitiveID){V o=p[id];" +
            std::string(explicitPhase ? "o.p.z=id*.25+pid;" : "") + "return o;}",
        "hs_5_0");
    shader(7004, 0x94,
           v + constants +
               "[domain(\"tri\")]float4 main(C c,float3 loc:SV_DomainLocation,const OutputPatch<V,3> "
               "p):SV_Position{return p[0].p*loc.x+p[1].p*loc.y+p[2].p*loc.z;}",
           "ds_5_0");
    State s{};
    s.sampleMask = UINT32_MAX;
    s.topology = 35;
    s.viewports = 22;
    s.stages[0].shader = 7000;
    s.stages[1].shader = 7002;
    s.stages[2].shader = 7004;
    s.rtCount = 2;
    s.omStart = 1;
    s.rtv[0] = 21;
    s.rtv[1] = 12;
    c.add(199, 3, 3, snapshot(s));
    c.add(200, 7, 0x3c, statePack(Id(199), Id(0), Id(1), 6u, 2u, 0u, 9u));
    return c;
}
} // namespace flora::testing
