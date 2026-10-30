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
inline Capture terminalDomainCapture(const std::string &mode) {
    auto capture = hullUavReadCapture();
    const auto replace = [&](Id id, const std::string &source, const char *profile) {
        std::erase_if(capture.entries, [&](const auto &entry) { return entry.id == id; });
        const auto code = compileClassProgram(source, profile);
        auto payload = statePack(uint64_t(code.size()));
        payload.insert(payload.end(), code.begin(), code.end());
        append(payload, Id(0));
        capture.add(id, 9, 0x81, payload);
    };
    const std::string declarations =
        "struct V{float4 p:SV_Position;};"
        "struct C{float edges[3]:SV_TessFactor;float inside:SV_InsideTessFactor;uint tag:ATTR;};"
        "RWStructuredBuffer<uint> dst:register(u1);";
    if (mode == "ds-upstream-alias") {
        replace(7003,
                declarations + "C patch(InputPatch<V,3> p,uint pid:SV_PrimitiveID){C c;"
                               "c.edges[0]=c.edges[1]=c.edges[2]=c.inside=1+(dst[0]%2);c.tag=0;return c;}"
                               "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")]"
                               "[outputcontrolpoints(3)][patchconstantfunc(\"patch\")]"
                               "V main(InputPatch<V,3> p,uint id:SV_OutputControlPointID){return p[id];}",
                "hs_5_0");
    } else {
        // The HS writes the same first word. This makes cross-replay byte
        // comparisons deterministic without assuming a graphics UAV schedule.
        for (const auto &entry : capture.entries)
            if (entry.id == 11)
                put(capture.bytes, entry.offset + 4, uint32_t(42));
    }
    if (mode == "ds-gs-alias") {
        const auto code = compileClassProgram(
            "struct V{float4 p:SV_Position;};[maxvertexcount(9)]"
            "void main(triangle V v[3],inout TriangleStream<V> output){"
            "uint count=1+uint(v[0].p.x)%3;for(uint j=0;j<count;++j){"
            "output.Append(v[0]);output.Append(v[1]);output.Append(v[2]);output.RestartStrip();}}",
            "gs_5_0");
        capture.add(8000, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(8001)));
        auto payload = statePack(uint64_t(code.size()));
        payload.insert(payload.end(), code.begin(), code.end());
        append(payload, Id(0));
        capture.add(8001, 9, 0x81, payload);
        State state{};
        state.sampleMask = UINT32_MAX;
        state.topology = 35;
        state.viewports = 22;
        state.stages[0].shader = 7000;
        state.stages[1].shader = 7002;
        state.stages[2].shader = 7004;
        state.stages[3].shader = 8000;
        state.rtCount = 2;
        state.omStart = 1;
        state.rtv[0] = 21;
        state.rtv[1] = 12;
        std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 199; });
        capture.add(199, 3, 3, snapshot(state));
        return capture;
    }
    replace(7005,
            declarations +
                "[domain(\"tri\")]float4 main(C c,float3 loc:SV_DomainLocation,const OutputPatch<V,3> p)"
                ":SV_Position{uint value;InterlockedOr(dst[0],0,value);"
                "float4 result=p[0].p*loc.x+p[1].p*loc.y+p[2].p*loc.z;" +
                (mode == "ds-nonfinite" ? std::string("result.x=asfloat((value&1)?0x7fc00000:0x7f800000);")
                                        : std::string("if(value)result.x+=value;else result.z+=1;")) +
                "return result;}",
            "ds_5_0");
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
inline Capture geometryResourceReadCapture(bool uav, bool alias, bool unbound = false) {
    auto capture = geometryUavEmissionCapture();
    std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 8001 || entry.id == 199; });
    const auto code =
        compileClassProgram("struct V{float4 p:SV_Position;};RWStructuredBuffer<uint> dst:register(u1);" +
                                std::string(uav ? (alias ? "" : "RWStructuredBuffer<uint> src:register(u2);")
                                                : "StructuredBuffer<uint> src:register(t0);") +
                                "[maxvertexcount(9)]void main(point V v[1],inout TriangleStream<V> output){"
                                "InterlockedAdd(dst[0],1);uint n=1+(" +
                                std::string(uav && alias ? "dst" : "src") +
                                "[uint(v[0].p.y)]%3);"
                                "for(uint j=0;j<n;++j){for(uint i=0;i<3;++i){V o;o.p=v[0].p;"
                                "o.p.x=float(j*3+i);output.Append(o);}output.RestartStrip();}}",
                            "gs_5_0");
    auto payload = statePack(uint64_t(code.size()));
    payload.insert(payload.end(), code.begin(), code.end());
    append(payload, Id(0));
    capture.add(8001, 9, 0x81, payload);
    capture.buffer(9000, 9001, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                   D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {0, 1, 2, 0});
    if (alias && !uav)
        for (const auto &entry : capture.entries)
            if (entry.id == 10)
                put(capture.bytes, entry.offset + 24,
                    uint32_t(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS));
    if (uav)
        capture.uav(9002, alias ? 10 : 9000, 0);
    else {
        auto view = statePack(Id(0), Id(0), Id(alias ? 10 : 9000));
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 4;
        append(view, desc);
        capture.add(9002, 5, 0x8c, view);
    }
    State state{};
    state.sampleMask = UINT32_MAX;
    state.topology = 1;
    state.stages[0].shader = 7000;
    state.stages[3].shader = 8000;
    state.viewports = 22;
    state.omStart = 1;
    state.rtCount = uav && !alias ? 3 : 2;
    state.rtv[0] = 21;
    state.rtv[1] = 12;
    if (uav && !alias)
        state.rtv[2] = unbound ? 0 : 9002;
    else if (!uav)
        state.stages[3].srv[0] = unbound ? 0 : 9002;
    capture.add(199, 3, 3, snapshot(state));
    return capture;
}
inline Capture fixedGeometryCapture(const std::string &mode) {
    auto capture = geometryUavEmissionCapture();
    const auto replace = [&](Id id, const std::string &source, const char *profile) {
        std::erase_if(capture.entries, [&](const auto &entry) { return entry.id == id; });
        const auto code = compileClassProgram(source, profile);
        auto payload = statePack(uint64_t(code.size()));
        payload.insert(payload.end(), code.begin(), code.end());
        append(payload, Id(0));
        capture.add(id, 9, 0x81, payload);
    };
    for (const auto &entry : capture.entries)
        if (entry.id == 11)
            put(capture.bytes, entry.offset + 4, uint32_t(42));
    const std::string declarations =
        "struct V{float4 p:SV_Position;};RWStructuredBuffer<uint> dst:register(u1);";
    std::string body = mode == "atomic" ? "uint value;InterlockedOr(dst[0],2,value);" : "uint value=dst[0];";
    body += "InterlockedAdd(dst[1],1);";
    if (mode == "branch")
        body += "[branch]if(value==0)return;";
    body += "V o;o.p=v[0].p;o.p.x=float(value);";
    if (mode == "nonfinite")
        body += "o.p.x=asfloat(0x7f800000);o.p.z=float(value);";
    body += "output.Append(o);o.p.x+=1;output.Append(o);o.p.x+=1;output.Append(o);output.RestartStrip();}";
    replace(8001,
            declarations + "[maxvertexcount(3)]void main(point V v[1],inout TriangleStream<V> output){" +
                body,
            "gs_5_0");
    if (mode == "upstream")
        replace(7001,
                declarations + "V main(uint id:SV_VertexID,uint inst:SV_InstanceID){V "
                               "o;o.p=float4(dst[0],inst,0,1);return o;}",
                "vs_5_0");
    return capture;
}
} // namespace flora::testing
