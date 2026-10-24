#include "InspectionCopyBindings.h"
#include "Replay.h"
#include "Unpredicated.h"
#include <algorithm>
#include <chrono>
#include <d3dcompiler.h>
#include <thread>

namespace flora {
std::vector<std::array<uint32_t, 4>>
Replay::observeTextureSamples(Id id, const D3D11_SHADER_RESOURCE_VIEW_DESC &view, uint32_t returnType,
                              uint64_t maxBytes, const CancelCheck &cancelled, uint32_t timeoutMs) {
    checkCancellation(cancelled);
    if (!timeoutMs || timeoutMs > 60000 || returnType < 3 || returnType > 5)
        throw std::invalid_argument("Invalid multisample observation options");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto metadata = observeTextureMetadata(id);
    const auto info = textureInfo(metadata.resource);
    const bool array = view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    const auto first = array ? view.Texture2DMSArray.FirstArraySlice : 0;
    const auto layers = array ? view.Texture2DMSArray.ArraySize : 1;
    if ((!array && view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DMS) || info.dimension != 3 ||
        info.samples <= 1 || info.samples > 32 || info.mips != 1 || !layers || first >= info.layers ||
        layers > info.layers - first || (!array && info.layers != 1))
        throw std::invalid_argument("Invalid multisample observation view");
    const uint64_t count = uint64_t(info.width) * info.height * layers * info.samples;
    if (!count || count >= UINT32_MAX / 16 || (count + 1) * 16 > maxBytes)
        throw std::runtime_error("Multisample observation byte budget exceeded");
    const auto bytes = UINT((count + 1) * 16);
    D3D11_TEXTURE2D_DESC desc{};
    std::memcpy(&desc, metadata.resource.desc.data(), sizeof desc);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags &= D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
    Com<ID3D11Texture2D> copy;
    check(device_->CreateTexture2D(&desc, nullptr, &copy), "Create private multisample input");
    Com<ID3D11ShaderResourceView> srv;
    check(device_->CreateShaderResourceView(copy.Get(), &view, &srv), "Create private multisample view");
    D3D11_BUFFER_DESC buffer{
        bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        16};
    Com<ID3D11Buffer> output, staging;
    check(device_->CreateBuffer(&buffer, nullptr, &output), "Create multisample observation output");
    buffer.Usage = D3D11_USAGE_STAGING;
    buffer.BindFlags = buffer.MiscFlags = buffer.StructureByteStride = 0;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device_->CreateBuffer(&buffer, nullptr, &staging), "Create multisample observation staging");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = UINT(count + 1);
    Com<ID3D11UnorderedAccessView> uav;
    check(device_->CreateUnorderedAccessView(output.Get(), &uavDesc, &uav), "Create sample output view");
    const std::string type = returnType == 3 ? "int4" : returnType == 4 ? "uint4" : "float4";
    const std::string source = std::string(array ? "Texture2DMSArray<" : "Texture2DMS<") + type +
                               "> src:register(t0);"
                               "RWStructuredBuffer<uint4> dst:register(u0);"
                               "[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID){"
                               "uint i=tid.x+tid.y*4194240u; if(i>" +
                               std::to_string(count) +
                               "u)return;"
                               "uint s=i%" +
                               std::to_string(info.samples) + "u;uint p=i/" + std::to_string(info.samples) +
                               "u;"
                               "uint x=p%" +
                               std::to_string(info.width) + "u; p/=" + std::to_string(info.width) +
                               "u;"
                               "uint y=p%" +
                               std::to_string(info.height) + "u;uint z=p/" + std::to_string(info.height) +
                               "u;"
                               "if(i==" +
                               std::to_string(count) + "u){x=" + std::to_string(info.width) +
                               "u;y=z=s=0;}"
                               "dst[i]=asuint(src.Load(" +
                               (array ? "int3(x,y,z)" : "int2(x,y)") + ",s));}";
    Com<ID3DBlob> code, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "FloraGPA sample observation", nullptr, nullptr,
                               "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr, "Compile multisample observation");
    Com<ID3D11ComputeShader> shader;
    check(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
          "Create multisample observation shader");
    Com<ID3D11DeviceContext> deferred;
    check(device_->CreateDeferredContext(0, &deferred), "Create multisample observation context");
    deferred->CSSetShader(shader.Get(), nullptr, 0);
    auto *input = srv.Get();
    auto *target = uav.Get();
    deferred->CSSetShaderResources(0, 1, &input);
    deferred->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    const auto groups = UINT((count + 64) / 64);
    deferred->Dispatch(std::min(groups, 65535u), (groups + 65534u) / 65535u, 1);
    deferred->ClearState();
    deferred->CopyResource(staging.Get(), output.Get());
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish multisample observation");
    std::vector<std::array<uint32_t, 4>> result(size_t(count + 1));
    checkCancellation(cancelled);
    if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Multisample observation timed out before submission");
    {
        Unpredicated unpredicated(context_.Get());
        PredicateIsolation predicates(*this);
        InspectionCopyBindings bindings(context_.Get(), options_.warp);
        context_->CopyResource(copy.Get(), get<ID3D11Resource>(id));
        // Resource MinLOD is immediate-context state, even for deferred shader work.
        if (desc.MiscFlags & D3D11_RESOURCE_MISC_RESOURCE_CLAMP)
            context_->SetResourceMinLOD(copy.Get(), metadata.minLod);
        context_->ExecuteCommandList(commands.Get(), TRUE);
    }
    context_->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    for (;;) {
        checkCancellation(cancelled);
        const auto status =
            context_->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (status == S_OK)
            break;
        if (status != DXGI_ERROR_WAS_STILL_DRAWING)
            check(status, "Read multisample observations");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Multisample observation timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::memcpy(result.data(), mapped.pData, bytes);
    context_->Unmap(staging.Get(), 0);
    checkCancellation(cancelled);
    return result;
}
} // namespace flora
