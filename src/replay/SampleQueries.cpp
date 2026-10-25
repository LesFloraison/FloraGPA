#include "Replay.h"
#include "Unpredicated.h"
#include <chrono>
#include <d3dcompiler.h>
#include <thread>

namespace flora {
NativeSampleQueries Replay::observeSampleQueries(ID3D11ShaderResourceView *srv, const CancelCheck &cancelled,
                                                 uint32_t timeoutMs) {
    checkCancellation(cancelled);
    if (!timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("Invalid sample query deadline");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    NativeSampleQueries result;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    Com<ID3D11Resource> resource;
    bool depthOnly = false, hasOutput = false;
    DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
    D3D11_RASTERIZER_DESC1 raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (srv) {
        srv->GetDesc(&view);
        if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DMS &&
            view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY)
            throw std::invalid_argument("Sample queries require an MS shader resource view");
        Com<ID3D11Device> owner;
        srv->GetDevice(&owner);
        if (owner.Get() != device_.Get())
            throw std::invalid_argument("Sample query view belongs to another device");
        srv->GetResource(&resource);
    } else {
        std::array<ID3D11RenderTargetView *, 8> raw{};
        Com<ID3D11DepthStencilView> depth;
        context_->OMGetRenderTargets(UINT(raw.size()), raw.data(), &depth);
        for (auto *value : raw) {
            Com<ID3D11RenderTargetView> target;
            target.Attach(value);
            if (target && !resource) {
                target->GetResource(&resource);
                D3D11_RENDER_TARGET_VIEW_DESC d{};
                target->GetDesc(&d);
                format = d.Format;
            }
        }
        if (!resource && depth) {
            depth->GetResource(&resource);
            D3D11_DEPTH_STENCIL_VIEW_DESC d{};
            depth->GetDesc(&d);
            format = d.Format;
            depthOnly = true;
        }
        hasOutput = bool(resource);
        Com<ID3D11RasterizerState> original;
        context_->RSGetState(&original);
        if (original) {
            D3D11_RASTERIZER_DESC d{};
            original->GetDesc(&d);
            raster.MultisampleEnable = d.MultisampleEnable;
            Com<ID3D11RasterizerState1> newer;
            if (SUCCEEDED(original.As(&newer))) {
                D3D11_RASTERIZER_DESC1 d1{};
                newer->GetDesc1(&d1);
                raster.ForcedSampleCount = d1.ForcedSampleCount;
            }
        }
        result.forcedSamples = raster.ForcedSampleCount;
        result.multisampleEnabled = raster.MultisampleEnable != FALSE;
    }
    // Independent probes found order-dependent no-target driver results. API
    // bindings alone cannot yet reconstruct that state; do not invent a target.
    if (!srv && !hasOutput)
        return result;
    Com<ID3D11Texture2D> originalTexture;
    if (resource && SUCCEEDED(resource.As(&originalTexture))) {
        D3D11_TEXTURE2D_DESC d{};
        originalTexture->GetDesc(&d);
        result.resourceSamples = d.SampleDesc.Count;
        result.quality = d.SampleDesc.Quality;
    }
    Com<ID3D11Texture2D> targetTexture;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv;
    if (!srv && hasOutput) {
        D3D11_TEXTURE2D_DESC d{1,
                               1,
                               1,
                               1,
                               format,
                               {result.resourceSamples, result.quality},
                               D3D11_USAGE_DEFAULT,
                               UINT(depthOnly ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET),
                               0,
                               0};
        check(device_->CreateTexture2D(&d, nullptr, &targetTexture), "Create private sample query target");
        if (depthOnly)
            check(device_->CreateDepthStencilView(targetTexture.Get(), nullptr, &dsv),
                  "Create private sample query depth view");
        else
            check(device_->CreateRenderTargetView(targetTexture.Get(), nullptr, &rtv),
                  "Create private sample query target view");
    }
    const std::string declaration = !srv ? ""
                                    : view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS
                                        ? "Texture2DMS<float4> src:register(t0);"
                                        : "Texture2DMSArray<float4> src:register(t0);";
    const std::string query = !srv ? "n=GetRenderTargetSampleCount();"
                              : view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS
                                  ? "src.GetDimensions(w,h,n);"
                                  : "src.GetDimensions(w,h,l,n);";
    const std::string position = srv ? "src.GetSamplePosition(i)" : "GetRenderTargetSamplePosition(i)";
    auto compile = [&](const std::string &source, const char *profile) {
        Com<ID3DBlob> binary, errors;
        const auto hr = D3DCompile(source.data(), source.size(), "FloraGPA sample queries", nullptr, nullptr,
                                   "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
        if (FAILED(hr) && errors)
            throw std::runtime_error(
                std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
        check(hr, "Compile sample queries");
        return binary;
    };
    const auto vertex = compile("float4 main(uint i:SV_VertexID):SV_Position{"
                                "return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                                "vs_5_0");
    const auto pixel = compile(declaration +
                                   "RWStructuredBuffer<uint4> dst:register(u1);"
                                   "void main(){uint w,h,l,n;" +
                                   query +
                                   "dst[33]=uint4(n,0,0,0);[unroll]for(uint i=0;i<33;++i)"
                                   "dst[i]=uint4(asuint(" +
                                   position + "),0,0);}",
                               "ps_5_0");
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs),
          "Create sample query vertex shader");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps),
          "Create sample query pixel shader");
    D3D11_BUFFER_DESC bd{
        34 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        16};
    Com<ID3D11Buffer> output, staging;
    Com<ID3D11UnorderedAccessView> uav;
    check(device_->CreateBuffer(&bd, nullptr, &output), "Create sample query output");
    check(device_->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create sample query output view");
    bd = {34 * 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    check(device_->CreateBuffer(&bd, nullptr, &staging), "Create sample query staging");
    Com<ID3D11RasterizerState> rs;
    if (raster.ForcedSampleCount) {
        Com<ID3D11Device1> newer;
        check(device_.As(&newer), "Read forced sample query device");
        Com<ID3D11RasterizerState1> state;
        check(newer->CreateRasterizerState1(&raster, &state), "Create forced sample query rasterizer");
        rs = state;
    } else {
        D3D11_RASTERIZER_DESC d{};
        std::memcpy(&d, &raster, sizeof d);
        check(device_->CreateRasterizerState(&d, &rs), "Create sample query rasterizer");
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                  D3D11_COMPARISON_ALWAYS};
    Com<ID3D11DepthStencilState> ds;
    check(device_->CreateDepthStencilState(&dd, &ds), "Create sample query depth state");
    Com<ID3D11DeviceContext> deferred;
    check(device_->CreateDeferredContext(0, &deferred), "Create sample query context");
    deferred->VSSetShader(vs.Get(), nullptr, 0);
    deferred->PSSetShader(ps.Get(), nullptr, 0);
    deferred->PSSetShaderResources(0, 1, &srv);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->RSSetState(rs.Get());
    const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    deferred->RSSetViewports(1, &viewport);
    deferred->OMSetDepthStencilState(ds.Get(), 0);
    auto *target = rtv.Get();
    auto *unordered = uav.Get();
    deferred->OMSetRenderTargetsAndUnorderedAccessViews(target ? 1 : 0, target ? &target : nullptr, dsv.Get(),
                                                        1, 1, &unordered, nullptr);
    const UINT sentinel[]{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    deferred->ClearUnorderedAccessViewUint(uav.Get(), sentinel);
    deferred->Draw(3, 0);
    deferred->ClearState();
    deferred->CopyResource(staging.Get(), output.Get());
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish sample queries");
    checkCancellation(cancelled);
    if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Sample queries timed out before submission");
    {
        Unpredicated unpredicated(context_.Get());
        PredicateIsolation predicates(*this);
        context_->ExecuteCommandList(commands.Get(), TRUE);
    }
    context_->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    for (;;) {
        checkCancellation(cancelled);
        const auto hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == S_OK)
            break;
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
            check(hr, "Read sample queries");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Sample queries timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::array<std::array<uint32_t, 4>, 34> values;
    std::memcpy(values.data(), mapped.pData, sizeof values);
    context_->Unmap(staging.Get(), 0);
    result.count = values[33][0];
    if (!result.count || result.count > 32 || (srv && result.count != result.resourceSamples) ||
        values[33][1] || values[33][2] || values[33][3])
        throw std::runtime_error("Invalid native sample query count");
    for (uint32_t index = 0; index < 33; ++index)
        if (values[index][2] || values[index][3] ||
            (index >= result.count && (values[index][0] || values[index][1])))
            throw std::runtime_error("Invalid native sample query position");
    result.positions.assign(values.begin(), values.begin() + result.count);
    result.observed = true;
    checkCancellation(cancelled);
    if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Sample queries timed out after readback");
    return result;
}
} // namespace flora
