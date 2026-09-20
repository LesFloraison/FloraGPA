#include "Replay.h"
#include "Unpredicated.h"
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <iomanip>
#include <sstream>

namespace flora {
namespace {
Com<ID3DBlob> compile(const std::string &source, const char *profile) {
    Com<ID3DBlob> code, error;
    auto hr = D3DCompile(source.data(), source.size(), "FloraGPA preview", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(hr) && error)
        throw std::runtime_error(
            std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize()));
    check(hr, "Compile preview shader");
    return code;
}
uint32_t typed(uint32_t f) {
    static const std::map<uint32_t, uint32_t> types{
        {1, 2},   {5, 6},   {9, 10},  {15, 16}, {19, 21}, {23, 24}, {27, 28}, {33, 34}, {39, 41},
        {44, 46}, {48, 49}, {53, 54}, {60, 61}, {70, 71}, {73, 74}, {76, 77}, {79, 80}, {82, 83},
        {90, 87}, {92, 88}, {94, 95}, {97, 98}, {20, 21}, {40, 41}, {45, 46}, {55, 56}};
    auto it = types.find(f);
    return it == types.end() ? f : it->second;
}
} // namespace
Image Replay::previewTexture(Id id, uint32_t mip, uint32_t layer, uint32_t slice, double low, double high,
                             const std::string &channel) {
    Unpredicated unpredicated(context_.Get());
    PredicateIsolation isolation(*this);
    if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(high - low) || high <= low)
        throw std::runtime_error("Display range must be finite and increasing");
    if (channel != "rgba" && channel != "rgb" && channel != "r" && channel != "g" && channel != "b" &&
        channel != "a")
        throw std::runtime_error("Invalid display channel");
    auto resource = frame_.resource(id);
    auto info = textureInfo(resource);
    if (!info.mips || mip >= info.mips || mip >= 32 || layer >= info.layers ||
        slice >= std::max(1u, info.depth >> mip))
        throw std::runtime_error("Texture selection exceeds storage");
    if (info.samples != 1)
        throw std::runtime_error("MSAA sample inspection migration pending");
    if (info.format >= 103 && info.format <= 105)
        throw std::runtime_error("Planar texture inspection migration pending");
    if (!options_.until && !resource.data)
        throw std::runtime_error("No captured initial bytes; select an event boundary");
    auto source = get<ID3D11Resource>(id);
    auto d = resource.desc;
    const size_t formatIndex = info.dimension == 2 ? 3 : 4;
    switch (d[formatIndex]) {
    case 20:
        d[formatIndex] = 19;
        break;
    case 40:
        d[formatIndex] = 39;
        break;
    case 45:
        d[formatIndex] = 44;
        break;
    case 55:
        d[formatIndex] = 53;
        break;
    }
    d[d.size() - 4] = D3D11_USAGE_DEFAULT;
    d[d.size() - 3] = D3D11_BIND_SHADER_RESOURCE;
    d[d.size() - 2] = 0;
    d[d.size() - 1] = 0;
    Com<ID3D11Resource> texture;
    if (info.dimension == 2) {
        D3D11_TEXTURE1D_DESC desc{};
        std::memcpy(&desc, d.data(), sizeof desc);
        Com<ID3D11Texture1D> obj;
        check(device_->CreateTexture1D(&desc, nullptr, &obj), "Create preview texture");
        texture = obj;
    } else if (info.dimension == 4) {
        D3D11_TEXTURE3D_DESC desc{};
        std::memcpy(&desc, d.data(), sizeof desc);
        Com<ID3D11Texture3D> obj;
        check(device_->CreateTexture3D(&desc, nullptr, &obj), "Create preview texture");
        texture = obj;
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        std::memcpy(&desc, d.data(), sizeof desc);
        Com<ID3D11Texture2D> obj;
        check(device_->CreateTexture2D(&desc, nullptr, &obj), "Create preview texture");
        texture = obj;
    }
    context_->ClearState();
    context_->CopyResource(texture.Get(), source);
    auto fmt = typed(info.format);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT(fmt);
    std::string type, coordinate;
    if (info.dimension == 2) {
        type = "Texture1DArray";
        coordinate = "int3(int(p.x)," + std::to_string(layer) + "," + std::to_string(mip) + ")";
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
        viewDesc.Texture1DArray = {0, info.mips, 0, info.layers};
    } else if (info.dimension == 4) {
        type = "Texture3D";
        coordinate = "int4(int2(p.xy)," + std::to_string(slice) + "," + std::to_string(mip) + ")";
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        viewDesc.Texture3D = {0, info.mips};
    } else {
        type = "Texture2DArray";
        coordinate = "int4(int2(p.xy)," + std::to_string(layer) + "," + std::to_string(mip) + ")";
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray = {0, info.mips, 0, info.layers};
    }
    static const std::set<UINT> uintFormats{3, 7, 12, 17, 22, 25, 30, 36, 42, 47, 50, 57, 62};
    static const std::set<UINT> sintFormats{4, 8, 14, 18, 32, 38, 43, 52, 59, 64};
    auto scalar = uintFormats.contains(fmt) ? "uint4" : sintFormats.contains(fmt) ? "int4" : "float4";
    auto expression = channel == "rgba"  ? "v"
                      : channel == "rgb" ? "float4(v.rgb,1)"
                                         : "float4(v." + channel + ".xxx,1)";
    std::ostringstream ps;
    ps.imbue(std::locale::classic());
    ps << std::setprecision(9) << type << '<' << scalar
       << "> tex:register(t0);float4 main(float4 p:SV_Position):SV_Target{float4 v=(float4(tex.Load("
       << coordinate << "))-" << low << ")/" << (high - low) << ";return " << expression << ";}";
    auto vertex = compile(
             "float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}",
             "vs_5_0"),
         pixel = compile(ps.str(), "ps_5_0");
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> shader;
    check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs),
          "Create preview VS");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &shader),
          "Create preview PS");
    Com<ID3D11ShaderResourceView> srv;
    check(device_->CreateShaderResourceView(texture.Get(), &viewDesc, &srv), "Create preview SRV");
    UINT width = std::max(1u, info.width >> mip), height = std::max(1u, info.height >> mip);
    D3D11_TEXTURE2D_DESC targetDesc{width,
                                    height,
                                    1,
                                    1,
                                    DXGI_FORMAT_R8G8B8A8_UNORM,
                                    {1, 0},
                                    D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET,
                                    0,
                                    0};
    Com<ID3D11Texture2D> target;
    check(device_->CreateTexture2D(&targetDesc, nullptr, &target), "Create preview output");
    Com<ID3D11RenderTargetView> rtv;
    check(device_->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create preview RTV");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    Com<ID3D11RasterizerState> rs;
    check(device_->CreateRasterizerState(&raster, &rs), "Create preview rasterizer");
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context_->RSSetState(rs.Get());
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs.Get(), nullptr, 0);
    context_->PSSetShader(shader.Get(), nullptr, 0);
    auto view = srv.Get();
    context_->PSSetShaderResources(0, 1, &view);
    auto outputView = rtv.Get();
    context_->OMSetRenderTargets(1, &outputView, nullptr);
    context_->Draw(3, 0);
    context_->ClearState();
    targetDesc.Usage = D3D11_USAGE_STAGING;
    targetDesc.BindFlags = 0;
    targetDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(device_->CreateTexture2D(&targetDesc, nullptr, &staging), "Create preview staging");
    context_->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read texture preview");
    Image image{width, height, fmt, {}, id};
    try {
        image.rgba.resize(size_t(width) * height * 4);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(image.rgba.data() + size_t(y) * width * 4,
                        static_cast<uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(width) * 4);
    } catch (...) {
        context_->Unmap(staging.Get(), 0);
        throw;
    }
    context_->Unmap(staging.Get(), 0);
    return image;
}
} // namespace flora
