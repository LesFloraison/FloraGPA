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
    if (typed != 12 && typed != 30 && typed != 20 && typed != 3) throw std::runtime_error("Unsupported checkpoint format");
    const bool depth = typed == 20;
    if (desc.MipLevels != 1 || desc.SampleDesc.Count <= 1 ||
        !(depth ? (desc.Format == 19 || desc.Format == 20) : typed == 3 ? (desc.Format >= 1 && desc.Format <= 4) : typed == 12 ? (desc.Format >= 9 && desc.Format <= 14) : (desc.Format >= 27 && desc.Format <= 32)))
        throw std::runtime_error("Incompatible checkpoint source");
    desc.Format = depth ? DXGI_FORMAT_R32G8X24_TYPELESS : typed == 3 ? DXGI_FORMAT_R32G32B32A32_TYPELESS : typed == 12 ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : 0);
    Com<ID3D11Texture2D> copy;
    check(device->CreateTexture2D(&desc, nullptr, &copy), "Create checkpoint texture");
    context->CopyResource(copy.Get(), texture.Get());
    return copy;
}
inline std::array<uint32_t, 4> readCheckpoint(ID3D11Device* device, ID3D11Texture2D* source,
                                             uint32_t typed, uint32_t sample, uint32_t layer,
                                             uint32_t x, uint32_t y) {
    D3D11_TEXTURE2D_DESC sourceDesc{}; source->GetDesc(&sourceDesc);
    if ((typed != 12 && typed != 30 && typed != 20 && typed != 3) || sourceDesc.MipLevels != 1 ||
        sourceDesc.SampleDesc.Count <= 1 || sample >= sourceDesc.SampleDesc.Count || layer >= sourceDesc.ArraySize ||
        x >= sourceDesc.Width || y >= sourceDesc.Height) throw std::runtime_error("Invalid checkpoint texel selection");
    const bool depth = typed == 20;
    if (sourceDesc.Format != (depth ? 19 : typed == 3 ? 1 : typed == 12 ? 9 : 27))
        throw std::runtime_error("Checkpoint reader requires matching typeless storage");
    auto compile = [](const std::string& text, const char* profile) {
        Com<ID3DBlob> code, errors;
        check(D3DCompile(text.data(), text.size(), "Texel checkpoint", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors), "Compile checkpoint shader");
        return code;
    };
    const auto vsCode = compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const auto coordinate = "int3(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(layer) + ")," + std::to_string(sample);
    const auto psCode = compile(depth ?
        "Texture2DMSArray<float> tex:register(t0);Texture2DMSArray<uint2> stencil:register(t1);uint4 main():SV_Target{return uint4(asuint(tex.Load(" + coordinate + ")),stencil.Load(" + coordinate + ").y,0,0);}" :
        "Texture2DMSArray<uint4> tex:register(t0);uint4 main():SV_Target{return tex.Load(" + coordinate + ");}", "ps_5_0");
    Com<ID3D11VertexShader> vs; Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Create checkpoint VS");
    check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Create checkpoint PS");
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT(typed); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    sd.Texture2DMSArray.ArraySize = sourceDesc.ArraySize;
    Com<ID3D11ShaderResourceView> srv;
    check(device->CreateShaderResourceView(source, &sd, &srv), "Create checkpoint SRV");
    Com<ID3D11ShaderResourceView> stencil;
    if (depth) {
        sd.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        check(device->CreateShaderResourceView(source, &sd, &stencil), "Create checkpoint stencil SRV");
    }
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
    if (stencil) { auto view = stencil.Get(); deferred->PSSetShaderResources(1, 1, &view); }
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
