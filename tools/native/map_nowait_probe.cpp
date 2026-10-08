// Development-only original capture producer; never linked into the replay runtime.
#include "texture_probe_helpers.h"
#include "original_capture_control.h"

int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5) return 2;
    output = argv[1];
    if (fs::exists(output)) return 2;
    fs::create_directories(output);
    try {
        const unsigned mode = unsigned(std::stoul(argv[2]));
        if (mode >= 8) throw std::runtime_error("Invalid Map NOWAIT mode");
        const unsigned dimension = mode % 4;
        const bool readWrite = mode >= 4;
        const unsigned width = dimension == 0 ? 65536 : dimension == 1 ? 8192 : dimension == 2 ? 1025 : 65;
        const unsigned height = dimension == 2 ? 127 : dimension == 3 ? 31 : 1;
        const unsigned depth = dimension == 3 ? 7 : 1;
        const size_t count = size_t(width) * height * depth;
        std::vector<uint8_t> initial(count * 4), expected(count * 4);
        for (size_t i = 0; i < initial.size(); ++i) {
            initial[i] = uint8_t((i * 13 + 17) % 251);
            expected[i] = readWrite ? initial[i] : uint8_t((i * 29 + 73) % 251);
        }
        const std::array<size_t, 4> pixels{0, width - 1, count / 2, count - 1};
        if (readWrite)
            for (unsigned n = 0; n < pixels.size(); ++n)
                for (unsigned channel = 0; channel < 4; ++channel)
                    expected[pixels[n] * 4 + channel] = uint8_t(203 + n * 7 + channel);
        save(output / "before.bin", initial);
        save(output / "expected.bin", expected);
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load development capture shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClass failed");
        const bool warp = GetEnvironmentVariableW(L"FLORA_MAP_NOWAIT_WARP", nullptr, 0) != 0;
        const bool debug = GetEnvironmentVariableW(L"FLORA_MAP_NOWAIT_DEBUG", nullptr, 0) != 0;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi)); checked(dxgi->GetAdapter(&adapter)); checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_ADAPTER_DESC ad{}; checked(adapter->GetDesc(&ad));
        Chain chain; createChain(chain, device.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        auto create = [&](unsigned role) {
            const auto usage = role == 0 ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
            const UINT cpu = role == 0 ? 0 : role == 1 ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : D3D11_CPU_ACCESS_READ;
            D3D11_SUBRESOURCE_DATA data{initial.data(), width * 4, width * height * 4};
            ComPtr<ID3D11Resource> resource;
            if (dimension == 0) {
                D3D11_BUFFER_DESC desc{UINT(initial.size()), usage, 0, cpu, 0, 0};
                ComPtr<ID3D11Buffer> value; checked(device->CreateBuffer(&desc, role == 0 ? &data : nullptr, &value));
                checked(value.As(&resource));
            } else if (dimension == 1) {
                D3D11_TEXTURE1D_DESC desc{width, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, usage, 0, cpu, 0};
                ComPtr<ID3D11Texture1D> value; checked(device->CreateTexture1D(&desc, role == 0 ? &data : nullptr, &value));
                checked(value.As(&resource));
            } else if (dimension == 2) {
                D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1,0}, usage, 0, cpu, 0};
                ComPtr<ID3D11Texture2D> value; checked(device->CreateTexture2D(&desc, role == 0 ? &data : nullptr, &value));
                checked(value.As(&resource));
            } else {
                D3D11_TEXTURE3D_DESC desc{width, height, depth, 1, DXGI_FORMAT_R8G8B8A8_UNORM, usage, 0, cpu, 0};
                ComPtr<ID3D11Texture3D> value; checked(device->CreateTexture3D(&desc, role == 0 ? &data : nullptr, &value));
                checked(value.As(&resource));
            }
            return resource;
        };
        auto source = create(0), target = create(1), readback = create(2);
        auto address = [&](const D3D11_MAPPED_SUBRESOURCE &mapped, size_t pixel) {
            const size_t z = pixel / (size_t(width) * height), y = pixel / width % height, x = pixel % width;
            return static_cast<uint8_t *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch + x * 4;
        };
        auto bytes = [&](const D3D11_MAPPED_SUBRESOURCE &mapped) {
            std::vector<uint8_t> result(initial.size());
            for (unsigned z = 0; z < depth; ++z)
                for (unsigned y = 0; y < height; ++y)
                    std::memcpy(result.data() + (size_t(z) * height + y) * width * 4,
                                address(mapped, (size_t(z) * height + y) * width), width * 4);
            return result;
        };
        std::ofstream report(output / "oracle.json"); report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"dimension\":" << dimension << ",\"read_write\":" << (readWrite ? "true" : "false")
               << ",\"warp\":" << (warp ? "true" : "false") << ",\"debug\":" << (debug ? "true" : "false")
               << ",\"capture_requested\":" << (argc == 5 ? "true" : "false")
               << ",\"width\":" << width << ",\"height\":" << height << ",\"depth\":" << depth
               << ",\"vendor_id\":" << ad.VendorId << ",\"device_id\":" << ad.DeviceId << ",\"frames\":[";
        for (unsigned frame = 0; frame < 12; ++frame) {
            MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {TranslateMessage(&msg); DispatchMessageW(&msg);}
            if (argc == 5 && frame == 5) {
                auto shim = GetModuleHandleW(L"shimd3d64.dll");
                flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                auto request = reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(GetProcAddress(shim,"CaptureNextFrame"));
                if (!request) throw std::runtime_error("CaptureNextFrame unavailable");
                request(argv[3], captured);
            }
            context->ClearState();
            for (unsigned i = 0; i < 64; ++i) context->CopyResource(target.Get(), source.Get());
            context->Flush();
            D3D11_MAPPED_SUBRESOURCE mapped{}; HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
            unsigned busy = 0; const auto deadline = GetTickCount64() + 10000;
            while (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                hr = context->Map(target.Get(), 0, readWrite ? D3D11_MAP_READ_WRITE : D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {++busy; if (GetTickCount64() > deadline) throw std::runtime_error("Writable Map timeout"); Sleep(1);}
            }
            checked(hr);
            if (readWrite && bytes(mapped) != initial) throw std::runtime_error("Read-write Map did not observe preceding copies");
            const UINT row = mapped.RowPitch, slice = mapped.DepthPitch;
            if (readWrite) {
                for (auto pixel : pixels) std::memcpy(address(mapped,pixel), expected.data()+pixel*4,4);
            } else {
                for (unsigned z=0; z<depth; ++z) for (unsigned y=0; y<height; ++y)
                    std::memcpy(address(mapped,(size_t(z)*height+y)*width),expected.data()+(size_t(z)*height+y)*width*4,width*4);
            }
            context->Unmap(target.Get(),0);
            context->CopyResource(readback.Get(),target.Get());
            checked(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
            auto actual = bytes(mapped); context->Unmap(readback.Get(),0);
            save(output/ ("frame"+std::to_string(frame)+".bin"),actual);
            if (actual != expected) throw std::runtime_error("Written resource differs from independent oracle");
            const float green[]{0,1,0,1}; context->ClearRenderTargetView(chain.rtv.Get(),green);
            checked(chain.swap->Present(0,0));
            if (frame) report << ',';
            report << "{\"frame\":" << frame << ",\"busy_polls\":" << busy << ",\"row_pitch\":" << row
                   << ",\"depth_pitch\":" << slice << ",\"bytes_verified\":true}";
            Sleep(25);
        }
        if (debug) {
            ComPtr<ID3D11InfoQueue> queue; checked(device.As(&queue));
            const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter(); std::ofstream messages(output/"debug.txt");
            for (UINT64 i=0;i<count;++i) {SIZE_T size=0; checked(queue->GetMessage(i,nullptr,&size)); std::vector<uint8_t> raw(size);
                auto message=reinterpret_cast<D3D11_MESSAGE *>(raw.data()); checked(queue->GetMessage(i,message,&size)); messages<<message->pDescription<<'\n';}
            if (count) throw std::runtime_error("D3D11 debug messages present");
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {std::ofstream(output/"error.txt")<<error.what(); return 1;}
}
