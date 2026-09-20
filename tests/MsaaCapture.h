#pragma once
#include "StateCapture.h"
namespace flora::testing {
inline Capture msaaOutputCapture(bool integer = true) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    auto shader = [&](Id id, uint16_t type, const char *profile, const std::string &text) {
        Com<ID3DBlob> binary, errors;
        check(D3DCompile(text.data(), text.size(), "MSAA test", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors),
              "Compile MSAA test");
        auto record = std::vector<uint8_t>(48);
        append(record, id + 1);
        c.add(id, 5, type, record);
        auto data = statePack(uint64_t(binary->GetBufferSize()));
        auto start = static_cast<const uint8_t *>(binary->GetBufferPointer());
        data.insert(data.end(), start, start + binary->GetBufferSize());
        append(data, uint64_t(0));
        c.add(id + 1, 9, 0x81, data);
    };
    shader(10, 0x90, "vs_5_0",
           "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}");
    c.add(20, 5, 0x85,
          statePack(Id(0), Id(0), 7u, 5u, 1u, 2u, integer ? 3u : 28u, 4u, 0u, 0u, 32u, 0u, 0u, Id(0)));
    c.add(50, 9, 0x87, statePack(1u, 0.f, 0.f, 7.f, 5.f, 0.f, 1.f));
    c.add(52, 5, 0x89, statePack(Id(0), Id(0), 3u, 1u, 0u, 0, 0.f, 0.f, 1u, 0u, 1u, 0u));
    for (uint32_t layer = 0; layer < 2; ++layer) {
        const Id view = 22 + layer, ps = 30 + layer * 2, event = 100 + layer * 10;
        c.add(view, 5, 0x8d, statePack(Id(0), Id(0), Id(20), integer ? 3u : 28u, 7u, layer, 1u, 0u));
        shader(ps, 0x92, "ps_5_0",
               integer ? "uint4 main(uint s:SV_SampleIndex):SV_Target{return uint4(" +
                             std::to_string(4 + layer * 20) + "+s*4,16777217+s,0,1);}"
                       : "float4 main(uint s:SV_SampleIndex):SV_Target{return float4((" +
                             std::to_string(4 + layer * 20) + "+s*4)/255.0,0,0,1);}");
        State s{};
        s.topology = 4;
        s.sampleMask = UINT32_MAX;
        s.omStart = 8;
        s.rtCount = 1;
        s.rtv[0] = view;
        s.rasterizer = 52;
        s.viewports = 50;
        s.stages[0].shader = 10;
        s.stages[4].shader = ps;
        c.add(event - 1, 3, 3, snapshot(s));
        c.add(event, 7, 0x37, statePack(event - 1, Id(0), Id(1), 3u, 0u));
    }
    return c;
}
} // namespace flora::testing
