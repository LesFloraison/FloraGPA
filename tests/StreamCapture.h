#pragma once
#include "ClassCapture.h"
namespace flora::testing {
inline const std::string streamProducer =
    "struct V{float4 p:SV_Position;};[maxvertexcount(3)]"
    "void main(point V input[1],inout TriangleStream<V> dst){V v;"
    "v.p=float4(-1,-1,0,1);dst.Append(v);v.p=float4(-1,3,0,1);dst.Append(v);"
    "v.p=float4(3,-1,0,1);dst.Append(v);dst.RestartStrip();}";
inline Capture streamCapture(bool signatureOnly = false, bool dirty = false,
                             const std::string &producerSource = streamProducer) {
    auto c = graphicsCounterCapture(false);
    c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                   [](const auto &e) {
                                       return e.category == 7 || e.id == 32 || e.id == 33 || e.id == 34;
                                   }),
                    c.entries.end());
    auto shader = [&](Id id, uint16_t type, Bytes code, Id so = 0) {
        c.add(id, 5, type, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), so, id + 1));
        auto raw = statePack(uint64_t(code.size()));
        raw.insert(raw.end(), code.begin(), code.end());
        append(raw, Id(0));
        c.add(id + 1, 9, 0x81, raw);
    };
    auto ps = compileClassProgram("float4 main():SV_Target{return float4(1,0,0,1);}", "ps_5_0");
    shader(32, 0x92, ps);
    auto vertex = compileClassProgram("float4 main(float4 p:POSITION):SV_Position{return p;}", "vs_5_0");
    shader(80, 0x90, vertex);
    auto producer = compileClassProgram(producerSource, "gs_5_0");
    if (signatureOnly) {
        Com<ID3DBlob> signature;
        check(D3DGetOutputSignatureBlob(vertex.data(), vertex.size(), &signature), "Extract SO signature");
        auto begin = static_cast<const uint8_t *>(signature->GetBufferPointer());
        producer.assign(begin, begin + signature->GetBufferSize());
    }
    shader(60, 0x91, producer, 62);
    c.add(62, 9, 0x85,
          statePack(1u, 0u, 0u, Id(64), 0u, uint8_t(0), uint8_t(4), uint8_t(0), uint8_t(0), 1u, 16u, 0u));
    auto name = [](const char *text) {
        auto n = uint32_t(std::strlen(text) + 1);
        auto raw = statePack(n);
        raw.insert(raw.end(), text, text + n);
        return raw;
    };
    c.add(64, 9, 1, name("SV_Position"));
    c.add(84, 9, 1, name("POSITION"));
    c.add(70, 5, 0x83, statePack(Id(0), Id(0), 256u, 0u, 17u, 0u, 0u, 0u, Id(71)));
    auto raw = statePack(256u);
    raw.resize(260, 0xcd);
    c.add(71, 9, 1, raw);
    c.add(82, 5, 0x82, statePack(Id(0), Id(0), Id(83)));
    auto layout = statePack(1u, Id(84), 0u, 2u, 0u, 0u, 0u, 0u, uint32_t(vertex.size()));
    layout.insert(layout.end(), vertex.begin(), vertex.end());
    c.add(83, 9, 0x84, layout);
    State s{};
    s.topology = signatureOnly ? 4 : 1;
    s.sampleMask = UINT32_MAX;
    s.viewports = 22;
    s.stages[0].shader = 30;
    s.stages[3].shader = 60;
    s.stages[4].shader = 32;
    s.rtv[0] = 21;
    s.rtCount = 1;
    s.omStart = 8;
    s.soCount = 1;
    s.so[0] = 70;
    s.soOffsets[0] = 16;
    c.add(90, 3, 3, snapshot(s));
    c.add(100, 7, 0x37, statePack(Id(90), Id(0), Id(1), signatureOnly ? 3u : 1u, 0u));
    if (dirty)
        s.mask[20] = 0x80000000;
    c.add(140, 3, 3, snapshot(s));
    c.add(150, 7, 0x37, statePack(Id(140), Id(0), Id(1), signatureOnly ? 3u : 1u, 0u));
    s.mask.fill(0);
    s.soCount = 0;
    s.so.fill(0);
    s.topology = 4;
    s.stages[3].shader = 0;
    s.stages[0].shader = 80;
    s.layout = 82;
    s.vb[0] = 70;
    s.strides[0] = 16;
    s.offsets[0] = 16;
    s.soCounts[0] = 99;
    c.add(190, 3, 3, snapshot(s));
    c.add(200, 7, 0x38, statePack(Id(190), Id(0), Id(1)));
    return c;
}
} // namespace flora::testing
