// Development-only originals: input clones preserve resource LOD and original storage.
#include "original_capture_control.h"
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 11)
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
        const bool warp = GetEnvironmentVariableW(L"FLORA_MINLOD_WARP", nullptr, 0) != 0;
        checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                  nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(d.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_ADAPTER_DESC adapterDesc{};
        checked(adapter->GetDesc(&adapterDesc));
        Chain chain;
        createChain(chain, d.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        Tex t;
        t.dimension = 1 + mode / 4;
        t.width = 8;
        t.height = t.dimension == 1 ? 1 : 8;
        t.depth = t.dimension == 3 ? 8 : 1;
        t.mips = 4;
        t.layers = t.dimension == 3 ? 1 : 2;
        const float lods[]{0, .75f, 1, 4};
        const float lod = lods[mode % 4];
        auto subs = storage(t, false);
        const std::array<std::array<uint8_t, 4>, 4> colors{
            {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}}};
        std::vector<D3D11_SUBRESOURCE_DATA> initial;
        for (size_t i = 0; i < subs.size(); ++i) {
            for (size_t p = 0; p < subs[i].bytes.size(); p += 4)
                memcpy(subs[i].bytes.data() + p, colors[i % 4].data(), 4);
            initial.push_back({subs[i].bytes.data(), subs[i].row, subs[i].slice});
        }
        auto makeTexture = [&](const std::vector<D3D11_SUBRESOURCE_DATA> &initial) {
            ComPtr<ID3D11Resource> result;
            if (t.dimension == 1) {
                D3D11_TEXTURE1D_DESC td{t.width,
                                        t.mips,
                                        t.layers,
                                        t.format,
                                        D3D11_USAGE_DEFAULT,
                                        t.bind,
                                        0,
                                        D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
                ComPtr<ID3D11Texture1D> r;
                checked(d->CreateTexture1D(&td, initial.data(), &r));
                result = r;
            } else if (t.dimension == 3) {
                D3D11_TEXTURE3D_DESC td{t.width, t.height, t.depth,
                                        t.mips,  t.format, D3D11_USAGE_DEFAULT,
                                        t.bind,  0,        D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
                ComPtr<ID3D11Texture3D> r;
                checked(d->CreateTexture3D(&td, initial.data(), &r));
                result = r;
            } else {
                D3D11_TEXTURE2D_DESC td{t.width,
                                        t.height,
                                        t.mips,
                                        t.layers,
                                        t.format,
                                        {1, 0},
                                        D3D11_USAGE_DEFAULT,
                                        t.bind,
                                        0,
                                        D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
                ComPtr<ID3D11Texture2D> r;
                checked(d->CreateTexture2D(&td, initial.data(), &r));
                result = r;
            }
            return result;
        };
        auto texture = makeTexture(initial);
        std::array<ComPtr<ID3D11Resource>, 5> clones;
        std::array<ComPtr<ID3D11ShaderResourceView>, 5> cloneViews;
        std::array<std::vector<uint8_t>, 5> cloneStorage;
        const std::array<uint8_t, 4> magenta{255, 0, 255, 255}, cyan{0, 255, 255, 255};
        for (unsigned v = 0; v < 5; ++v) {
            auto copied = subs;
            if (v >= 1 && v <= 3) {
                auto &patch = copied[v - 1].bytes;
                const auto color = v == 2 ? cyan : magenta;
                for (size_t p = 0; p < patch.size(); p += 4)
                    memcpy(patch.data() + p, color.data(), 4);
            }
            std::vector<D3D11_SUBRESOURCE_DATA> init;
            for (const auto &sub : copied) {
                init.push_back({sub.bytes.data(), sub.row, sub.slice});
                cloneStorage[v].insert(cloneStorage[v].end(), sub.bytes.begin(), sub.bytes.end());
            }
            clones[v] = makeTexture(init);
            checked(d->CreateShaderResourceView(clones[v].Get(), nullptr, &cloneViews[v]));
            save(output / (L"expected-clone-" + std::to_wstring(v) + L".bin"), cloneStorage[v]);
        }
        auto storageStage = create(d.Get(), t, true);
        std::vector<uint8_t> expectedStorage;
        for (const auto &sub : subs)
            expectedStorage.insert(expectedStorage.end(), sub.bytes.begin(), sub.bytes.end());
        save(output / L"expected-storage.bin", expectedStorage);
        ComPtr<ID3D11ShaderResourceView> srv;
        checked(d->CreateShaderResourceView(texture.Get(), nullptr, &srv));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        checked(d->CreateSamplerState(&sd, &sampler));
        const std::string types[]{"Texture1DArray", "Texture2DArray", "Texture3D"};
        const std::string coords[]{"float2(.5,0)", "float3(.5,.5,0)", "float3(.5,.5,.5)"};
        const std::string shader =
            types[t.dimension - 1] +
            "<float4> tex:register(t0);SamplerState sam:register(s0);"
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}"
            "float4 ps():SV_Target{return tex.Sample(sam," +
            coords[t.dimension - 1] + ");}";
        ComPtr<ID3DBlob> vsCode, psCode, errors;
        checked(D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0,
                           &vsCode, &errors));
        errors.Reset();
        checked(D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                           &psCode, &errors));
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        checked(d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        checked(d->CreateRasterizerState(&rd, &rs));
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        auto stage = create(d.Get(), screen, true);
        auto pixels = storage(screen, false);
        const std::array<uint8_t, 4> empty{};
        const auto color = lod == 4 ? empty : colors[lod == 0 ? 0 : 1];
        for (size_t p = 0; p < pixels[0].bytes.size(); p += 4)
            memcpy(pixels[0].bytes.data() + p, color.data(), 4);
        save(output / L"expected.rgba", pixels[0].bytes);
        std::array<std::vector<uint8_t>, 5> cloneImages;
        for (unsigned v = 0; v < 5; ++v) {
            auto expected = color;
            if (v == 1 && lod == 0)
                expected = magenta;
            if (v == 2 && (lod == .75f || lod == 1))
                expected = cyan;
            if (v == 4)
                expected = colors[0]; // Deliberately reset the clone to LOD 0.
            cloneImages[v].resize(8 * 8 * 4);
            for (size_t p = 0; p < cloneImages[v].size(); p += 4)
                memcpy(cloneImages[v].data() + p, expected.data(), 4);
            save(output / (L"expected-clone-" + std::to_wstring(v) + L".rgba"), cloneImages[v]);
        }
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"dimension\":" << t.dimension << ",\"lod\":" << lod
               << ",\"warp\":" << (warp ? "true" : "false") << ",\"vendor_id\":" << adapterDesc.VendorId
               << ",\"device_id\":" << adapterDesc.DeviceId
               << ",\"capture_requested\":" << (argc == 5 ? "true" : "false") << ",\"frames\":[";
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
                flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                request(argv[3], captured);
            }
            c->ClearState();
            const float black[]{0, 0, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), black);
            c->SetResourceMinLOD(texture.Get(), lod);
            const auto observed = c->GetResourceMinLOD(texture.Get());
            if (observed != lod)
                throw std::runtime_error("Resource LOD observation mismatch");
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            auto input = srv.Get();
            auto sam = sampler.Get();
            c->PSSetShaderResources(0, 1, &input);
            c->PSSetSamplers(0, 1, &sam);
            for (unsigned v = 0; v < 5; ++v) {
                c->SetResourceMinLOD(clones[v].Get(), v == 4 ? 0 : lod);
                auto input = cloneViews[v].Get();
                c->PSSetShaderResources(0, 1, &input);
                c->Draw(3, 0);
                c->CopyResource(stage.Get(), chain.buffer.Get());
                auto image = read(c.Get(), stage.Get(), pixels);
                save(output / (L"clone-" + std::to_wstring(v) + L".rgba"), image);
                c->CopyResource(storageStage.Get(), clones[v].Get());
                auto data = read(c.Get(), storageStage.Get(), subs);
                save(output / (L"clone-" + std::to_wstring(v) + L".bin"), data);
                if (image != cloneImages[v] || data != cloneStorage[v] ||
                    c->GetResourceMinLOD(clones[v].Get()) != (v == 4 ? 0 : lod) ||
                    c->GetResourceMinLOD(texture.Get()) != lod)
                    throw std::runtime_error("Clone image, storage or LOD mismatch");
            }
            c->PSSetShaderResources(0, 1, &input);
            c->Draw(3, 0);
            c->CopyResource(stage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), stage.Get(), pixels);
            save(output / L"frame.rgba", rgba);
            if (rgba != pixels[0].bytes)
                throw std::runtime_error("Resource LOD image mismatch");
            c->CopyResource(storageStage.Get(), texture.Get());
            auto packed = read(c.Get(), storageStage.Get(), subs);
            save(output / L"storage.bin", packed);
            if (packed != expectedStorage || c->GetResourceMinLOD(texture.Get()) != lod)
                throw std::runtime_error("Full storage copy changed data or resource LOD");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"observed_lod\":" << observed
                   << ",\"image_verified\":true,\"storage_verified\":true,\"lod_preserved\":true,\"clone_"
                      "checks\":5}";
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
