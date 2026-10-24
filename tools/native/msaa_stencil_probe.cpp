// Independent D3D11 point-load probe: no capture reader, replay, or analysis implementation.
#define NOMINMAX
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("D3D11 operation failed: " + std::to_string(result));
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *profile) {
    ComPtr<ID3DBlob> bytes, errors;
    const auto result =
        D3DCompile(source.data(), source.size(), "Independent MSAA stencil probe", nullptr, nullptr, "main",
                   profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytes, &errors);
    if (FAILED(result) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(result);
    return bytes;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP [RESOURCE_MIN_LOD]\n";
        return 2;
    }
    try {
        const std::filesystem::path directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        const auto vendor = std::stoul(argv[2]);
        const bool clamped = argc == 4;
        const float minLod = clamped ? std::stof(argv[3]) : 0;
        if (!std::isfinite(minLod) || minLod < 0 || minLod > 16)
            throw std::runtime_error("Invalid probe MinLOD");
        ComPtr<IDXGIAdapter1> adapter;
        if (vendor) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto result = factory->EnumAdapters1(i, &candidate);
                if (result == DXGI_ERROR_NOT_FOUND)
                    break;
                check(result);
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
        D3D_FEATURE_LEVEL level{};
        check(D3D11CreateDevice(adapter.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                &level, &context));
        auto vsCode = compile(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actualAdapter;
        check(device.As(&dxgi));
        check(dxgi->GetAdapter(&actualAdapter));
        DXGI_ADAPTER_DESC actual{};
        check(actualAdapter->GetDesc(&actual));
        ComPtr<ID3D11VertexShader> vs;
        check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        context->RSSetViewports(1, &viewport);
        D3D11_TEXTURE2D_DESC targetDesc{
            1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET,
            0, 0};
        ComPtr<ID3D11Texture2D> target, staging;
        check(device->CreateTexture2D(&targetDesc, nullptr, &target));
        targetDesc.Usage = D3D11_USAGE_STAGING;
        targetDesc.BindFlags = 0;
        targetDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&targetDesc, nullptr, &staging));
        ComPtr<ID3D11RenderTargetView> output;
        check(device->CreateRenderTargetView(target.Get(), nullptr, &output));
        D3D11_BUFFER_DESC cbDesc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&cbDesc, nullptr, &constants));
        auto *cb = constants.Get();
        context->PSSetConstantBuffers(0, 1, &cb);
        std::ofstream report(directory / "results.json");
        report << "{\"vendor\":" << vendor << ",\"adapter_vendor\":" << actual.VendorId
               << ",\"adapter_device\":" << actual.DeviceId << ",\"adapter_luid\":"
               << ((uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart)
               << ",\"resource_clamp\":" << (clamped ? "true" : "false")
               << ",\"requested_min_lod\":" << minLod << ",\"records\":[";
        bool first = true;
        for (UINT format : {22u, 47u})
            for (bool array : {false, true}) {
                D3D11_TEXTURE2D_DESC desc{4,
                                          4,
                                          1,
                                          array ? 2u : 1u,
                                          DXGI_FORMAT(format == 22 ? 19 : 44),
                                          {4, 0},
                                          D3D11_USAGE_DEFAULT,
                                          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL,
                                          0,
                                          clamped ? D3D11_RESOURCE_MISC_RESOURCE_CLAMP : 0u};
                ComPtr<ID3D11Texture2D> source;
                check(device->CreateTexture2D(&desc, nullptr, &source));
                D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc{};
                depthDesc.Format = DXGI_FORMAT(format == 22 ? 20 : 45);
                depthDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                depthDesc.Texture2DMSArray = {0, desc.ArraySize};
                ComPtr<ID3D11DepthStencilView> depth;
                check(device->CreateDepthStencilView(source.Get(), &depthDesc, &depth));
                context->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .25f,
                                               90);
                if (clamped)
                    context->SetResourceMinLOD(source.Get(), minLod);
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = DXGI_FORMAT(format);
                view.ViewDimension =
                    array ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                if (array)
                    view.Texture2DMSArray = {0, 2};
                ComPtr<ID3D11ShaderResourceView> srv;
                check(device->CreateShaderResourceView(source.Get(), &view, &srv));
                const auto psCode =
                    compile(std::string(array ? "Texture2DMSArray<uint4>" : "Texture2DMS<uint4>") +
                                " src:register(t0);cbuffer Address:register(b0){uint4 p;}uint4 "
                                "main():SV_Target{return src.Load(" +
                                (array ? "int3(p.xyz)" : "int2(p.xy)") + ",p.w);}",
                            "ps_5_0");
                const auto stem = std::to_string(format) + (array ? "-array" : "-2d");
                std::ofstream binary(directory / (stem + ".dxbc"), std::ios::binary);
                binary.write(static_cast<const char *>(psCode->GetBufferPointer()), psCode->GetBufferSize());
                ComPtr<ID3D11PixelShader> ps;
                check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr,
                                                &ps));
                context->PSSetShader(ps.Get(), nullptr, 0);
                auto *input = srv.Get();
                context->PSSetShaderResources(0, 1, &input);
                for (UINT sample : {0u, 3u})
                    for (auto p : std::vector<std::array<UINT, 4>>{{1, 2, 0, sample},
                                                                   {4, 0, 0, sample},
                                                                   {UINT32_MAX, 0, 0, sample},
                                                                   {0, 4, 0, sample},
                                                                   {0, 0, 2, sample}}) {
                        context->UpdateSubresource(constants.Get(), 0, nullptr, p.data(), 0, 0);
                        auto *rtv = output.Get();
                        context->OMSetRenderTargets(1, &rtv, nullptr);
                        context->Draw(3, 0);
                        context->OMSetRenderTargets(0, nullptr, nullptr);
                        context->CopyResource(staging.Get(), target.Get());
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
                                throw std::runtime_error("Probe readback timed out");
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        std::array<UINT, 4> bits{};
                        std::memcpy(bits.data(), mapped.pData, 16);
                        context->Unmap(staging.Get(), 0);
                        if (!first)
                            report << ',';
                        first = false;
                        report << "{\"format\":" << format << ",\"array\":" << (array ? "true" : "false")
                               << ",\"address\":[" << p[0] << ',' << p[1] << ',' << p[2] << ',' << p[3]
                               << "],\"bits\":[" << bits[0] << ',' << bits[1] << ',' << bits[2] << ','
                               << bits[3] << "]}";
                    }
            }
        ComPtr<ID3D11InfoQueue> queue;
        check(device.As(&queue));
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size));
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size));
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        report << "],\"validation_errors\":0}\n";
        if (!report)
            throw std::runtime_error("Cannot write probe results");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
