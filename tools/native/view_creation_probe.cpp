// Original GPA view-creation fixtures. Development only; never shipped with FloraGPA.
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // 0..3 RTV, 10..13 DSV, 20..23 UAV. Variants: explicit, default, failed, validation-only.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int scenario = std::stoi(argv[2]), kind = scenario / 10, variant = scenario % 10;
        if (scenario < 0 || kind > 2 || variant > 3)
            throw std::runtime_error("Invalid scenario");
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
        t.format = kind == 1   ? DXGI_FORMAT_D32_FLOAT
                   : kind == 2 ? DXGI_FORMAT_R32_UINT
                               : DXGI_FORMAT_R8G8B8A8_UNORM;
        t.bind = kind == 1 ? D3D11_BIND_DEPTH_STENCIL
                           : D3D11_BIND_SHADER_RESOURCE |
                                 (kind == 2 ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
        if (kind == 1 && variant == 1) {
            t.width = t.height = 8;
            t.layers = t.mips = 1;
        }
        auto texture = create(d.Get(), t), stage = create(d.Get(), t, true);
        auto initial = storage(t, false), expected = initial;
        const UINT selectedMip = variant == 1 ? 0 : 1, selectedLayer = variant == 1 ? 0 : 1;
        for (size_t sub = 0; sub < initial.size(); ++sub) {
            const bool selected = variant == 1 ? sub % t.mips == 0 : sub == t.mips + 1;
            for (size_t i = 0; i < initial[sub].bytes.size(); i += 4) {
                if (kind == 1) {
                    float before = 1.f, after = selected ? .25f : 1.f;
                    memcpy(initial[sub].bytes.data() + i, &before, 4);
                    memcpy(expected[sub].bytes.data() + i, &after, 4);
                } else if (selected && kind == 2) {
                    UINT value = 123;
                    memcpy(expected[sub].bytes.data() + i, &value, 4);
                } else if (selected) {
                    expected[sub].bytes[i + 1] = 255;
                    expected[sub].bytes[i + 3] = 255;
                }
            }
        }
        std::vector<uint8_t> expectedBytes;
        for (const auto &s : expected)
            expectedBytes.insert(expectedBytes.end(), s.bytes.begin(), s.bytes.end());
        save(output / L"expected.bin", expectedBytes);
        std::vector<ComPtr<ID3D11DepthStencilView>> initDepth;
        if (kind == 1)
            for (UINT layer = 0; layer < t.layers; ++layer)
                for (UINT mip = 0; mip < t.mips; ++mip) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                    desc.Format = t.format;
                    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray = {mip, layer, 1};
                    ComPtr<ID3D11DepthStencilView> v;
                    checked(d->CreateDepthStencilView(texture.Get(), &desc, &v));
                    initDepth.push_back(v);
                }
        ComPtr<ID3D11ShaderResourceView> srv;
        if (kind != 1) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = t.format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray = {0, t.mips, 0, t.layers};
            checked(d->CreateShaderResourceView(texture.Get(), &sd, &srv));
        }
        std::string shader =
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0.5,1);}";
        if (kind == 1)
            shader += "float4 ps():SV_Target{return float4(0,1,0,1);}";
        else {
            const std::string type = kind == 2 ? "uint" : "float4";
            const std::string predicate = kind == 2 ? "v==123" : "all(v==float4(0,1,0,1))";
            shader += "Texture2DArray<" + type + "> tex:register(t0);float4 ps():SV_Target{" + type +
                      " v=tex.Load(int4(0,0," + std::to_string(selectedLayer) + "," +
                      std::to_string(selectedMip) + "));return " + predicate +
                      "?float4(0,1,0,1):float4(1,0,1,1);}";
        }
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
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = kind == 1;
        dd.DepthFunc = D3D11_COMPARISON_GREATER;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ComPtr<ID3D11DepthStencilState> ds;
        checked(d->CreateDepthStencilState(&dd, &ds));
        Tex screen;
        screen.width = screen.height = 8;
        screen.layers = screen.mips = 1;
        auto screenStage = create(d.Get(), screen, true);
        auto screenStorage = storage(screen, true);
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        DXGI_ADAPTER_DESC ad{};
        checked(adapter->GetDesc(&ad));
        report << "{\"mode\":" << scenario << ",\"vendor_id\":" << ad.VendorId
               << ",\"device_id\":" << ad.DeviceId << ",\"frames\":[";
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
            if (kind == 1)
                for (const auto &v : initDepth)
                    c->ClearDepthStencilView(v.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
            else
                for (UINT sub = 0; sub < initial.size(); ++sub)
                    c->UpdateSubresource(texture.Get(), sub, nullptr, initial[sub].bytes.data(),
                                         initial[sub].row, initial[sub].slice);
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            ComPtr<ID3D11UnorderedAccessView> uav;
            auto make = [&]<class View, class Desc>(auto method, Desc desc, ComPtr<View> &result) {
                if (variant >= 2) {
                    auto bad = desc;
                    if (variant == 2)
                        reinterpret_cast<UINT *>(&bad)[kind == 1 ? 3 : 2] = 99;
                    ComPtr<View> absent;
                    auto hr = (d.Get()->*method)(texture.Get(), &bad,
                                                 variant == 2 ? absent.GetAddressOf() : nullptr);
                    if ((variant == 2 && (SUCCEEDED(hr) || absent)) || (variant == 3 && hr != S_FALSE))
                        throw std::runtime_error("Unexpected failed/validation result");
                }
                checked(
                    (d.Get()->*method)(texture.Get(), variant == 1 ? nullptr : &desc, result.GetAddressOf()));
                Desc actual{};
                result->GetDesc(&actual);
                ComPtr<ID3D11Resource> parent;
                result->GetResource(&parent);
                if (parent.Get() != texture.Get())
                    throw std::runtime_error("View parent differs");
                ComPtr<ID3D11Device> owner;
                result->GetDevice(&owner);
                if (owner.Get() != d.Get())
                    throw std::runtime_error("View device differs");
                ComPtr<IUnknown> unknown;
                checked(result.As(&unknown));
                result->AddRef();
                result->Release();
            };
            if (kind == 0) {
                D3D11_RENDER_TARGET_VIEW_DESC v{};
                v.Format = t.format;
                v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray = {1, 1, 1};
                make(&ID3D11Device::CreateRenderTargetView, v, rtv);
                const float green[]{0, 1, 0, 1};
                c->ClearRenderTargetView(rtv.Get(), green);
            } else if (kind == 1) {
                D3D11_DEPTH_STENCIL_VIEW_DESC v{};
                v.Format = t.format;
                v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray = {1, 1, 1};
                make(&ID3D11Device::CreateDepthStencilView, v, dsv);
                c->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, .25f, 0);
            } else {
                D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
                v.Format = t.format;
                v.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray = {1, 1, 1};
                make(&ID3D11Device::CreateUnorderedAccessView, v, uav);
                const UINT value[]{123, 0, 0, 0};
                c->ClearUnorderedAccessViewUint(uav.Get(), value);
            }
            c->CopyResource(stage.Get(), texture.Get());
            const auto bytes = read(c.Get(), stage.Get(), expected);
            if (bytes != expectedBytes)
                throw std::runtime_error("View write byte mismatch");
            save(output / L"texture.bin", bytes);
            const float magenta[]{1, 0, 1, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), magenta);
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, dsv.Get());
            c->OMSetDepthStencilState(ds.Get(), 0);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            if (srv) {
                auto input = srv.Get();
                c->PSSetShaderResources(0, 1, &input);
            }
            c->Draw(3, 0);
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            const auto rgba = read(c.Get(), screenStage.Get(), screenStorage);
            if (rgba != screenStorage[0].bytes)
                throw std::runtime_error("View dependent image mismatch");
            save(output / L"frame.rgba", rgba);
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"bytes_verified\":true,\"image_verified\":true}";
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
