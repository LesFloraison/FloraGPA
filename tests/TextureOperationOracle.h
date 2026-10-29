#pragma once
#include "application/DxbcTextureInputs.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QtTest>
#include <d3d11sdklayers.h>

namespace flora::testing {
using namespace dxbc_exec;
// Execute the supplied original shader independently of the native GPU helper.
inline std::array<uint32_t, 4> directTexture(ID3D11Device *device, Bytes code, uint32_t stage,
                                             const TextureInput &input, const TextureInputView &view,
                                             const std::vector<std::array<uint32_t, 4>> &values,
                                             uint32_t outputPrecision, const SamplerInput *sampler = nullptr,
                                             uint32_t outputCount = 1, uint32_t outputIndex = 0) {
    if (!outputCount || outputCount > 1024 || outputIndex >= outputCount ||
        (stage != 5 && (outputCount != 1 || outputIndex != 0)))
        throw std::runtime_error("Invalid independent texture output range");
    Com<ID3D11DeviceContext> context;
    auto shaderCheck = [&](HRESULT status, const char *message) {
        if (FAILED(status)) {
            qInfo() << "Original shader stage" << stage << "output precision" << outputPrecision;
            Com<ID3D11InfoQueue> queue;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
                for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T size = 0;
                    queue->GetMessage(i, nullptr, &size);
                    std::vector<uint8_t> bytes(size);
                    auto *entry = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                    if (SUCCEEDED(queue->GetMessage(i, entry, &size)))
                        qInfo().noquote() << entry->pDescription;
                }
        }
        check(status, message);
    };
    device->GetImmediateContext(&context);
    context->ClearState();
    const auto subs = textureSubresources(input.resource);
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
    for (const auto &sub : subs)
        initial.push_back(
            {input.storage.bytes.data() + sub.offset, UINT(sub.rowPitch), UINT(sub.slicePitch)});
    Com<ID3D11Resource> resource;
    auto create = [&]<class T, class D>(auto method) {
        D desc{};
        std::memcpy(&desc, input.resource.desc.data(), sizeof desc);
        Com<T> texture;
        shaderCheck((device->*method)(&desc, initial.data(), &texture),
                    "Create independent precision texture");
        resource = texture;
    };
    if (input.resource.type == 0x84)
        create.operator()<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>(&ID3D11Device::CreateTexture1D);
    else if (input.resource.type == 0x85)
        create.operator()<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>(&ID3D11Device::CreateTexture2D);
    else
        create.operator()<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>(&ID3D11Device::CreateTexture3D);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    std::memcpy(&desc, view.nativeDescriptor.data(), sizeof desc);
    Com<ID3D11ShaderResourceView> srv;
    if (view.planeSlice) {
        Com<ID3D11Device3> extended;
        check(device->QueryInterface(IID_PPV_ARGS(&extended)), "Get independent planar view device");
        D3D11_SHADER_RESOURCE_VIEW_DESC1 selected{};
        selected.Format = desc.Format;
        selected.ViewDimension = desc.ViewDimension;
        if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
            selected.Texture2D = {desc.Texture2D.MostDetailedMip, desc.Texture2D.MipLevels, *view.planeSlice};
        else if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
            selected.Texture2DArray = {desc.Texture2DArray.MostDetailedMip, desc.Texture2DArray.MipLevels,
                                       desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize,
                                       *view.planeSlice};
        else
            throw std::runtime_error("Invalid independent planar view dimension");
        Com<ID3D11ShaderResourceView1> actual;
        check(extended->CreateShaderResourceView1(resource.Get(), &selected, &actual),
              "Create independent plane SRV");
        D3D11_SHADER_RESOURCE_VIEW_DESC1 observed{};
        actual->GetDesc1(&observed);
        if (std::memcmp(&selected, &observed, sizeof selected))
            throw std::runtime_error("Independent plane view descriptor changed");
        srv = actual;
    } else
        check(device->CreateShaderResourceView(resource.Get(), &desc, &srv),
              "Create independent precision SRV");
    if (input.resource.desc.back() & D3D11_RESOURCE_MISC_RESOURCE_CLAMP)
        context->SetResourceMinLOD(resource.Get(), input.minLod);
    D3D11_BUFFER_DESC bd{
        UINT(values.size() * 16), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA constants{values.data(), 0, 0};
    Com<ID3D11Buffer> cb;
    check(device->CreateBuffer(&bd, &constants, &cb), "Create independent precision constants");
    auto *buffer = cb.Get();
    auto *source = view.resource ? srv.Get() : nullptr;
    Com<ID3D11SamplerState> samplerState;
    if (sampler && sampler->descriptor) {
        D3D11_SAMPLER_DESC sd{};
        std::memcpy(&sd, sampler->descriptor->data(), sizeof sd);
        check(device->CreateSamplerState(&sd, &samplerState), "Create independent texture sampler");
    }
    auto *samplerPointer = samplerState.Get();
    context->VSSetSamplers(4, 1, &samplerPointer);
    context->PSSetSamplers(4, 1, &samplerPointer);
    context->CSSetSamplers(4, 1, &samplerPointer);
    Com<ID3D11Buffer> output;
    Com<ID3D11Texture2D> target;
    if (stage == 0) {
        const auto vertex = compileHlsl("float4 main(uint id:SV_VertexID):SV_Position{"
                                        "float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,0,1);}",
                                        "vs_5_0")
                                .bytecode;
        Com<ID3D11VertexShader> vs;
        Com<ID3D11PixelShader> ps;
        check(device->CreateVertexShader(vertex.data(), vertex.size(), nullptr, &vs), "Create oracle VS");
        shaderCheck(device->CreatePixelShader(code.data(), code.size(), nullptr, &ps),
                    "Create original precision PS");
        D3D11_TEXTURE2D_DESC td{1,
                                1,
                                1,
                                1,
                                outputPrecision == 1 || outputPrecision == 2 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                : outputPrecision == 4                       ? DXGI_FORMAT_R32G32B32A32_SINT
                                                                             : DXGI_FORMAT_R32G32B32A32_UINT,
                                {1, 0},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET,
                                0,
                                0};
        check(device->CreateTexture2D(&td, nullptr, &target), "Create independent precision target");
        Com<ID3D11RenderTargetView> rtv;
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create oracle RTV");
        auto *rt = rtv.Get();
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = true;
        Com<ID3D11RasterizerState> rs;
        check(device->CreateRasterizerState(&raster, &rs), "Create oracle raster state");
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(2, 1, &buffer);
        context->PSSetShaderResources(3, 1, &source);
        context->RSSetState(rs.Get());
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &rt, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);
    } else {
        bd = {outputCount * 16,
              D3D11_USAGE_DEFAULT,
              UINT(stage == 1 ? D3D11_BIND_STREAM_OUTPUT : D3D11_BIND_UNORDERED_ACCESS),
              0,
              stage == 1 ? 0u : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
              stage == 1 ? 0u : 16u};
        check(device->CreateBuffer(&bd, nullptr, &output), "Create oracle output buffer");
        if (stage == 1) {
            Com<ID3D11VertexShader> vs;
            Com<ID3D11GeometryShader> gs;
            shaderCheck(device->CreateVertexShader(code.data(), code.size(), nullptr, &vs),
                        "Create original precision VS");
            const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 0, 0, 4, 0};
            const UINT stride = 16, offset = 0;
            check(device->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), &entry, 1, &stride,
                                                               1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
                                                               &gs),
                  "Create oracle SO transport");
            auto *out = output.Get();
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(2, 1, &buffer);
            context->VSSetShaderResources(3, 1, &source);
            context->GSSetShader(gs.Get(), nullptr, 0);
            context->SOSetTargets(1, &out, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->Draw(1, 0);
        } else {
            Com<ID3D11ComputeShader> cs;
            Com<ID3D11UnorderedAccessView> uav;
            shaderCheck(device->CreateComputeShader(code.data(), code.size(), nullptr, &cs),
                        "Create original precision CS");
            check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create oracle output UAV");
            auto *out = uav.Get();
            context->CSSetShader(cs.Get(), nullptr, 0);
            context->CSSetConstantBuffers(2, 1, &buffer);
            context->CSSetShaderResources(3, 1, &source);
            context->CSSetUnorderedAccessViews(3, 1, &out, nullptr);
            context->Dispatch(1, 1, 1);
        }
    }
    context->ClearState();
    Com<ID3D11Resource> staging;
    if (stage == 0) {
        D3D11_TEXTURE2D_DESC td{};
        target->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> copy;
        check(device->CreateTexture2D(&td, nullptr, &copy), "Create oracle texture staging");
        context->CopyResource(copy.Get(), target.Get());
        staging = copy;
    } else {
        output->GetDesc(&bd);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> copy;
        check(device->CreateBuffer(&bd, nullptr, &copy), "Create oracle buffer staging");
        context->CopyResource(copy.Get(), output.Get());
        staging = copy;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read independent precision output");
    std::array<uint32_t, 4> result;
    std::memcpy(result.data(), static_cast<const uint8_t *>(mapped.pData) + outputIndex * 16, 16);
    context->Unmap(staging.Get(), 0);
    return result;
}
} // namespace flora::testing
