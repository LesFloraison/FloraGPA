#pragma once
#include "StateCapture.h"
namespace flora::testing {
inline Capture textureEditCapture(uint32_t format, uint32_t samples, bool output, bool depth = false,
                                  uint16_t kind = 0x85) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    Resource r;
    r.type = kind;
    const uint32_t bind = depth ? 64u : 40u;
    if (kind == 0x84)
        r.desc = {4, 2, 2, format, 0, bind, 0, 0};
    else if (kind == 0x86)
        r.desc = {4, 4, 4, 2, format, 0, bind, 0, 0};
    else
        r.desc = {4, 4, samples > 1 ? 1u : 2u, 2, format, samples, 0, 0, bind, 0, 0};
    auto raw = statePack(Id(0), Id(0));
    for (auto v : r.desc)
        append(raw, v);
    append(raw, samples > 1 ? Id(0) : Id(21));
    c.add(20, 5, kind, raw);
    if (samples == 1) {
        auto subs = textureSubresources(r);
        auto size = subs.back().offset + subs.back().size;
        auto data = word(uint32_t(size));
        data.resize(size + 4);
        c.add(21, 9, 1, data);
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT(format);
    if (samples > 1) {
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
        srv.Texture2DMSArray = {0, 2};
    } else if (kind == 0x84) {
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
        srv.Texture1DArray = {0, 2, 0, 2};
    } else if (kind == 0x86) {
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D = {0, 2};
    } else {
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray = {0, 2, 0, 2};
    }
    // Depth fixtures exercise persistent outputs, so no invalid typed depth SRV is needed.
    if (!depth) {
        c.add(22, 5, 0x8c, statePack(Id(0), Id(0), Id(20), srv));
        c.add(23, 5, 0x8c, statePack(Id(0), Id(0), Id(20), srv));
    }
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT(format);
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dsv.Texture2DMSArray = {0, 2};
        c.add(24, 5, 0x8e, statePack(Id(0), Id(0), Id(20), dsv));
        c.add(50, 7, 0x31, statePack(Id(0), Id(1), Id(24), 3u, 0.f, uint8_t(0)));
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = DXGI_FORMAT(format);
        if (samples > 1) {
            rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            rtv.Texture2DMSArray = {0, 2};
        } else if (kind == 0x84) {
            rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
            rtv.Texture1DArray = {1, 0, 2};
        } else if (kind == 0x86) {
            rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rtv.Texture3D = {1, 0, 1};
        } else {
            rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray = {1, 0, 2};
        }
        c.add(24, 5, 0x8d, statePack(Id(0), Id(0), Id(20), rtv));
        if (samples > 1)
            c.add(50, 7, 0x32, statePack(Id(0), Id(1), Id(24), uint8_t(1), 0.f, 0.f, 0.f, 0.f));
    }
    const auto shader = [&](Id id, uint16_t type, const char *profile, const char *source) {
        Com<ID3DBlob> binary;
        check(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0,
                         &binary, nullptr),
              "Compile texture edit fixture");
        auto record = std::vector<uint8_t>(48);
        append(record, id + 1);
        c.add(id, 5, type, record);
        auto data = statePack(uint64_t(binary->GetBufferSize()));
        auto first = static_cast<const uint8_t *>(binary->GetBufferPointer());
        data.insert(data.end(), first, first + binary->GetBufferSize());
        append(data, Id(0));
        c.add(id + 1, 9, 0x81, data);
    };
    shader(40, 0x90, "vs_5_0",
           "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}");
    shader(42, 0x92, "ps_5_0", "float4 main():SV_Target{return 0;}");
    c.add(44, 9, 0x87, statePack(1u, 0.f, 0.f, 2.f, 2.f, 0.f, 1.f));
    State state{};
    state.stages[0].shader = 40;
    state.stages[4].shader = 42;
    state.viewports = 44;
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    if (output) {
        if (depth)
            state.dsv = 24;
        else {
            state.rtCount = 1;
            state.rtv[0] = 24;
        }
    } else {
        state.stages[4].srv[0] = 22;
        state.stages[5].srv[0] = 23;
    }
    c.add(90, 3, 3, snapshot(state));
    c.add(100, 7, 0x37, statePack(Id(90), Id(0), Id(1), 0u, 0u));
    c.add(200, 7, 0x37, statePack(Id(90), Id(0), Id(1), 0u, 0u));
    return c;
}
} // namespace flora::testing
