// Development producer: original, unmodified GPA captures plus independent byte/image oracles.
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // mode: 0=upload, 1=initial, 2=initial+overwrite, 3=default SRV, 4=failed, 5=validation only,
    // 6=immediate overwrite, 7=overlapping source rows, 8=overlapping source slices (3D).
    // Add 10 for 1D, 30 for 3D.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int scenario = std::stoi(argv[2]);
        const int dimension = scenario >= 10 ? scenario / 10 : 2, mode = scenario % 10;
        if (scenario < 0 || mode > 8 || (scenario >= 10 && dimension != 1 && dimension != 3) ||
            (mode == 7 && dimension == 1) || (mode == 8 && dimension != 3))
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
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &d, nullptr, &c));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(d.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, d.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        Tex t;
        t.dimension = UINT(dimension);
        if (dimension == 1)
            t.height = 1;
        if (dimension == 3) {
            t.layers = 1;
            t.height = 8;
            t.depth = 4;
        }
        auto initial = storage(t, true), final = initial;
        // Every array/mip has different contents; padded caller pitches exercise shim packing.
        for (size_t i = 0; i < initial.size(); ++i)
            for (size_t j = 0; j < initial[i].bytes.size(); j += 4) {
                initial[i].bytes[j] = uint8_t(17 + i * 29);
                initial[i].bytes[j + 1] = uint8_t(43 + i * 11);
                initial[i].bytes[j + 2] = uint8_t(67 + i * 7 + (j / initial[i].slice) * 13);
                if (mode >= 7) {
                    initial[i].bytes[j] += uint8_t((j / 4) % initial[i].width);
                    initial[i].bytes[j + 1] += uint8_t(((j / initial[i].row) % initial[i].rows) * 3);
                }
            }
        auto flatten = [](const auto &subs) {
            std::vector<uint8_t> bytes;
            for (const auto &s : subs)
                bytes.insert(bytes.end(), s.bytes.begin(), s.bytes.end());
            return bytes;
        };
        std::vector<std::vector<uint8_t>> padded(initial.size());
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for (size_t i = 0; i < initial.size(); ++i) {
            auto &s = initial[i];
            const UINT pitch = mode == 7 ? s.row / 2 : s.row + 16;
            const UINT slice = mode == 8 ? pitch : pitch * s.rows + 32;
            padded[i].resize(std::max(size_t(slice) * s.depth,
                                      size_t(slice) * (s.depth - 1) + size_t(pitch) * (s.rows - 1) + s.row),
                             0xcd);
            for (UINT z = 0; z < s.depth; ++z)
                for (UINT y = 0; y < s.rows; ++y)
                    memcpy(padded[i].data() + size_t(z) * slice + size_t(y) * pitch,
                           s.bytes.data() + size_t(z) * s.slice + size_t(y) * s.row, s.row);
            // Gather the expected GPU bytes independently from the caller memory offsets.
            // Overlapping rows/slices are legal source layouts; padding is not texture data.
            for (UINT z = 0; z < s.depth; ++z)
                for (UINT y = 0; y < s.rows; ++y)
                    memcpy(s.bytes.data() + size_t(z) * s.slice + size_t(y) * s.row,
                           padded[i].data() + size_t(z) * slice + size_t(y) * pitch, s.row);
            // D3D11 ignores both pitch observations for Texture1D.
            data.push_back({padded[i].data(), dimension == 1 ? 0 : pitch, dimension == 1 ? 0 : slice});
        }
        final = initial;
        if (mode == 2 || mode == 6)
            for (auto &sub : final)
                for (size_t j = 0; j < sub.bytes.size(); j += 4)
                    sub.bytes[j] += 5;
        const auto initialBytes = flatten(initial), finalBytes = flatten(final);
        save(output / L"initial.bin", initialBytes);
        save(output / L"expected.bin", finalBytes);
        auto staging = create(d.Get(), t, true);
        Tex screen;
        screen.width = screen.height = 8;
        screen.layers = screen.mips = 1;
        auto screenStage = create(d.Get(), screen, true);
        const auto screenStorage = storage(screen, true);
        const auto red = final[0].bytes[0], green = final[0].bytes[1], blue = final[0].bytes[2];
        const std::string textureType = dimension == 1   ? "Texture1DArray"
                                        : dimension == 3 ? "Texture3D"
                                                         : "Texture2DArray";
        const std::string coordinate = dimension == 1 ? "int3(0,0,0)" : "int4(0,0,0,0)";
        std::string shader = textureType +
                             "<float4> tex:register(t0);float4 vs(uint i:SV_VertexID):SV_Position{return "
                             "float4(i==2?3:-1,i==1?3:-1,0,1);}float4 ps():SV_Target{uint4 "
                             "v=(uint4)round(tex.Load(" +
                             coordinate + ")*255);return all(v==uint4(" + std::to_string(red) + "," +
                             std::to_string(green) + "," + std::to_string(blue) +
                             ",255))?float4(0,1,0,1):float4(1,0,1,1);}";
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
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        DXGI_ADAPTER_DESC adapterDesc{};
        checked(adapter->GetDesc(&adapterDesc));
        report << "{\"mode\":" << scenario << ",\"dimension\":" << dimension
               << ",\"vendor_id\":" << adapterDesc.VendorId << ",\"device_id\":" << adapterDesc.DeviceId
               << ",\"feature_level\":" << UINT(d->GetFeatureLevel()) << ",\"frames\":[";
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
            if (mode == 4 || mode == 5) {
                HRESULT hr{};
                bool returned = false;
                if (dimension == 1) {
                    D3D11_TEXTURE1D_DESC desc{mode == 4 ? 0u : t.width, t.mips, t.layers, t.format,
                                              D3D11_USAGE_DEFAULT,      t.bind, 0,        0};
                    ComPtr<ID3D11Texture1D> absent;
                    hr = d->CreateTexture1D(&desc, nullptr, mode == 4 ? absent.GetAddressOf() : nullptr);
                    returned = bool(absent);
                } else if (dimension == 3) {
                    D3D11_TEXTURE3D_DESC desc{mode == 4 ? 0u : t.width, t.height, t.depth, t.mips, t.format,
                                              D3D11_USAGE_DEFAULT,      t.bind,   0,       0};
                    ComPtr<ID3D11Texture3D> absent;
                    hr = d->CreateTexture3D(&desc, nullptr, mode == 4 ? absent.GetAddressOf() : nullptr);
                    returned = bool(absent);
                } else {
                    D3D11_TEXTURE2D_DESC desc{mode == 4 ? 0u : 16u,
                                              16,
                                              2,
                                              2,
                                              DXGI_FORMAT_R8G8B8A8_UNORM,
                                              {1, 0},
                                              D3D11_USAGE_DEFAULT,
                                              D3D11_BIND_SHADER_RESOURCE,
                                              0,
                                              0};
                    ComPtr<ID3D11Texture2D> absent;
                    hr = d->CreateTexture2D(&desc, nullptr, mode == 4 ? absent.GetAddressOf() : nullptr);
                    returned = bool(absent);
                }
                if ((mode == 4 && (SUCCEEDED(hr) || returned)) || (mode == 5 && hr != S_FALSE))
                    throw std::runtime_error("Unexpected failed/validation result");
            }
            auto texture =
                create(d.Get(), t, false, (mode == 1 || mode == 2 || mode >= 6) ? data.data() : nullptr);
            if (mode == 1 || mode == 2 || mode >= 7) {
                c->CopyResource(staging.Get(), texture.Get());
                if (read(c.Get(), staging.Get(), initial) != initialBytes)
                    throw std::runtime_error("Creation bytes mismatch");
            }
            if (mode != 1 && mode < 7)
                for (UINT sub = 0; sub < final.size(); ++sub)
                    c->UpdateSubresource(texture.Get(), sub, nullptr, final[sub].bytes.data(), final[sub].row,
                                         final[sub].slice);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = t.format;
            if (dimension == 1) {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
                sd.Texture1DArray = {0, t.mips, 0, t.layers};
            } else if (dimension == 3) {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                sd.Texture3D = {0, t.mips};
            } else {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray = {0, t.mips, 0, t.layers};
            }
            ComPtr<ID3D11ShaderResourceView> srv;
            checked(d->CreateShaderResourceView(texture.Get(), mode == 3 ? nullptr : &sd, &srv));
            c->CopyResource(staging.Get(), texture.Get());
            const auto actual = read(c.Get(), staging.Get(), final);
            if (actual != finalBytes)
                throw std::runtime_error("Final texture bytes mismatch");
            save(output / L"texture.bin", actual);
            const float magenta[]{1, 0, 1, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), magenta);
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            auto input = srv.Get();
            c->PSSetShaderResources(0, 1, &input);
            c->Draw(3, 0);
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            const auto rgba = read(c.Get(), screenStage.Get(), screenStorage);
            if (rgba != screenStorage[0].bytes)
                throw std::runtime_error("Texture dependent image mismatch");
            save(output / L"frame.rgba", rgba);
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"bytes_verified\":true,\"image_verified\":true}";
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
