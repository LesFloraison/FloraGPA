// Standalone repeated-MSAA-copy oracle. No replay, GPA or external capture backend.
#define NOMINMAX
#include <array>
#include <chrono>
#include <cstdint>
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
namespace fs = std::filesystem;
namespace {
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(hr));
}
ComPtr<ID3DBlob> compile(const std::string &text, const char *profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(text.data(), text.size(), "MSAA storage oracle", nullptr, nullptr, "main",
                               profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr, "Compile");
    return code;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 6 ||
        (std::wstring(argv[2]) != L"hardware" && std::wstring(argv[2]) != L"intel" &&
         std::wstring(argv[2]) != L"warp") ||
        (std::wstring(argv[3]) != L"typeless" && std::wstring(argv[3]) != L"typed") ||
        (std::wstring(argv[4]) != L"immediate" && std::wstring(argv[4]) != L"deferred") ||
        (std::wstring(argv[5]) != L"srv" && std::wstring(argv[5]) != L"srv-rt")) {
        std::cerr << "NEW_OUT hardware|intel|warp typeless|typed immediate|deferred srv|srv-rt\n"
                     "Typed controls can reproduce device removal on affected drivers.\n";
        return 2;
    }
    ComPtr<ID3D11InfoQueue> debug;
    try {
        const auto output = fs::absolute(argv[1]);
        if (!fs::create_directory(output))
            throw std::runtime_error("Output directory must be new");
        const bool intel = std::wstring(argv[2]) == L"intel";
        const bool warp = std::wstring(argv[2]) == L"warp";
        const bool typed = std::wstring(argv[3]) == L"typed";
        const bool deferredCopy = std::wstring(argv[4]) == L"deferred";
        const bool preserveOutput = std::wstring(argv[5]) == L"srv-rt";
        ComPtr<IDXGIAdapter1> selected;
        if (intel) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create factory");
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc), "Describe adapter");
                if (desc.VendorId == 0x8086 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("No Intel hardware adapter");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
        check(D3D11CreateDevice(selected.Get(),
                                intel  ? D3D_DRIVER_TYPE_UNKNOWN
                                : warp ? D3D_DRIVER_TYPE_WARP
                                       : D3D_DRIVER_TYPE_HARDWARE,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create device");
        check(device.As(&debug), "Get debug queue");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        check(device.As(&dxgi), "Get DXGI device");
        check(dxgi->GetAdapter(&adapter), "Get actual adapter");
        DXGI_ADAPTER_DESC actual{};
        check(adapter->GetDesc(&actual), "Describe actual adapter");
        std::ofstream configuration(output / "configuration.txt");
        configuration.exceptions(std::ios::badbit | std::ios::failbit);
        configuration << "vendor=" << actual.VendorId << "\ndevice=" << actual.DeviceId << "\nluid="
                      << ((uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) |
                          actual.AdapterLuid.LowPart)
                      << "\nfeature_level=" << level << "\ntyped_source=" << typed
                      << "\ndeferred_copy=" << deferredCopy << "\npreserve_output=" << preserveOutput << '\n';
        configuration.close();
        const auto vertex = compile(
            "float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,0,1);}", "vs_5_0");
        ComPtr<ID3D11VertexShader> vs;
        check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs),
              "Create VS");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = raster.MultisampleEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        check(device->CreateRasterizerState(&raster, &rs), "Create rasterizer");
        std::ofstream report(output / "results.csv");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "samples,quality,repeat,inspection,sample,r,g,b,a\n";
        unsigned reads = 0;
        for (UINT samples : {2u, 4u})
            for (UINT quality : {0u, UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN)}) {
                D3D11_TEXTURE2D_DESC sourceDesc{1,
                                                1,
                                                1,
                                                1,
                                                typed ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                      : DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                                {samples, quality},
                                                D3D11_USAGE_DEFAULT,
                                                D3D11_BIND_RENDER_TARGET,
                                                0,
                                                0};
                ComPtr<ID3D11Texture2D> source;
                check(device->CreateTexture2D(&sourceDesc, nullptr, &source), "Create source");
                D3D11_RENDER_TARGET_VIEW_DESC sourceView{};
                sourceView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                sourceView.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
                ComPtr<ID3D11RenderTargetView> sourceRtv;
                check(device->CreateRenderTargetView(source.Get(), &sourceView, &sourceRtv),
                      "Create source RTV");
                for (UINT repeat = 0; repeat < 3; ++repeat) {
                    context->ClearState();
                    const float scale = float(1u << repeat);
                    const float clear[]{.125f * scale, .5f * scale, .25f * scale, .25f * scale};
                    context->ClearRenderTargetView(sourceRtv.Get(), clear);
                    auto sourceRaw = sourceRtv.Get();
                    context->OMSetRenderTargets(1, &sourceRaw, nullptr);
                    context->Flush();
                    for (UINT inspection = 0; inspection < 2; ++inspection)
                        for (UINT sample = 0; sample < samples; ++sample) {
                            auto desc = sourceDesc;
                            desc.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;
                            desc.BindFlags =
                                D3D11_BIND_SHADER_RESOURCE | (preserveOutput ? D3D11_BIND_RENDER_TARGET : 0);
                            ComPtr<ID3D11Texture2D> readable, target, staging;
                            check(device->CreateTexture2D(&desc, nullptr, &readable), "Create readable");
                            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                            sd.Format = DXGI_FORMAT_R16G16B16A16_UINT;
                            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                            sd.Texture2DMSArray.ArraySize = 1;
                            ComPtr<ID3D11ShaderResourceView> srv;
                            check(device->CreateShaderResourceView(readable.Get(), &sd, &srv), "Create SRV");
                            desc.SampleDesc = {1, 0};
                            desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
                            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                            check(device->CreateTexture2D(&desc, nullptr, &target), "Create output");
                            D3D11_RENDER_TARGET_VIEW_DESC td{};
                            td.Format = desc.Format;
                            td.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                            td.Texture2DArray.ArraySize = 1;
                            ComPtr<ID3D11RenderTargetView> rtv;
                            check(device->CreateRenderTargetView(target.Get(), &td, &rtv),
                                  "Create output RTV");
                            desc.BindFlags = 0;
                            desc.Usage = D3D11_USAGE_STAGING;
                            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            check(device->CreateTexture2D(&desc, nullptr, &staging), "Create staging");
                            const auto pixel =
                                compile("Texture2DMSArray<uint4> t:register(t0);uint4 main(float4 "
                                        "p:SV_Position):SV_Target{return t.Load(int3(0,0,0)," +
                                            std::to_string(sample) + ");}",
                                        "ps_5_0");
                            ComPtr<ID3D11PixelShader> ps;
                            check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
                                                            nullptr, &ps),
                                  "Create PS");
                            ComPtr<ID3D11DeviceContext> dc;
                            check(device->CreateDeferredContext(0, &dc), "Create deferred context");
                            (deferredCopy ? dc.Get() : context.Get())
                                ->CopyResource(readable.Get(), source.Get());
                            dc->VSSetShader(vs.Get(), nullptr, 0);
                            dc->PSSetShader(ps.Get(), nullptr, 0);
                            dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                            dc->RSSetState(rs.Get());
                            const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
                            dc->RSSetViewports(1, &viewport);
                            auto input = srv.Get();
                            auto outputView = rtv.Get();
                            dc->PSSetShaderResources(0, 1, &input);
                            dc->OMSetRenderTargets(1, &outputView, nullptr);
                            dc->Draw(3, 0);
                            ComPtr<ID3D11CommandList> commands;
                            check(dc->FinishCommandList(FALSE, &commands), "Finish commands");
                            context->ExecuteCommandList(commands.Get(), TRUE);
                            context->CopyResource(staging.Get(), target.Get());
                            context->Flush();
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                            for (;;) {
                                const auto hr = context->Map(staging.Get(), 0, D3D11_MAP_READ,
                                                             D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                                if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                                    check(hr, "Map");
                                    break;
                                }
                                check(device->GetDeviceRemovedReason(), "Device status");
                                if (std::chrono::steady_clock::now() >= deadline)
                                    throw std::runtime_error("Readback timeout");
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                            std::array<uint16_t, 4> bits{};
                            std::memcpy(bits.data(), mapped.pData, sizeof(bits));
                            context->Unmap(staging.Get(), 0);
                            report << samples << ',' << quality << ',' << repeat << ',' << inspection << ','
                                   << sample;
                            for (auto value : bits)
                                report << ',' << value;
                            report << std::endl;
                            const std::array<uint16_t, 4> base{0x3000, 0x3800, 0x3400, 0x3400};
                            for (unsigned i = 0; i < 4; ++i)
                                if (bits[i] != base[i] + (repeat << 10))
                                    throw std::runtime_error("Raw half bits differ");
                            ++reads;
                        }
                }
            }
        check(device->GetDeviceRemovedReason(), "Final device status");
        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            check(debug->GetMessage(i, nullptr, &size), "Debug message size");
            std::vector<uint8_t> bytes(size);
            auto message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(debug->GetMessage(i, message, &size), "Debug message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(message->pDescription);
        }
        std::cout << "reads=" << reads << " different_words=0 debug_failures=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
