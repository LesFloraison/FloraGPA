// Development producer: original, unmodified GPA captures plus independent byte/image oracles.
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // mode: 0=upload, 1=initial, 2=initial+overwrite, 3=default SRV, 4=failed, 5=validation only,
    // 6=initial+overwrite without any intervening texture read.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 6)
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
        auto initial = storage(t, true), final = initial;
        // Every array/mip has different contents; padded caller pitches exercise shim packing.
        for (size_t i = 0; i < initial.size(); ++i)
            for (size_t j = 0; j < initial[i].bytes.size(); j += 4) {
                initial[i].bytes[j] = uint8_t(17 + i * 29);
                initial[i].bytes[j + 1] = uint8_t(43 + i * 11);
                initial[i].bytes[j + 2] = uint8_t(67 + i * 7);
            }
        final = initial;
        if (mode == 2 || mode == 6)
            for (auto &sub : final)
                for (size_t j = 0; j < sub.bytes.size(); j += 4)
                    sub.bytes[j] += 5;
        auto flatten = [](const auto &subs) {
            std::vector<uint8_t> bytes;
            for (const auto &s : subs)
                bytes.insert(bytes.end(), s.bytes.begin(), s.bytes.end());
            return bytes;
        };
        const auto initialBytes = flatten(initial), finalBytes = flatten(final);
        save(output / L"initial.bin", initialBytes);
        save(output / L"expected.bin", finalBytes);
        std::vector<std::vector<uint8_t>> padded(initial.size());
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for (size_t i = 0; i < initial.size(); ++i) {
            const auto &s = initial[i];
            const UINT pitch = s.row + 16;
            padded[i].resize(size_t(pitch) * s.rows, 0xcd);
            for (UINT y = 0; y < s.rows; ++y)
                memcpy(padded[i].data() + size_t(y) * pitch, s.bytes.data() + size_t(y) * s.row, s.row);
            data.push_back({padded[i].data(), pitch, pitch * s.rows});
        }
        auto staging = create(d.Get(), t, true);
        Tex screen;
        screen.width = screen.height = 8;
        screen.layers = screen.mips = 1;
        auto screenStage = create(d.Get(), screen, true);
        const auto screenStorage = storage(screen, true);
        const auto red = final[0].bytes[0], green = final[0].bytes[1], blue = final[0].bytes[2];
        std::string shader =
            "Texture2DArray<float4> tex:register(t0);float4 vs(uint i:SV_VertexID):SV_Position{return "
            "float4(i==2?3:-1,i==1?3:-1,0,1);}float4 ps():SV_Target{uint4 "
            "v=(uint4)round(tex.Load(int4(0,0,0,0))*255);return all(v==uint4(" +
            std::to_string(red) + "," + std::to_string(green) + "," + std::to_string(blue) +
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
        report << "{\"mode\":" << mode << ",\"frames\":[";
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
                auto hr = d->CreateTexture2D(&desc, nullptr, mode == 4 ? absent.GetAddressOf() : nullptr);
                if ((mode == 4 && (SUCCEEDED(hr) || absent)) || (mode == 5 && hr != S_FALSE))
                    throw std::runtime_error("Unexpected failed/validation result");
            }
            auto texture =
                create(d.Get(), t, false, (mode == 1 || mode == 2 || mode == 6) ? data.data() : nullptr);
            if (mode == 1 || mode == 2) {
                c->CopyResource(staging.Get(), texture.Get());
                if (read(c.Get(), staging.Get(), initial) != initialBytes)
                    throw std::runtime_error("Creation bytes mismatch");
            }
            if (mode != 1)
                for (UINT sub = 0; sub < final.size(); ++sub)
                    c->UpdateSubresource(texture.Get(), sub, nullptr, final[sub].bytes.data(), final[sub].row,
                                         final[sub].slice);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = t.format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray = {0, 2, 0, 2};
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
