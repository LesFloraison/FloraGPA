// Development-only original captures: resource LOD affects actual sampled pixels.
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
        if (mode < 0 || mode > 5)
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
        t.width = t.height = 8;
        t.mips = 4;
        t.layers = 1;
        auto subs = storage(t, false);
        const std::array<std::array<uint8_t, 4>, 4> colors{
            {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}}};
        std::vector<D3D11_SUBRESOURCE_DATA> initial;
        for (size_t i = 0; i < subs.size(); ++i) {
            for (size_t p = 0; p < subs[i].bytes.size(); p += 4)
                memcpy(subs[i].bytes.data() + p, colors[i].data(), 4);
            initial.push_back({subs[i].bytes.data(), subs[i].row, subs[i].slice});
        }
        D3D11_TEXTURE2D_DESC td{8,
                                8,
                                4,
                                1,
                                DXGI_FORMAT_R8G8B8A8_UNORM,
                                {1, 0},
                                D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE,
                                0,
                                D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
        ComPtr<ID3D11Texture2D> texture;
        checked(d->CreateTexture2D(&td, initial.data(), &texture));
        ComPtr<ID3D11ShaderResourceView> srv;
        checked(d->CreateShaderResourceView(texture.Get(), nullptr, &srv));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        checked(d->CreateSamplerState(&sd, &sampler));
        const std::string shader =
            "Texture2D<float4> tex:register(t0);SamplerState sam:register(s0);"
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}"
            "float4 ps():SV_Target{return tex.Sample(sam,float2(0.5,0.5));}";
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
        const unsigned mip = mode == 2 ? 2 : mode == 1 || mode >= 4 ? 1 : 0;
        for (size_t p = 0; p < pixels[0].bytes.size(); p += 4)
            memcpy(pixels[0].bytes.data() + p, colors[mip].data(), 4);
        save(output / L"expected.rgba", pixels[0].bytes);
        // Mode 5 deliberately has no setter inside the captured frame.
        if (mode == 5)
            c->SetResourceMinLOD(texture.Get(), 1);
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"expected_lod\":" << mip << ",\"frames\":[";
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
            const float black[]{0, 0, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), black);
            if (mode != 5) {
                c->SetResourceMinLOD(texture.Get(), mode == 3 ? 2.0f : float(mip));
                if (mode == 3)
                    c->SetResourceMinLOD(texture.Get(), 0);
            }
            if (mode == 4)
                c->ClearState();
            const auto observed = c->GetResourceMinLOD(texture.Get());
            if (observed != float(mip))
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
            c->Draw(3, 0);
            c->CopyResource(stage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), stage.Get(), pixels);
            save(output / L"frame.rgba", rgba);
            if (rgba != pixels[0].bytes)
                throw std::runtime_error("Resource LOD image mismatch");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"observed_lod\":" << observed
                   << ",\"image_verified\":true}";
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
