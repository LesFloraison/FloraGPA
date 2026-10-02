// Development-only native DXGI semantics probe. No capture/player dependencies.
#define NOMINMAX
#include <Windows.h>
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
            for (UINT flags : {0u, UINT(DXGI_PRESENT_TEST), UINT(DXGI_PRESENT_DO_NOT_SEQUENCE)}) {
                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = desc.Height = 8;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
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
                context->OMSetRenderTargets(1, &target, nullptr);
                const float color[]{1, 0, 0, 1};
                context->ClearRenderTargetView(target, color);
                ComPtr<ID3D11RenderTargetView> before;
                context->OMGetRenderTargets(1, &before, nullptr);
                auto hr = chain->Present(0, flags);
                ComPtr<ID3D11RenderTargetView> after;
                context->OMGetRenderTargets(1, &after, nullptr);
                if (!first)
                    report << ',';
                first = false;
                report << "{\"warp\":" << (warp ? "true" : "false") << ",\"flags\":" << flags
                       << ",\"hresult\":" << int32_t(hr)
                       << ",\"rtv_bound_before\":" << (before.Get() == rtv.Get() ? "true" : "false")
                       << ",\"rtv_bound_after\":" << (after ? "true" : "false")
                       << '}';
                context->ClearState();
                context->Flush();
            }
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
