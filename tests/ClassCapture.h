#pragma once
#include "StateCapture.h"
namespace flora::testing {
inline std::vector<uint8_t> compileClassProgram(const std::string &source, const char *profile) {
    Com<ID3DBlob> binary, error;
    auto hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", profile, 0, 0,
                         &binary, &error);
    if (FAILED(hr) && error)
        throw std::runtime_error(static_cast<const char *>(error->GetBufferPointer()));
    check(hr, "Compile dynamic class fixture");
    auto begin = static_cast<const uint8_t *>(binary->GetBufferPointer());
    return {begin, begin + binary->GetBufferSize()};
}
inline std::vector<uint8_t> classProgram(bool interfaces = true) {
    const std::string source =
        interfaces
            ? "interface I{uint apply(uint x);};"
              "class A:I{uint value;uint apply(uint x){return x+value;}};"
              "class B:I{uint value;uint apply(uint x){return x+value*2;}};"
              "cbuffer Classes:register(b0){A first[2];B second[2];} I selected;"
              "RWStructuredBuffer<uint> dst:register(u0);"
              "[numthreads(4,1,1)]void main(uint3 id:SV_DispatchThreadID){dst[id.x]=selected.apply(id.x);}"
            : "RWStructuredBuffer<uint> dst:register(u0);"
              "[numthreads(4,1,1)]void main(uint3 id:SV_DispatchThreadID){dst[id.x]=77+id.x;}";
    return compileClassProgram(source, "cs_5_0");
}
inline Capture classCapture(bool created = false, uint32_t classCount = 1, Id instance = 62,
                            uint32_t createdFlag = UINT32_MAX, bool badName = false) {
    Capture c;
    c.add(1, 5, 0x127, statePack(Id(0), Id(0), 0u, 0u));
    std::vector<uint8_t> buffer(16);
    append(buffer, D3D11_BUFFER_DESC{64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0});
    append(buffer, Id(3));
    c.add(2, 5, 0x83, buffer);
    auto data = word(64);
    for (uint32_t value : {10, 20, 30, 40}) {
        append(data, value);
        data.resize(data.size() + 12);
    }
    c.add(3, 9, 1, data);
    c.buffer(7, 8, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {});
    c.uav(9, 7, 0);
    auto program = classProgram();
    c.add(15, 5, 0x93, statePack(Id(0), Id(0), Id(0), Id(0), Id(60), Id(0), Id(16)));
    auto code = statePack(uint64_t(program.size()));
    code.insert(code.end(), program.begin(), program.end());
    append(code, Id(0));
    c.add(16, 9, 0x81, code);
    c.add(60, 5, 0x97, statePack(Id(0), Id(0)));
    auto names = statePack(created ? 0u : 6u);
    if (!created) {
        for (char ch : std::string("first"))
            names.push_back(ch);
        names.push_back(badName ? 'X' : 0);
    }
    append(names, created ? 2u : 0u);
    if (created) {
        names.push_back('A');
        names.push_back(badName ? 'X' : 0);
    }
    c.add(64, 9, 0x89, names);
    c.add(62, 5, 0x98,
          statePack(Id(0), Id(60), 0u, 1u, 0u, created ? 2u : 0u, created ? 3u : 0u, 0u, 0u,
                    createdFlag == UINT32_MAX ? uint32_t(created) : createdFlag, Id(64)));
    State state{};
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[5].shader = 15;
    state.stages[5].cb[created ? 2 : 0] = 2;
    state.stages[5].classes[0] = instance;
    state.stages[5].classCount = classCount;
    state.csUav[0] = 9;
    state.csCount = 1;
    c.add(80, 3, 3, snapshot(state));
    c.add(100, 7, 0x35, statePack(Id(80), Id(0), Id(1), 1u, 1u, 1u));
    c.add(110, 7, 0x35, statePack(Id(80), Id(0), Id(1), 1u, 1u, 1u));
    return c;
}
inline Capture graphicsClassCapture(bool emptyTable = false) {
    auto c = graphicsCounterCapture(false);
    c.entries.erase(
        std::remove_if(c.entries.begin(), c.entries.end(),
                       [](const auto &e) { return e.id == 2 || e.id == 3 || e.id == 32 || e.id == 33; }),
        c.entries.end());
    auto classes = classCapture();
    for (const auto &e : classes.entries)
        if (e.id == 2 || e.id == 3 || e.id == 60 || e.id == 62 || e.id == 64)
            c.add(e.id, e.category, e.type,
                  {classes.bytes.begin() + e.offset, classes.bytes.begin() + e.offset + e.size});
    auto program = compileClassProgram(
        "interface I{uint apply(uint x);};class A:I{uint value;uint apply(uint x){return x+value;}};" +
            std::string(emptyTable ? "cbuffer Classes:register(b0){A first[2];} I selected;"
                                   : "class B:I{uint value;uint apply(uint x){return x+value*2;}};"
                                     "cbuffer Classes:register(b0){A first[2];B second[2];} I selected;") +
            "float4 main():SV_Target{return float4(selected.apply(0)==20?1:0,0,0,1);}",
        "ps_5_0");
    c.add(32, 5, 0x92, statePack(Id(0), Id(0), Id(0), Id(0), Id(60), Id(0), Id(33)));
    auto code = statePack(uint64_t(program.size()));
    code.insert(code.end(), program.begin(), program.end());
    append(code, Id(0));
    c.add(33, 9, 0x81, code);
    for (const auto &e : c.entries)
        if (e.id == 34) {
            put(c.bytes, e.offset + 14056, Id(2));
            put(c.bytes, e.offset + 15328, Id(62));
            put(c.bytes, e.offset + 17376, 1u);
        }
    return c;
}
} // namespace flora::testing
