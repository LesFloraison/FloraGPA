#pragma once
#include "ClassCapture.h"
namespace flora::testing {
inline Capture blendCapture(UINT targets = 1, bool dual = false, bool logical = false, UINT samples = 1,
                            int capturedLogic = -1, bool legacy = false) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    auto shader = [&](Id id, bool pixel, const std::string &source) {
        auto code = compileClassProgram(source, pixel ? "ps_5_0" : "vs_5_0");
        c.add(id, 5, pixel ? 0x92 : 0x90, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), id + 1));
        auto raw = statePack(uint64_t(code.size()));
        raw.insert(raw.end(), code.begin(), code.end());
        append(raw, Id(0));
        c.add(id + 1, 9, 0x81, raw);
    };
    shader(10, false, "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}");
    std::string source = "struct O{";
    for (UINT i = 0; i < (dual ? 2u : targets); ++i)
        source += std::string(logical ? "uint4" : "float4") + " t" + std::to_string(i) + ":SV_Target" +
                  std::to_string(i) + ";";
    source += "};O main(){O o;";
    for (UINT i = 0; i < (dual ? 2u : targets); ++i)
        source += "o.t" + std::to_string(i) + "=" +
                  (logical          ? "uint4(0xabcdef12u,0x13579bdfu,0xff00ff00u,0x80000001u)"
                   : dual && i == 1 ? "float4(.25,.75,.5,.25)"
                                    : "float4(.5,.25,.75,.5)") +
                  ";";
    shader(12, true, source + "return o;}");
    for (UINT i = 0; i < targets; ++i) {
        Id id = 1000 + 4 * i;
        UINT format = logical ? 3u : 2u;
        c.add(id, 5, 0x85,
              statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, format, samples, 0u, 0u, 32u, 0u, 0u,
                        samples == 1 ? id + 1 : Id(0)));
        if (samples == 1)
            c.add(id + 1, 9, 1,
                  logical ? statePack(16u, 0x0f0f0f0fu, 0x2468ace0u, 0x55aa55aau, 0x7fffffffu)
                          : statePack(16u, .125f, .5f, .25f, .25f));
        c.add(id + 2, 5, 0x8d, statePack(Id(0), Id(0), id, format, samples == 1 ? 4u : 6u, 0u, 0u, 0u));
        if (samples > 1)
            c.add(70 + i, 7, 0x32, statePack(Id(0), Id(1), id + 2, uint8_t(1), .125f, .5f, .25f, .25f));
    }
    c.add(50, 9, 0x87, statePack(1u, 0.f, 0.f, 1.f, 1.f, 0.f, 1.f));
    c.add(52, 5, 0x89, statePack(Id(0), Id(0), 3u, 1u, 0u, 0, 0.f, 0.f, 1u, 0u, samples > 1 ? 1u : 0u, 0u));
    auto blend = statePack(Id(0), Id(0), 0u, 0u);
    for (UINT i = 0; i < 8; ++i) {
        auto row = legacy ? statePack(0u, 2u, 1u, 1u, 2u, 1u, 1u, 15u)
                          : statePack(0u, capturedLogic >= 0 ? 1u : 0u, 2u, 1u, 1u, 2u, 1u, 1u,
                                      capturedLogic < 0 ? 4u : UINT(capturedLogic), 15u);
        blend.insert(blend.end(), row.begin(), row.end());
    }
    c.add(64, 5, legacy ? 0x8a : 0x10d, blend);
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = targets;
    s.blend = 64;
    s.blendFactor = {.25f, .5f, .75f, 1.f};
    s.rasterizer = 52;
    s.viewports = 50;
    s.stages[0].shader = 10;
    s.stages[4].shader = 12;
    for (UINT i = 0; i < targets; ++i)
        s.rtv[i] = 1002 + 4 * i;
    c.add(90, 3, 3, snapshot(s));
    c.add(190, 3, 3, snapshot(s));
    c.add(100, 7, 0x37, statePack(Id(90), Id(0), Id(1), 3u, 0u));
    c.add(200, 7, 0x37, statePack(Id(190), Id(0), Id(1), 3u, 0u));
    return c;
}
inline std::array<uint32_t, 4> blendPixel(Replay &replay, Id id = 1000, UINT sample = 0) {
    Com<ID3D11Texture2D> source;
    Com<ID3D11DeviceContext> context;
    replay.inspectNativeState([&](auto *native, const auto &objects) {
        context = native;
        check(objects.at(id).As(&source), "Get blend test texture");
    });
    Com<ID3D11Device> device;
    context->GetDevice(&device);
    context->ClearState();
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    std::array<uint32_t, 4> pixel{};
    if (desc.SampleDesc.Count == 1) {
        desc.BindFlags = desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Blend staging texture");
        context->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read blend pixel");
        std::memcpy(pixel.data(), mapped.pData, 16);
        context->Unmap(staging.Get(), 0);
    } else {
        // A test-only shader reads one exact MSAA sample, without resolving or quantizing it.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Com<ID3D11Texture2D> copy;
        check(device->CreateTexture2D(&desc, nullptr, &copy), "MSAA oracle copy");
        context->CopyResource(copy.Get(), source.Get());
        Com<ID3D11ShaderResourceView> srv;
        check(device->CreateShaderResourceView(copy.Get(), nullptr, &srv), "MSAA oracle SRV");
        D3D11_BUFFER_DESC bd{
            16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            16};
        Com<ID3D11Buffer> output;
        check(device->CreateBuffer(&bd, nullptr, &output), "MSAA oracle output");
        Com<ID3D11UnorderedAccessView> uav;
        check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "MSAA oracle UAV");
        auto bytes = compileClassProgram(
            "Texture2DMS<float4> src:register(t0);RWStructuredBuffer<uint4> "
            "dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=asuint(src.Load(int2(0,0)," +
                std::to_string(sample) + "));}",
            "cs_5_0");
        Com<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, &shader),
              "MSAA oracle shader");
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, srv.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        context->Dispatch(1, 1, 1);
        context->ClearState();
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;
        check(device->CreateBuffer(&bd, nullptr, &staging), "MSAA oracle staging");
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read MSAA sample");
        std::memcpy(pixel.data(), mapped.pData, 16);
        context->Unmap(staging.Get(), 0);
    }
    return pixel;
}
} // namespace flora::testing
