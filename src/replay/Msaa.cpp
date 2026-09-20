#include "Replay.h"
#include "Unpredicated.h"
#include <d3dcompiler.h>

namespace flora {
namespace {
Com<ID3DBlob> compileMsaa(const std::string &source, const char *profile) {
    Com<ID3DBlob> code, errors;
    auto hr = D3DCompile(source.data(), source.size(), "FloraGPA MSAA", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr, "Compile MSAA inspection shader");
    return code;
}
struct DepthFamily {
    uint32_t typed, storage, depth, stencil;
};
std::optional<DepthFamily> depthFamily(uint32_t format, uint32_t bind) {
    if (!(bind & D3D11_BIND_DEPTH_STENCIL) && format != 20 && format != 40 && format != 45 && format != 55)
        return {};
    switch (format) {
    case 19:
    case 20:
        return DepthFamily{20, 19, 21, 22};
    case 44:
    case 45:
        return DepthFamily{45, 44, 46, 47};
    case 39:
    case 40:
        return DepthFamily{40, 39, 41, 0};
    case 53:
    case 55:
        return DepthFamily{55, 53, 56, 0};
    default:
        return {};
    }
}
std::optional<std::pair<uint32_t, uint32_t>> integerFamily(uint32_t format, uint32_t typed) {
    struct Family {
        uint32_t first, last, access;
    };
    static constexpr Family families[] = {{1, 4, 3},    {5, 8, 7},    {9, 14, 12},  {15, 18, 17},
                                          {23, 25, 25}, {27, 32, 30}, {33, 38, 36}, {39, 43, 42},
                                          {48, 52, 50}, {53, 59, 57}, {60, 64, 62}};
    for (const auto &f : families)
        if (format >= f.first && format <= f.last && typed >= f.first && typed <= f.last)
            return std::pair{f.first, f.access};
    return {};
}
} // namespace

MsaaStorage Replay::readMsaa(Id id, std::optional<uint32_t> sample, uint32_t typedFormat) {
    const auto resource = frame_.resource(id);
    const auto info = textureInfo(resource);
    if (info.dimension != 3 || info.samples <= 1 || info.mips != 1)
        throw std::runtime_error("MSAA inspection requires a multisampled 2D texture with one mip");
    if (sample && *sample >= info.samples)
        throw std::runtime_error("MSAA sample index out of bounds");
    auto source = get<ID3D11Resource>(id);
    const auto depth = depthFamily(info.format, resource.desc[8]);
    const auto bits = sample ? integerFamily(info.format, typedFormat) : std::nullopt;
    const auto targetFormat = depth ? depth->typed : typedFormat;
    MsaaStorage result;
    result.resource = resource;
    result.resource.data = 0;
    result.resource.desc[4] = targetFormat;
    result.resource.desc[5] = 1;
    result.resource.desc[6] = 0;
    result.resource.desc[7] = D3D11_USAGE_DEFAULT;
    result.resource.desc[8] = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    result.resource.desc[9] = result.resource.desc[10] = 0;
    result.integerBits = bits.has_value();
    result.depthStencil = depth.has_value();
    auto targetResource = result.resource;
    if (bits) {
        targetResource.desc[4] = bits->second;
        targetResource.desc[8] = D3D11_BIND_RENDER_TARGET;
    }
    auto texture = [&](const Resource &r) {
        D3D11_TEXTURE2D_DESC desc{};
        std::memcpy(&desc, r.desc.data(), sizeof desc);
        Com<ID3D11Texture2D> out;
        check(device_->CreateTexture2D(&desc, nullptr, &out), "Create MSAA inspection texture");
        return out;
    };
    auto target = texture(targetResource);
    UINT support = 0;
    check(device_->CheckFormatSupport(DXGI_FORMAT(targetFormat), &support), "MSAA resolve format support");
    const bool hardware = !sample && !depth && (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE);
    Unpredicated unpredicated(context_.Get());
    PredicateIsolation isolation(*this);
    Com<ID3D11DeviceContext> deferred;
    check(device_->CreateDeferredContext(0, &deferred), "Create MSAA inspection context");
    if (hardware) {
        result.mode = "hardware_resolve";
        for (uint32_t layer = 0; layer < info.layers; ++layer)
            deferred->ResolveSubresource(target.Get(), layer, source, layer, DXGI_FORMAT(targetFormat));
    } else {
        result.mode = sample ? "selected_sample" : "gpa_shader_mean";
        auto copyResource = resource;
        copyResource.data = 0;
        copyResource.desc[4] = bits ? bits->first : depth ? depth->storage : info.format;
        copyResource.desc[7] = D3D11_USAGE_DEFAULT;
        copyResource.desc[8] = D3D11_BIND_SHADER_RESOURCE | (depth && !bits ? D3D11_BIND_DEPTH_STENCIL : 0);
        copyResource.desc[9] = copyResource.desc[10] = 0;
        auto readable = texture(copyResource);
        deferred->CopyResource(readable.Get(), source);
        auto vertex = compileMsaa(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        Com<ID3D11VertexShader> vs;
        check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs),
              "Create MSAA VS");
        deferred->VSSetShader(vs.Get(), nullptr, 0);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_VIEWPORT viewport{0, 0, float(info.width), float(info.height), 0, 1};
        deferred->RSSetViewports(1, &viewport);
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.MultisampleEnable = bits ? TRUE : FALSE;
        Com<ID3D11RasterizerState> rs;
        check(device_->CreateRasterizerState(&raster, &rs), "Create MSAA rasterizer");
        deferred->RSSetState(rs.Get());
        auto srv = [&](uint32_t format, uint32_t first, uint32_t count) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT(format);
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {first, count};
            Com<ID3D11ShaderResourceView> view;
            check(device_->CreateShaderResourceView(readable.Get(), &desc, &view), "Create MSAA SRV");
            return view;
        };
        auto pixel = [&](const std::string &text) {
            auto binary = compileMsaa(text, "ps_5_0");
            Com<ID3D11PixelShader> ps;
            check(
                device_->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr, &ps),
                "Create MSAA PS");
            deferred->PSSetShader(ps.Get(), nullptr, 0);
            deferred->Draw(3, 0);
        };
        auto bindSrv = [&](ID3D11ShaderResourceView *view) { deferred->PSSetShaderResources(0, 1, &view); };
        auto depthState = [&](bool write, UINT stencilMask) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            desc.DepthEnable = write;
            desc.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            desc.StencilEnable = !write;
            desc.StencilReadMask = 255;
            desc.StencilWriteMask = UINT8(stencilMask);
            desc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                              write ? D3D11_STENCIL_OP_KEEP : D3D11_STENCIL_OP_REPLACE,
                              D3D11_COMPARISON_ALWAYS};
            desc.BackFace = desc.FrontFace;
            Com<ID3D11DepthStencilState> state;
            check(device_->CreateDepthStencilState(&desc, &state), "Create MSAA depth stencil state");
            deferred->OMSetDepthStencilState(state.Get(), write ? 0 : 255);
        };
        if (depth && !bits) {
            auto depthView = srv(depth->depth, 0, info.layers);
            auto stencilView =
                depth->stencil ? srv(depth->stencil, 0, info.layers) : Com<ID3D11ShaderResourceView>{};
            for (uint32_t layer = 0; layer < info.layers; ++layer) {
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT(targetFormat);
                desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray = {0, layer, 1};
                Com<ID3D11DepthStencilView> view;
                check(device_->CreateDepthStencilView(target.Get(), &desc, &view), "Create MSAA result DSV");
                deferred->ClearDepthStencilView(
                    view.Get(), D3D11_CLEAR_DEPTH | (stencilView ? D3D11_CLEAR_STENCIL : 0), 0, 0);
                deferred->OMSetRenderTargets(0, nullptr, view.Get());
                depthState(true, 255);
                bindSrv(depthView.Get());
                const auto coordinate = "int3(int2(p.xy)," + std::to_string(layer) + ")";
                auto body = sample ? "return tex.Load(" + coordinate + "," + std::to_string(*sample) + ").x;"
                                   : "float v=0;for(uint s=0;s<" + std::to_string(info.samples) +
                                         ";s++)v+=tex.Load(" + coordinate + ",s).x/" +
                                         std::to_string(info.samples) + ".0;return v;";
                pixel("Texture2DMSArray<float2> tex:register(t0);float main(float4 p:SV_Position):SV_Depth{" +
                      body + "}");
                if (stencilView) {
                    bindSrv(stencilView.Get());
                    body = sample ? "uint v=tex.Load(" + coordinate + "," + std::to_string(*sample) + ").y;"
                                  : "float total=0;for(uint s=0;s<" + std::to_string(info.samples) +
                                        ";s++)total+=tex.Load(" + coordinate + ",s).y;uint v=(uint)(total/" +
                                        std::to_string(info.samples) + ".0);";
                    for (unsigned bit = 0; bit < 8; ++bit) {
                        depthState(false, 1u << bit);
                        pixel("Texture2DMSArray<uint2> tex:register(t0);float main(float4 "
                              "p:SV_Position):SV_Depth{" +
                              body + "if((v & " + std::to_string(1u << bit) + ")==0)discard;return 0;}");
                    }
                }
            }
        } else {
            static const std::set<uint32_t> uintFormats{3, 7, 12, 17, 22, 25, 30, 36, 42, 47, 50, 57, 62};
            static const std::set<uint32_t> sintFormats{4, 8, 14, 18, 32, 38, 43, 52, 59, 64};
            const std::string kind = bits || uintFormats.contains(targetFormat) ? "uint4"
                                     : sintFormats.contains(targetFormat)       ? "int4"
                                                                                : "float4";
            const auto access = bits ? bits->second : targetFormat;
            for (uint32_t layer = 0; layer < info.layers; ++layer) {
                auto view = srv(access, bits ? layer : 0, bits ? 1 : info.layers);
                bindSrv(view.Get());
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT(access);
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray = {0, layer, 1};
                Com<ID3D11RenderTargetView> output;
                check(device_->CreateRenderTargetView(target.Get(), &desc, &output),
                      "Create MSAA result RTV");
                auto raw = output.Get();
                deferred->OMSetRenderTargets(1, &raw, nullptr);
                const auto coordinate = "int3(int2(p.xy)," + std::to_string(bits ? 0 : layer) + ")";
                const auto body = sample
                                      ? "return tex.Load(" + coordinate + "," + std::to_string(*sample) + ");"
                                      : "float4 v=0;for(uint s=0;s<" + std::to_string(info.samples) +
                                            ";s++)v+=tex.Load(" + coordinate + ",s);return (" + kind +
                                            ")(v/" + std::to_string(info.samples) + ".0);";
                pixel("Texture2DMSArray<" + kind + "> tex:register(t0);" + kind +
                      " main(float4 p:SV_Position):SV_Target{" + body + "}");
            }
        }
    }
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish MSAA inspection commands");
    context_->ExecuteCommandList(commands.Get(), TRUE);
    check(device_->GetDeviceRemovedReason(), "MSAA inspection device status");
    result.bytes = readTextureStorage(target.Get(), targetResource);
    if (!bits && (targetFormat == 88 || targetFormat == 93)) {
        result.canonicalX = true;
        for (size_t i = 3; i < result.bytes.size(); i += 4)
            result.bytes[i] = 0;
    }
    return result;
}
} // namespace flora
