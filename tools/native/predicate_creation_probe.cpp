// Original GPA predicate creation fixtures; development only.
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
        if (mode < 0 || mode > 9)
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
        auto compile = [&](const std::string &src, const char *profile) {
            ComPtr<ID3DBlob> b, error;
            checked(D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile, 0, 0, &b,
                               &error));
            return b;
        };
        auto vb = compile(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        auto pb = compile("float4 main():SV_Target{return float4(0,1,0,1);}", "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        checked(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        checked(d->CreateRasterizerState(&rd, &rs));
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        auto staging = create(d.Get(), screen, true);
        auto expected = storage(screen, true)[0].bytes;
        if (mode == 2 || mode == 3)
            for (size_t i = 0; i < expected.size(); i += 4) {
                expected[i] = expected[i + 2] = 255;
                expected[i + 1] = 0;
            }
        save(output / L"expected.rgba", expected);
        DXGI_ADAPTER_DESC ad{};
        checked(adapter->GetDesc(&ad));
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"vendor_id\":" << ad.VendorId << ",\"device_id\":" << ad.DeviceId
               << ",\"frames\":[";
        for (unsigned frame = 0; frame < 12; ++frame) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (argc == 5 && frame == 5) {
                auto shim = GetModuleHandleW(L"shimd3d64.dll");
                auto req =
                    shim ? reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                               GetProcAddress(shim, "CaptureNextFrame"))
                         : nullptr;
                if (!req)
                    throw std::runtime_error("CaptureNextFrame unavailable");
                req(argv[3], captured);
            }
            c->ClearState();
            D3D11_QUERY_DESC desc{mode == 8 ? D3D11_QUERY_SO_OVERFLOW_PREDICATE
                                            : D3D11_QUERY_OCCLUSION_PREDICATE,
                                  mode == 4 ? D3D11_QUERY_MISC_PREDICATEHINT : 0u};
            HRESULT observation = S_OK;
            ComPtr<ID3D11Predicate> predicate, unused;
            if (mode == 5) {
                D3D11_QUERY_DESC invalid{D3D11_QUERY_EVENT, 0};
                observation = d->CreatePredicate(&invalid, &unused);
                if (SUCCEEDED(observation) || unused)
                    throw std::runtime_error("Invalid predicate unexpectedly created");
            }
            if (mode == 6) {
                observation = d->CreatePredicate(&desc, nullptr);
                if (observation != S_FALSE)
                    throw std::runtime_error("Unexpected validation-only result");
            }
            if (mode == 7)
                checked(d->CreatePredicate(&desc, &unused));
            checked(d->CreatePredicate(&desc, &predicate));
            D3D11_QUERY_DESC actual{};
            predicate->GetDesc(&actual);
            auto size = predicate->GetDataSize();
            ComPtr<ID3D11Predicate> identity;
            checked(predicate.As(&identity));
            if (actual.Query != desc.Query || actual.MiscFlags != desc.MiscFlags || size != sizeof(BOOL) ||
                identity.Get() != predicate.Get())
                throw std::runtime_error("Predicate metadata mismatch");
            identity.Reset();
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            const bool visible = mode != 0 && mode != 3 && mode != 8;
            auto interval = [&](bool draw) {
                c->Begin(predicate.Get());
                c->Draw(draw ? 3 : 0, 0);
                c->End(predicate.Get());
            };
            interval(visible);
            if (mode == 9)
                interval(false);
            const bool queryValue = mode == 9 ? false : visible;
            BOOL measured = FALSE;
            if (mode != 4) {
                auto until = GetTickCount64() + 5000;
                HRESULT hr;
                while ((hr = c->GetData(predicate.Get(), &measured, sizeof measured, 0)) == S_FALSE) {
                    if (GetTickCount64() > until)
                        throw std::runtime_error("Predicate timeout");
                    Sleep(1);
                }
                checked(hr);
                if (bool(measured) != queryValue)
                    throw std::runtime_error("Predicate GPU result differs from expected");
            }
            const float magenta[]{1, 0, 1, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), magenta);
            BOOL comparison = (mode == 2 || mode == 3) ? BOOL(queryValue) : BOOL(!queryValue);
            c->SetPredication(predicate.Get(), comparison);
            c->Draw(3, 0);
            c->SetPredication(nullptr, FALSE);
            c->CopyResource(staging.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), staging.Get(), storage(screen, true));
            if (rgba != expected)
                throw std::runtime_error("Predicate image mismatch");
            save(output / L"frame.rgba", rgba);
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame
                   << ",\"image_verified\":true,\"query_value\":" << (queryValue ? "true" : "false")
                   << ",\"query_read\":" << (mode == 4 ? "false" : "true") << ",\"comparison\":" << comparison
                   << ",\"observation_hresult\":" << observation << '}';
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
