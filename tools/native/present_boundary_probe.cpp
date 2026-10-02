// Development-only native DXGI semantics probe. No capture/player dependencies.
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void checked(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("DXGI/D3D11 setup failed: " + std::to_string(result));
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    fs::path output(argv[1]);
    if (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output)))
        return 2;
    fs::create_directories(output);
    std::ofstream report(output / L"present.json");
    try {
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"headless_composition_swapchain\":true,\"observations\":[";
        bool first = true;
        for (bool warp : {false, true}) {
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                      nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            checked(device.As(&dxgi));
            checked(dxgi->GetAdapter(&adapter));
            checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
            for (unsigned scenario = 0; scenario < 10; ++scenario) {
                for (UINT flags : {0u, UINT(DXGI_PRESENT_TEST), UINT(DXGI_PRESENT_DO_NOT_SEQUENCE)}) {
                    DXGI_SWAP_CHAIN_DESC1 desc{};
                    desc.Width = desc.Height = 8;
                    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
                    if (scenario >= 8)
                        desc.BufferUsage |= DXGI_USAGE_UNORDERED_ACCESS;
                    desc.BufferCount = 2;
                    desc.Scaling = DXGI_SCALING_STRETCH;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                    ComPtr<IDXGISwapChain1> chain;
                    checked(factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &chain));
                    ComPtr<ID3D11Texture2D> buffer;
                    ComPtr<ID3D11RenderTargetView> rtv;
                    checked(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
                    checked(device->CreateRenderTargetView(buffer.Get(), nullptr, &rtv));
                    auto target = rtv.Get();
                    ComPtr<ID3D11Texture2D> other;
                    ComPtr<ID3D11RenderTargetView> otherRtv;
                    ComPtr<ID3D11ShaderResourceView> srv;
                    ComPtr<ID3D11UnorderedAccessView> uav, uavBefore, uavAfter;
                    if (scenario == 0)
                        context->OMSetRenderTargets(1, &target, nullptr);
                    if (scenario == 1) {
                        D3D11_TEXTURE2D_DESC td{};
                        buffer->GetDesc(&td);
                        td.MiscFlags = 0;
                        checked(device->CreateTexture2D(&td, nullptr, &other));
                        checked(device->CreateRenderTargetView(other.Get(), nullptr, &otherRtv));
                        ID3D11RenderTargetView *targets[]{otherRtv.Get(), target};
                        context->OMSetRenderTargets(2, targets, nullptr);
                    }
                    using Setter = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(
                        UINT, UINT, ID3D11ShaderResourceView *const *);
                    using Getter = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(
                        UINT, UINT, ID3D11ShaderResourceView **);
                    const Setter setters[]{&ID3D11DeviceContext::VSSetShaderResources,
                                           &ID3D11DeviceContext::HSSetShaderResources,
                                           &ID3D11DeviceContext::DSSetShaderResources,
                                           &ID3D11DeviceContext::GSSetShaderResources,
                                           &ID3D11DeviceContext::PSSetShaderResources,
                                           &ID3D11DeviceContext::CSSetShaderResources};
                    const Getter getters[]{&ID3D11DeviceContext::VSGetShaderResources,
                                           &ID3D11DeviceContext::HSGetShaderResources,
                                           &ID3D11DeviceContext::DSGetShaderResources,
                                           &ID3D11DeviceContext::GSGetShaderResources,
                                           &ID3D11DeviceContext::PSGetShaderResources,
                                           &ID3D11DeviceContext::CSGetShaderResources};
                    if (scenario >= 2 && scenario < 8) {
                        checked(device->CreateShaderResourceView(buffer.Get(), nullptr, &srv));
                        auto view = srv.Get();
                        (context.Get()->*setters[scenario - 2])(3, 1, &view);
                    }
                    if (scenario >= 8) {
                        checked(device->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav));
                        auto view = uav.Get();
                        if (scenario == 8)
                            context->CSSetUnorderedAccessViews(3, 1, &view, nullptr);
                        else
                            context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 3, 1,
                                                                               &view, nullptr);
                    }
                    const float color[]{1, 0, 0, 1};
                    context->ClearRenderTargetView(target, color);
                    ComPtr<ID3D11RenderTargetView> before;
                    context->OMGetRenderTargets(1, &before, nullptr);
                    ComPtr<ID3D11ShaderResourceView> srvBefore, srvAfter;
                    if (scenario >= 2 && scenario < 8)
                        (context.Get()->*getters[scenario - 2])(3, 1, &srvBefore);
                    if (scenario == 8)
                        context->CSGetUnorderedAccessViews(3, 1, &uavBefore);
                    if (scenario == 9)
                        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 3, 1,
                                                                           &uavBefore);
                    auto hr = chain->Present(0, flags);
                    ComPtr<ID3D11RenderTargetView> after;
                    context->OMGetRenderTargets(1, &after, nullptr);
                    if (scenario >= 2 && scenario < 8)
                        (context.Get()->*getters[scenario - 2])(3, 1, &srvAfter);
                    if (scenario == 8)
                        context->CSGetUnorderedAccessViews(3, 1, &uavAfter);
                    if (scenario == 9)
                        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 3, 1,
                                                                           &uavAfter);
                    ID3D11RenderTargetView *targetsAfter[2]{};
                    context->OMGetRenderTargets(2, targetsAfter, nullptr);
                    ComPtr<ID3D11RenderTargetView> slot0, slot1;
                    slot0.Attach(targetsAfter[0]);
                    slot1.Attach(targetsAfter[1]);
                    if (!first)
                        report << ',';
                    first = false;
                    report << "{\"warp\":" << (warp ? "true" : "false") << ",\"flags\":" << flags
                           << ",\"scenario\":" << scenario << ",\"hresult\":" << int32_t(hr)
                           << ",\"rtv_bound_before\":" << (before.Get() == rtv.Get() ? "true" : "false")
                           << ",\"rtv_bound_after\":" << (after ? "true" : "false")
                           << ",\"other_rtv_retained\":"
                           << (otherRtv && slot0.Get() == otherRtv.Get() ? "true" : "false")
                           << ",\"slot1_after\":" << (slot1 ? "true" : "false")
                           << ",\"srv_before\":" << (srvBefore ? "true" : "false")
                           << ",\"srv_after\":" << (srvAfter ? "true" : "false")
                           << ",\"uav_before\":" << (uavBefore ? "true" : "false")
                           << ",\"uav_after\":" << (uavAfter ? "true" : "false") << '}';
                    context->ClearState();
                    context->Flush();
                }
            }
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
