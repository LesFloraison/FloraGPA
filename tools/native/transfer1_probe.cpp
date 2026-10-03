// Development-only, self-owned Context1 transfer captures with CPU byte oracles.
#include "texture_probe_helpers.h"
#include <d3d11_1.h>
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 25)
            throw std::runtime_error("Invalid mode");
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load development shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("RegisterClass failed");
        ComPtr<ID3D11Device> d;
        ComPtr<ID3D11DeviceContext> c;
        const bool debug = GetEnvironmentVariableW(L"FLORA_PROBE_DEBUG", nullptr, 0) != 0;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  debug ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                  nullptr, &c));
        ComPtr<ID3D11DeviceContext1> c1;
        checked(c.As(&c1));
        D3D11_FEATURE_DATA_D3D11_OPTIONS features{};
        checked(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &features, sizeof features));
        const bool update = mode >= 10 && mode <= 20 || mode == 22 || mode >= 24;
        const bool buffer = mode >= 7 && mode <= 9 || mode >= 14 && mode <= 16 || mode == 22 || mode == 23;
        const bool cb = mode == 15 || mode == 16 || mode == 22;
        const bool same = mode == 4 || mode == 5 || mode == 23;
        const bool empty = mode == 6 || mode == 20;
        UINT flags = mode == 2 || mode == 9 || mode == 12 || mode == 16 ? D3D11_COPY_DISCARD
                     : mode == 3 || mode == 8 || mode == 13 || mode == 15 || mode == 21
                         ? D3D11_COPY_NO_OVERWRITE
                         : 0;
        if (same && !features.CopyWithOverlap)
            throw std::runtime_error("CopyWithOverlap unavailable");
        if (cb && mode == 15 && !features.ConstantBufferPartialUpdate)
            throw std::runtime_error("ConstantBufferPartialUpdate unavailable");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(d.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, d.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        Tex t;
        t.width = t.height = 8;
        t.mips = t.layers = 1;
        if (mode == 17) {
            t.dimension = 3;
            t.depth = 4;
        }
        if (mode == 18) {
            t.dimension = 1;
            t.width = 16;
            t.height = 1;
            t.mips = t.layers = 2;
        }
        if (mode == 19 || mode == 25)
            t.format = DXGI_FORMAT_BC1_UNORM;
        auto initial = storage(t, false), sourceData = initial;
        if (buffer)
            initial = sourceData = {{64, 1, 1, 64, 1, 64, std::vector<uint8_t>(64)}};
        for (size_t sub = 0; sub < initial.size(); ++sub)
            for (size_t i = 0; i < initial[sub].bytes.size(); ++i) {
                initial[sub].bytes[i] = uint8_t((i * 7 + sub * 11 + 17) % 251);
                sourceData[sub].bytes[i] = uint8_t((i * 13 + sub * 19 + 93) % 251);
            }
        ComPtr<ID3D11Resource> src, dst, stage;
        if (buffer) {
            D3D11_BUFFER_DESC bd{64,
                                 D3D11_USAGE_DEFAULT,
                                 UINT(cb ? D3D11_BIND_CONSTANT_BUFFER : D3D11_BIND_VERTEX_BUFFER),
                                 0,
                                 0,
                                 0};
            ComPtr<ID3D11Buffer> b;
            checked(d->CreateBuffer(&bd, nullptr, &b));
            src = b;
            b.Reset();
            checked(d->CreateBuffer(&bd, nullptr, &b));
            dst = b;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            b.Reset();
            checked(d->CreateBuffer(&bd, nullptr, &b));
            stage = b;
        } else {
            src = create(d.Get(), t);
            dst = create(d.Get(), t);
            stage = create(d.Get(), t, true);
        }
        UINT sub = mode == 18 ? 3 : 0, x = 0, y = 0, z = 0;
        D3D11_BOX box{0, 0, 0, buffer ? 64u : initial[sub].width, initial[sub].height, initial[sub].depth};
        bool boxed = false;
        if (mode == 1 || mode == 3 || mode == 4 || mode == 5) {
            box = {0, 0, 0, mode == 5 ? 3u : 6u, 6, 1};
            x = mode == 5 ? 5 : 2;
            y = 2;
            boxed = true;
        }
        if (mode == 8 || mode == 23) {
            box = {0, 0, 0, 32, 1, 1};
            x = 16;
            boxed = true;
        }
        if (mode == 11 || mode == 13 || mode == 17) {
            box = {2, 1, mode == 17 ? 1u : 0u, 7, 6, mode == 17 ? 3u : 1u};
            boxed = true;
        }
        if (mode == 14 || mode == 15) {
            box = {16, 0, 0, 48, 1, 1};
            boxed = true;
        }
        if (mode == 25) {
            box = {4, 4, 0, 8, 8, 1};
            boxed = true;
        }
        if (empty) {
            box = {2, 1, 0, 2, 5, 1};
            boxed = true;
        }
        auto expected = initial;
        const auto &originalSource = same ? initial : sourceData;
        const UINT unit = buffer ? 1u : 4u;
        const bool bc = mode == 19 || mode == 25;
        UINT width = box.right - box.left, rows = box.bottom - box.top, depth = box.back - box.front;
        UINT rowBytes = bc ? ((width + 3) / 4) * 8 : width * unit;
        if (bc)
            rows = (rows + 3) / 4;
        UINT rowPitch = rowBytes + (mode == 11 || mode == 17 || mode == 24 ? 20u : 0u);
        UINT depthPitch = rowPitch * rows + (mode == 17 ? 32u : 0u);
        std::vector<uint8_t> upload(std::max<size_t>(4, size_t(depthPitch) * depth), 0xcc);
        for (UINT dz = 0; dz < depth; ++dz)
            for (UINT dy = 0; dy < rows; ++dy) {
                const auto sourceAt = size_t(update ? dz : box.front + dz) * originalSource[sub].slice +
                                      size_t(update ? dy : box.top + dy) * originalSource[sub].row +
                                      (update ? 0 : box.left * unit);
                if (rowBytes)
                    memcpy(upload.data() + size_t(dz) * depthPitch + size_t(dy) * rowPitch,
                           originalSource[sub].bytes.data() + sourceAt, rowBytes);
                const auto destinationAt =
                    size_t(update ? box.front + dz : z + dz) * expected[sub].slice +
                    size_t(update ? (bc ? box.top / 4 : box.top) + dy : y + dy) * expected[sub].row +
                    (update ? (bc ? box.left / 4 * 2 : box.left) : x) * unit;
                if (rowBytes)
                    memcpy(expected[sub].bytes.data() + destinationAt,
                           originalSource[sub].bytes.data() + sourceAt, rowBytes);
            }
        std::vector<uint8_t> expectedBytes;
        for (const auto &s : expected)
            expectedBytes.insert(expectedBytes.end(), s.bytes.begin(), s.bytes.end());
        save(output / L"expected.bin", expectedBytes);
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        auto screenStage = create(d.Get(), screen, true);
        auto screenStorage = storage(screen, true);
        // Only 8x8 RGBA textures have a direct presentation copy. Other modes
        // have a marker image and must be accepted using resource byte evidence.
        const bool direct = !buffer && t.dimension == 2 && !bc;
        const auto expectedImage = direct ? expected[sub].bytes : screenStorage[0].bytes;
        save(output / L"expected.rgba", expectedImage);
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"flags\":" << flags
               << ",\"copy_with_overlap\":" << features.CopyWithOverlap
               << ",\"constant_buffer_partial_update\":" << features.ConstantBufferPartialUpdate
               << ",\"direct_image\":" << (direct ? "true" : "false") << ",\"frames\":[";
        for (unsigned frame = 0; frame < 12; ++frame) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (argc == 5 && frame == 5) {
                auto shim = GetModuleHandleW(L"shimd3d64.dll");
                auto request =
                    shim ? reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                               GetProcAddress(shim, "CaptureNextFrame"))
                         : nullptr;
                if (!request)
                    throw std::runtime_error("CaptureNextFrame unavailable");
                request(argv[3], captured);
            }
            c->ClearState();
            // Force an explicit context/resource observation in original GPA.
            // The earlier direct-copy-only producer omitted the context resource;
            // those immutable files remain a separate capture-omission control.
            const float seedScreen[]{0, 0, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), seedScreen);
            for (UINT i = 0; i < initial.size(); ++i) {
                c->UpdateSubresource(dst.Get(), i, nullptr, initial[i].bytes.data(), initial[i].row,
                                     initial[i].slice);
                c->UpdateSubresource(src.Get(), i, nullptr, sourceData[i].bytes.data(), sourceData[i].row,
                                     sourceData[i].slice);
            }
            // Complete seeding before a NO_OVERWRITE operation: the promise must
            // not conflict with an earlier outstanding transfer in this producer.
            c->CopyResource(stage.Get(), dst.Get());
            read(c.Get(), stage.Get(), initial);
            if (update)
                c1->UpdateSubresource1(dst.Get(), sub, boxed ? &box : nullptr, upload.data(), rowPitch,
                                       depthPitch, flags);
            else
                c1->CopySubresourceRegion1(dst.Get(), sub, x, y, z, same ? dst.Get() : src.Get(), sub,
                                           boxed ? &box : nullptr, flags);
            c->CopyResource(stage.Get(), dst.Get());
            auto actual = read(c.Get(), stage.Get(), expected);
            save(output / L"actual.bin", actual);
            if (actual != expectedBytes)
                throw std::runtime_error("Transfer resource byte mismatch");
            if (direct)
                c->CopySubresourceRegion(chain.buffer.Get(), 0, 0, 0, 0, dst.Get(), sub, nullptr);
            else {
                const float green[]{0, 1, 0, 1};
                c->ClearRenderTargetView(chain.rtv.Get(), green);
            }
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), screenStage.Get(), screenStorage);
            save(output / L"frame.rgba", rgba);
            if (rgba != expectedImage)
                throw std::runtime_error("Transfer image mismatch");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"resource_verified\":true,\"image_verified\":true}";
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
