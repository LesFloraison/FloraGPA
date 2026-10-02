// Self-owned development producer. GPA injection is optional and never used by the application.
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
fs::path output;
void checked(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("D3D11/DXGI failure: " + std::to_string(hr));
}
void WINAPI captured(const wchar_t *message) {
    std::wofstream(output / L"capture-message.txt") << (message ? message : L"<null>");
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}
struct Chain {
    HWND window{};
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    ~Chain() {
        if (window)
            DestroyWindow(window);
    }
};
void createChain(Chain &chain, ID3D11Device *device, IDXGIFactory *factory, DXGI_SWAP_EFFECT effect,
                 bool tearing = false) {
    chain.window = CreateWindowW(L"FloraPresentCaptureProbe", L"FloraGPA Present probe", WS_OVERLAPPEDWINDOW,
                                 0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!chain.window)
        throw std::runtime_error("CreateWindow failed");
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = desc.BufferDesc.Height = 8;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = effect >= DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? 2 : 1;
    desc.OutputWindow = chain.window;
    desc.Windowed = TRUE;
    desc.SwapEffect = effect;
    desc.Flags = tearing ? 0x800 : 0;
    checked(factory->CreateSwapChain(device, &desc, &chain.swap));
    checked(chain.swap->GetBuffer(0, IID_PPV_ARGS(&chain.buffer)));
    checked(device->CreateRenderTargetView(chain.buffer.Get(), nullptr, &chain.rtv));
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    // output mode(0=TEST,1=flip sequential,2=discard,3=flip discard,4=tearing) [capture shimloader]
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output)))
        return 2;
    fs::create_directories(output);
    try {
        int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 4)
            throw std::runtime_error("Invalid mode");
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load research shimloader");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("RegisterClass failed");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain main, secondary;
        createChain(main, device.Get(), factory.Get(),
                    mode == 0 ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_DISCARD);
        if (mode)
            createChain(secondary, device.Get(), factory.Get(),
                        mode == 2   ? DXGI_SWAP_EFFECT_DISCARD
                        : mode == 3 ? DXGI_SWAP_EFFECT(4)
                                    : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                        mode == 4);
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC td{};
        main.buffer->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = td.MiscFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked(device->CreateTexture2D(&td, nullptr, &staging));
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"frames\":[";
        bool requested = false;
        unsigned failures = 0;
        for (unsigned frame = 0; frame < 12; ++frame) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            HMODULE shim = GetModuleHandleW(L"shimd3d64.dll");
            if (argc == 5 && frame == 5 && shim) {
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(shim, "CaptureNextFrame"));
                if (!request)
                    throw std::runtime_error("CaptureNextFrame is unavailable");
                request(argv[3], captured);
                requested = true;
            }
            context->ClearState();
            Chain &observed = mode ? secondary : main;
            auto rtv = observed.rtv.Get();
            context->OMSetRenderTargets(1, &rtv, nullptr);
            const float red[]{1, 0, 0, 1}, green[]{0, 1, 0, 1};
            context->ClearRenderTargetView(rtv, red);
            ComPtr<ID3D11RenderTargetView> before, after;
            context->OMGetRenderTargets(1, &before, nullptr);
            auto result = observed.swap->Present(0, mode == 0 ? DXGI_PRESENT_TEST : mode == 4 ? 0x200 : 0);
            context->OMGetRenderTargets(1, &after, nullptr);
            // Subsequent work must be visible in the saved stream, not inferred from frame-tail pixels.
            context->ClearRenderTargetView(main.rtv.Get(), green);
            context->CopyResource(staging.Get(), main.buffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::vector<unsigned char> pixels(256);
            for (unsigned row = 0; row < 8; ++row)
                memcpy(pixels.data() + row * 32, static_cast<char *>(mapped.pData) + row * mapped.RowPitch,
                       32);
            context->Unmap(staging.Get(), 0);
            for (unsigned p = 0; p < 256; ++p)
                if (pixels[p] != ((p % 4 == 1 || p % 4 == 3) ? 255 : 0))
                    ++failures;
            std::ofstream image(output / L"oracle.rgba", std::ios::binary);
            image.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
            auto terminal = main.swap->Present(0, 0);
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"hresult\":" << int32_t(result)
                   << ",\"before\":" << (before.Get() == rtv ? "true" : "false")
                   << ",\"after\":" << (after.Get() == rtv ? "true" : "false")
                   << ",\"terminal_hresult\":" << int32_t(terminal) << '}';
            Sleep(25);
        }
        report << "],\"capture_requested\":" << (requested ? "true" : "false")
               << ",\"pixel_failures\":" << failures << ",\"completed\":true}\n";
        context->ClearState();
        return failures ? 3 : 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
