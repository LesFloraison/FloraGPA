#pragma once
#include "replay/Replay.h"
#include <QFile>
#include <algorithm>
#include <d3dcompiler.h>
namespace flora::testing {
template <class T> void append(std::vector<uint8_t> &out, const T &value) {
    const auto *p = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof value);
}
template <class T> void put(std::vector<uint8_t> &out, size_t pos, T value) {
    std::memcpy(out.data() + pos, &value, sizeof value);
}
inline std::vector<uint8_t> word(uint32_t value) {
    std::vector<uint8_t> out;
    append(out, value);
    return out;
}
struct Capture {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x128);
    std::vector<Entry> entries;
    void add(Id id, uint8_t category, uint16_t type, std::vector<uint8_t> payload) {
        entries.push_back({id, bytes.size(), uint32_t(payload.size()), 0, category, type});
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    void buffer(Id id, Id dataId, UINT bind, UINT misc, std::array<uint32_t, 4> values) {
        std::vector<uint8_t> resource(16);
        append(resource, D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, bind, 0, misc, misc ? 4u : 0u});
        append(resource, dataId);
        add(id, 5, 0x83, resource);
        auto data = word(16);
        append(data, values);
        add(dataId, 9, 1, data);
    }
    void uav(Id id, Id buffer, UINT flags) {
        std::vector<uint8_t> raw(16);
        append(raw, buffer);
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 4;
        desc.Buffer.Flags = flags;
        append(raw, desc);
        add(id, 5, 0x8f, raw);
    }
    void save(const QString &path) {
        auto output = bytes;
        put(output, 0, 0x41504749u);
        put(output, 4, 0x128u);
        put(output, 8, 3u);
        put(output, 12, uint32_t(entries.size()));
        put(output, 0xf4, uint64_t(output.size()));
        std::memcpy(output.data() + 0x44, "DX11", 4);
        for (auto &e : entries) {
            append(output, e.id);
            append(output, e.offset);
            append(output, e.size);
            append(output, e.flags);
            append(output, e.category);
            append(output, e.type);
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(output.data()), output.size()) != qint64(output.size()))
            throw std::runtime_error("Cannot write test capture");
    }
};
inline std::vector<uint8_t> snapshot(const State &s) {
    std::vector<uint8_t> out(128);
    std::memcpy(out.data(), s.mask.data(), 128);
    append(out, s.ib);
    append(out, s.ibFormat);
    append(out, s.ibOffset);
    append(out, s.layout);
    append(out, s.topology);
    append(out, s.vb);
    append(out, s.strides);
    append(out, s.offsets);
    auto stage = [&](const Stage &x) {
        append(out, x.cb);
        append(out, x.samplers);
        append(out, x.shader);
        append(out, x.srv);
        append(out, x.classes);
        append(out, x.classCount);
    };
    for (int i = 0; i < 4; ++i)
        stage(s.stages[i]);
    append(out, s.so);
    append(out, s.soOffsets);
    append(out, s.soCounts);
    append(out, s.soCount);
    append(out, s.scissors);
    append(out, s.rasterizer);
    append(out, s.viewports);
    stage(s.stages[4]);
    append(out, s.blend);
    append(out, s.blendFactor);
    append(out, s.sampleMask);
    append(out, s.depthState);
    append(out, s.stencilRef);
    append(out, s.rtv);
    append(out, s.omStart);
    append(out, s.dsv);
    append(out, s.omCounts);
    append(out, s.rtCount);
    stage(s.stages[5]);
    append(out, s.csStart);
    append(out, s.csCount);
    append(out, s.csUav);
    append(out, s.csCounts);
    append(out, s.predicate);
    append(out, s.predicateValue);
    append(out, 0u);
    append(out, s.omExtended);
    append(out, s.omExtendedCounts);
    append(out, s.csExtended);
    append(out, s.csExtendedCounts);
    if (out.size() != 22320)
        throw std::runtime_error("Bad synthetic state schema");
    return out;
}
inline Capture computeCapture(bool counter = false) {
    Capture capture;
    capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
    capture.buffer(2, 3, D3D11_BIND_CONSTANT_BUFFER, 0, {2, 0, 0, 0});
    capture.buffer(4, 5, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {7, 8, 9, 10});
    std::vector<uint8_t> srv(16);
    append(srv, Id(4));
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 4;
    append(srv, desc);
    capture.add(6, 5, 0x8c, srv);
    capture.buffer(7, 8, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {10, 0, 0, 0});
    capture.uav(9, 7, 0);
    capture.buffer(10, 11, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {});
    capture.uav(12, 10, counter ? D3D11_BUFFER_UAV_FLAG_COUNTER : D3D11_BUFFER_UAV_FLAG_APPEND);
    capture.buffer(13, 14, 0, 0, {});
    const std::string hlsl =
        std::string("cbuffer Constants:register(b0){uint4 c;} StructuredBuffer<uint> input:register(t0);"
                    "RWStructuredBuffer<uint> output:register(u0);") +
        (counter ? "RWStructuredBuffer<uint> appended:register(u1);"
                 : "AppendStructuredBuffer<uint> appended:register(u1);") +
        "[numthreads(1,1,1)] void main(){output[0]+=input[0]+c.x;" +
        (counter ? "uint i=appended.IncrementCounter(); appended[i]=input[0];}"
                 : "appended.Append(input[0]);}");
    Com<ID3DBlob> binary, diagnostics;
    check(D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &binary,
                     &diagnostics),
          "Compile test shader");
    std::vector<uint8_t> shader(56);
    put(shader, 48, Id(16));
    capture.add(15, 5, 0x93, shader);
    std::vector<uint8_t> code;
    append(code, uint64_t(binary->GetBufferSize()));
    auto first = static_cast<const uint8_t *>(binary->GetBufferPointer());
    code.insert(code.end(), first, first + binary->GetBufferSize());
    append(code, Id(0));
    capture.add(16, 9, 0x81, code);
    State state{};
    state.topology = 1;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[5].shader = 15;
    state.stages[5].cb[0] = 2;
    state.stages[5].srv[0] = 6;
    state.csUav[0] = 9;
    state.csUav[1] = 12;
    state.csCount = 2;
    capture.add(17, 3, 3, snapshot(state));
    // Reset the append counter once, before both dispatches.
    std::vector<uint8_t> setter(16);
    put(setter, 8, Id(1));
    append(setter, 0u);
    append(setter, 2u);
    append(setter, uint8_t(1));
    append(setter, Id(9));
    append(setter, Id(12));
    append(setter, uint8_t(1));
    append(setter, UINT32_MAX);
    append(setter, 0u);
    capture.add(90, 7, 0x25e, setter);
    std::vector<uint8_t> dispatch;
    append(dispatch, Id(17));
    append(dispatch, Id(0));
    append(dispatch, Id(1));
    append(dispatch, 1u);
    append(dispatch, 1u);
    append(dispatch, 1u);
    capture.add(100, 7, 0x35, dispatch);
    capture.add(110, 7, 0x35, dispatch);
    std::vector<uint8_t> copyCount(16);
    put(copyCount, 8, Id(1));
    append(copyCount, Id(13));
    append(copyCount, 0u);
    append(copyCount, Id(12));
    capture.add(111, 7, 0x3f, copyCount);
    return capture;
}
inline uint32_t firstWord(Replay &replay, Id id) {
    auto b = replay.readBuffer(id);
    return Reader(b).read<uint32_t>();
}
inline Capture graphicsCounterCapture(bool counter) {
    auto capture = computeCapture(counter);
    capture.entries.erase(std::remove_if(capture.entries.begin(), capture.entries.end(),
                                         [](const auto &e) { return e.category == 7; }),
                          capture.entries.end());
    std::vector<uint8_t> texture(16);
    append(texture, D3D11_TEXTURE2D_DESC{1,
                                         1,
                                         1,
                                         1,
                                         DXGI_FORMAT_R8G8B8A8_UNORM,
                                         {1, 0},
                                         D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_RENDER_TARGET,
                                         0,
                                         0});
    append(texture, Id(0));
    capture.add(20, 5, 0x85, texture);
    std::vector<uint8_t> rtv(16);
    append(rtv, Id(20));
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    append(rtv, desc);
    capture.add(21, 5, 0x8d, rtv);
    auto viewport = word(1);
    append(viewport, D3D11_VIEWPORT{0, 0, 1, 1, 0, 1});
    capture.add(22, 9, 0x87, viewport);
    auto shader = [&](Id id, uint16_t type, const std::string &source, const char *profile) {
        Com<ID3DBlob> code, errors;
        check(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", profile, 0, 0,
                         &code, &errors),
              "Compile graphics fixture");
        std::vector<uint8_t> resource(56);
        put(resource, 48, id + 1);
        capture.add(id, 5, type, resource);
        std::vector<uint8_t> bytes;
        append(bytes, uint64_t(code->GetBufferSize()));
        auto begin = static_cast<const uint8_t *>(code->GetBufferPointer());
        bytes.insert(bytes.end(), begin, begin + code->GetBufferSize());
        append(bytes, Id(0));
        capture.add(id + 1, 9, 0x81, bytes);
    };
    shader(30, 0x90,
           "float4 main(uint id:SV_VertexID):SV_Position {float2 "
           "p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}",
           "vs_5_0");
    shader(32, 0x92,
           std::string(counter ? "RWStructuredBuffer<uint>" : "AppendStructuredBuffer<uint>") +
               " target:register(u1);float4 main():SV_Target {" +
               (counter ? "uint i=target.IncrementCounter(); target[i]=7;" : "target.Append(7);") +
               "return float4(1,0,0,1);}",
           "ps_5_0");
    State state{};
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.viewports = 22;
    state.stages[0].shader = 30;
    state.stages[4].shader = 32;
    state.rtv[0] = 21;
    state.rtv[1] = 12;
    state.omStart = 1;
    state.rtCount = 2;
    capture.add(34, 3, 3, snapshot(state));
    std::vector<uint8_t> setter(16);
    put(setter, 8, Id(1));
    append(setter, 1u);
    append(setter, uint8_t(1));
    append(setter, Id(21));
    append(setter, Id(0));
    append(setter, 1u);
    append(setter, 1u);
    append(setter, uint8_t(1));
    append(setter, Id(12));
    append(setter, uint8_t(1));
    append(setter, 0u);
    capture.add(190, 7, 0x3500, setter);
    std::vector<uint8_t> draw;
    append(draw, Id(34));
    append(draw, Id(0));
    append(draw, Id(1));
    append(draw, 3u);
    append(draw, 0u);
    capture.add(200, 7, 0x37, draw);
    return capture;
}
} // namespace flora::testing
