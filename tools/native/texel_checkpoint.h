#pragma once
// Development probe only: retain GPU copies at a Draw boundary and read them after replay.
#include "replay/Replay.h"
#include <d3dcompiler.h>

namespace flora::diagnostic {
inline void requireUnpredicated(ID3D11DeviceContext* context) {
    Com<ID3D11Predicate> predicate;
    context->GetPredication(&predicate, nullptr);
    if (predicate) throw std::runtime_error("Checkpoint does not support bound predication");
}
inline Com<ID3D11Texture2D> checkpoint(ID3D11Device* device, ID3D11DeviceContext* context,
                                     IUnknown* source, uint32_t typed) {
    requireUnpredicated(context);
    Com<ID3D11Texture2D> texture;
    check(source->QueryInterface(IID_PPV_ARGS(&texture)), "Get checkpoint texture");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Format = typed == 12 ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> copy;
    check(device->CreateTexture2D(&desc, nullptr, &copy), "Create checkpoint texture");
    context->CopyResource(copy.Get(), texture.Get());
    return copy;
}
inline std::array<uint32_t, 4> readCheckpoint(ID3D11Device* device, ID3D11Texture2D* source,
                                             uint32_t typed, uint32_t sample, uint32_t layer,
                                             uint32_t x, uint32_t y) {
    auto compile = [](const std::string& text, const char* profile) {
        Com<ID3DBlob> code, errors;
        check(D3DCompile(text.data(), text.size(), "Texel checkpoint", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors), "Compile checkpoint shader");
        return code;
    };
    const auto vsCode = compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const auto psCode = compile("Texture2DMSArray<uint4> tex:register(t0);uint4 main():SV_Target{return tex.Load(int3(" +
        std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(layer) + ")," + std::to_string(sample) + ");}", "ps_5_0");
    Com<ID3D11VertexShader> vs; Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Create checkpoint VS");
    check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Create checkpoint PS");
    D3D11_TEXTURE2D_DESC sourceDesc{}; source->GetDesc(&sourceDesc);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT(typed); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    sd.Texture2DMSArray.ArraySize = sourceDesc.ArraySize;
    Com<ID3D11ShaderResourceView> srv;
    check(device->CreateShaderResourceView(source, &sd, &srv), "Create checkpoint SRV");
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_UINT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> target, staging;
    check(device->CreateTexture2D(&td, nullptr, &target), "Create checkpoint output");
    Com<ID3D11RenderTargetView> rtv;
    check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create checkpoint RTV");
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&td, nullptr, &staging), "Create checkpoint staging");
    Com<ID3D11DeviceContext> deferred, immediate;
    check(device->CreateDeferredContext(0, &deferred), "Create checkpoint context");
    device->GetImmediateContext(&immediate); requireUnpredicated(immediate.Get());
    deferred->VSSetShader(vs.Get(), nullptr, 0); deferred->PSSetShader(ps.Get(), nullptr, 0);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1}; deferred->RSSetViewports(1, &viewport);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    Com<ID3D11RasterizerState> rs;
    check(device->CreateRasterizerState(&rd, &rs), "Create checkpoint rasterizer"); deferred->RSSetState(rs.Get());
    auto input = srv.Get(); deferred->PSSetShaderResources(0, 1, &input);
    auto output = rtv.Get(); deferred->OMSetRenderTargets(1, &output, nullptr);
    deferred->Draw(3, 0); deferred->CopyResource(staging.Get(), target.Get());
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish checkpoint commands");
    immediate->ExecuteCommandList(commands.Get(), TRUE);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map checkpoint texel");
    std::array<uint32_t, 4> words{};
    const bool valid = mapped.pData && mapped.RowPitch >= sizeof words;
    if (valid) std::memcpy(words.data(), mapped.pData, sizeof words);
    immediate->Unmap(staging.Get(), 0);
    if (!valid) throw std::runtime_error("Invalid checkpoint row pitch");
    check(device->GetDeviceRemovedReason(), "Checkpoint device status");
    return words;
}
} // namespace flora::diagnostic
