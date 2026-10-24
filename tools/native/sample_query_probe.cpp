// Independent sample-query observations. No capture, replay, or private vendor modules.
#define NOMINMAX
#include <array>
#include <chrono>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("D3D11 operation failed: " + std::to_string(hr));
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *profile) {
    ComPtr<ID3DBlob> binary, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "Independent sample queries", nullptr, nullptr,
                               "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr);
    return binary;
}
void save(const std::filesystem::path &path, ID3DBlob *blob) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize());
    if (!file)
        throw std::runtime_error("Cannot save probe shader");
}
void validate(ID3D11InfoQueue *queue) {
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        check(queue->GetMessage(i, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP [MULTISAMPLE_ENABLE_0_OR_1]\n";
        return 2;
    }
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        const auto vendor = std::stoul(argv[2]);
        const auto multisample = argc == 4 ? std::stoul(argv[3]) : 0;
        if (multisample > 1)
            throw std::runtime_error("Invalid rasterizer multisample setting");
        ComPtr<IDXGIAdapter1> adapter;
        if (vendor) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(index, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr);
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc));
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    adapter = candidate;
                    break;
                }
            }
            if (!adapter)
                throw std::runtime_error("Requested adapter is unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature{};
        check(D3D11CreateDevice(adapter.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                &feature, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actualAdapter;
        check(device.As(&dxgi));
        check(dxgi->GetAdapter(&actualAdapter));
        DXGI_ADAPTER_DESC actual{};
        check(actualAdapter->GetDesc(&actual));
        Json report{{"schema", 1},
                    {"scope", "independent_sample_queries"},
                    {"vendor", vendor},
                    {"adapter_vendor", actual.VendorId},
                    {"adapter_device", actual.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                    {"feature_level", feature},
                    {"cases", Json::array()}};
        report["multisample_enable"] = bool(multisample);
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_BACK;
        rasterDesc.DepthClipEnable = TRUE;
        rasterDesc.MultisampleEnable = BOOL(multisample);
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rasterDesc, &raster));
        ComPtr<ID3D11InfoQueue> queue;
        check(device.As(&queue));
        const auto vsCode = compile(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        save(directory / "vertex.dxbc", vsCode.Get());
        ComPtr<ID3D11VertexShader> vs;
        check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        std::array<ComPtr<ID3D11PixelShader>, 3> shaders;
        for (unsigned mode = 0; mode < 3; ++mode) {
            const std::string resource = mode == 2   ? ""
                                         : mode == 1 ? "Texture2DMSArray<float> src:register(t0);"
                                                     : "Texture2DMS<float> src:register(t0);";
            const std::string query =
                mode == 2   ? "n=GetRenderTargetSampleCount();float2 xy=GetRenderTargetSamplePosition(p.x);"
                : mode == 1 ? "src.GetDimensions(w,h,l,n);float2 xy=src.GetSamplePosition(p.x);"
                            : "src.GetDimensions(w,h,n);float2 xy=src.GetSamplePosition(p.x);";
            const std::string load = mode == 2   ? "uint value=0;"
                                     : mode == 1 ? "uint value=p.x<n?asuint(src.Load(int3(0,0,0),p.x)):0;"
                                                 : "uint value=p.x<n?asuint(src.Load(int2(0,0),p.x)):0;";
            const auto code =
                compile(resource +
                            "cbuffer Params:register(b0){uint4 p;}"
                            "RWStructuredBuffer<uint4> dst:register(u1);void main(){uint w,h,l,n;" +
                            query + load + "dst[0]=uint4(n,asuint(xy),value);}",
                        "ps_5_0");
            save(directory / ("query-" + std::to_string(mode) + ".dxbc"), code.Get());
            ComPtr<ID3DBlob> assembly;
            check(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &assembly));
            save(directory / ("query-" + std::to_string(mode) + ".asm.txt"), assembly.Get());
            check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                            &shaders[mode]));
        }
        D3D11_BUFFER_DESC bd{
            16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            16};
        ComPtr<ID3D11Buffer> output, staging, constants;
        check(device->CreateBuffer(&bd, nullptr, &output));
        bd = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        check(device->CreateBuffer(&bd, nullptr, &staging));
        bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(device->CreateBuffer(&bd, nullptr, &constants));
        ComPtr<ID3D11UnorderedAccessView> uav;
        check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
        for (UINT count : {1u, 2u, 4u, 8u, 16u})
            for (UINT quality : {0u, UINT32_MAX, UINT32_MAX - 1}) {
                UINT levels{};
                check(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32_FLOAT, count, &levels));
                for (unsigned mode = 0; mode < 6; ++mode) {
                    const auto shaderMode = mode % 3;
                    const bool unbound = mode >= 3;
                    Json item{{"count", count},     {"quality", quality}, {"quality_levels", levels},
                              {"mode", shaderMode}, {"unbound", unbound}, {"records", Json::array()}};
                    if (!levels) {
                        item["status"] = "unsupported_sample_count";
                        report["cases"].push_back(item);
                        continue;
                    }
                    context->ClearState();
                    D3D11_TEXTURE2D_DESC td{1,
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_R32_FLOAT,
                                            {count, quality},
                                            D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_RENDER_TARGET,
                                            0,
                                            0};
                    ComPtr<ID3D11Texture2D> target, texture;
                    auto hr = device->CreateTexture2D(&td, nullptr, &target);
                    if (FAILED(hr)) {
                        item["status"] = "target_creation_failed";
                        item["hresult"] = uint32_t(hr);
                        report["cases"].push_back(item);
                        queue->ClearStoredMessages();
                        continue;
                    }
                    ComPtr<ID3D11RenderTargetView> rtv;
                    check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
                    ComPtr<ID3D11ShaderResourceView> srv;
                    if (shaderMode != 2 && !unbound) {
                        td.ArraySize = shaderMode == 1 ? 3 : 1;
                        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                        check(device->CreateTexture2D(&td, nullptr, &texture));
                        ComPtr<ID3D11RenderTargetView> initializer;
                        check(device->CreateRenderTargetView(texture.Get(), nullptr, &initializer));
                        const float color[]{.25f, 0, 0, 0};
                        context->ClearRenderTargetView(initializer.Get(), color);
                        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                        vd.Format = td.Format;
                        vd.ViewDimension = shaderMode == 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                           : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                        if (shaderMode == 1)
                            vd.Texture2DMSArray = {1, 2};
                        hr = device->CreateShaderResourceView(texture.Get(), &vd, &srv);
                        if (FAILED(hr)) {
                            item["status"] = "srv_creation_failed";
                            item["hresult"] = uint32_t(hr);
                            report["cases"].push_back(item);
                            queue->ClearStoredMessages();
                            continue;
                        }
                    }
                    context->VSSetShader(vs.Get(), nullptr, 0);
                    context->RSSetState(raster.Get());
                    context->PSSetShader(shaders[shaderMode].Get(), nullptr, 0);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
                    context->RSSetViewports(1, &viewport);
                    auto *cb = constants.Get();
                    auto *view = srv.Get();
                    auto *renderTarget = rtv.Get();
                    auto *unordered = uav.Get();
                    context->PSSetConstantBuffers(0, 1, &cb);
                    context->PSSetShaderResources(0, 1, &view);
                    const bool noTarget = shaderMode == 2 && unbound;
                    context->OMSetRenderTargetsAndUnorderedAccessViews(noTarget ? 0 : 1,
                                                                       noTarget ? nullptr : &renderTarget,
                                                                       nullptr, 1, 1, &unordered, nullptr);
                    for (UINT index = 0; index <= count + 1; ++index) {
                        const auto sample = index == count + 1 ? UINT32_MAX : index;
                        const std::array<UINT, 4> p{sample, 0, 0, 0};
                        context->UpdateSubresource(constants.Get(), 0, nullptr, p.data(), 0, 0);
                        const UINT sentinel[]{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
                        context->ClearUnorderedAccessViewUint(uav.Get(), sentinel);
                        context->Draw(3, 0);
                        context->CopyResource(staging.Get(), output.Get());
                        context->Flush();
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        for (;;) {
                            const auto status = context->Map(staging.Get(), 0, D3D11_MAP_READ,
                                                             D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                            if (status == S_OK)
                                break;
                            if (status != DXGI_ERROR_WAS_STILL_DRAWING)
                                check(status);
                            if (std::chrono::steady_clock::now() >= deadline)
                                throw std::runtime_error("Sample query readback timed out");
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        std::array<UINT, 4> bits{};
                        std::memcpy(bits.data(), mapped.pData, 16);
                        context->Unmap(staging.Get(), 0);
                        item["records"].push_back({{"sample", sample}, {"bits", bits}});
                        validate(queue.Get());
                    }
                    item["status"] = "observed";
                    report["cases"].push_back(item);
                }
            }
        report["validation_errors"] = 0;
        std::ofstream file(directory / "results.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot write sample query report");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
