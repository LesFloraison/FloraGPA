// Self-owned ClearView producer. Captures are original GPA output, never rewritten.
#include "texture_probe_helpers.h"
#include <d3d11_1.h>
#include <limits>
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 15)
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
        ComPtr<ID3D11DeviceContext1> c1;
        checked(c.As(&c1));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(d.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, d.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        const bool buffer = mode == 7 || mode == 8, depth = mode == 6;
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        checked(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options));
        if (!options.ClearView)
            throw std::runtime_error("ClearView feature unavailable");
        D3D11_FEATURE_DATA_D3D11_OPTIONS1 options1{};
        checked(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options1, sizeof options1));
        if (depth && !options1.ClearViewAlsoSupportsDepthOnlyFormats)
            throw std::runtime_error("Depth-only ClearView feature unavailable");
        const bool array = mode >= 4 && mode <= 6, oneD = mode == 10, msaa = mode == 9;
        const bool separate = buffer || depth || array || oneD || msaa;
        Tex t;
        t.width = t.height = array ? 16 : 8;
        t.mips = array ? 2 : 1;
        t.layers = array ? 3 : oneD ? 2 : 1;
        t.dimension = oneD ? 1 : 2;
        if (oneD)
            t.height = 1;
        t.samples = msaa ? 4 : 1;
        t.format = depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
        t.bind = depth       ? D3D11_BIND_DEPTH_STENCIL
                 : mode == 5 ? D3D11_BIND_UNORDERED_ACCESS
                             : D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Resource> target;
        ComPtr<ID3D11View> view;
        std::vector<ComPtr<ID3D11View>> seeds;
        if (buffer) {
            D3D11_BUFFER_DESC bd{48,
                                 D3D11_USAGE_DEFAULT,
                                 UINT(mode == 7 ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_UNORDERED_ACCESS),
                                 0,
                                 0,
                                 0};
            ComPtr<ID3D11Buffer> b;
            checked(d->CreateBuffer(&bd, nullptr, &b));
            target = b;
            for (bool full : {true, false}) {
                if (mode == 7) {
                    D3D11_RENDER_TARGET_VIEW_DESC vd{};
                    vd.Format = DXGI_FORMAT_R32_FLOAT;
                    vd.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
                    vd.Buffer = {full ? 0u : 2u, full ? 12u : 8u};
                    ComPtr<ID3D11RenderTargetView> v;
                    checked(d->CreateRenderTargetView(b.Get(), &vd, &v));
                    if (full)
                        seeds.push_back(v);
                    else
                        view = v;
                } else {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
                    vd.Format = DXGI_FORMAT_R32_UINT;
                    vd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                    vd.Buffer = {full ? 0u : 2u, full ? 12u : 8u, 0};
                    ComPtr<ID3D11UnorderedAccessView> v;
                    checked(d->CreateUnorderedAccessView(b.Get(), &vd, &v));
                    if (full)
                        seeds.push_back(v);
                    else
                        view = v;
                }
            }
        } else if (separate) {
            target = create(d.Get(), t);
            for (UINT index = 0; index <= t.mips; index++) {
                bool full = index < t.mips;
                UINT mip = full ? index : array ? 1 : 0;
                UINT first = full ? 0 : array ? 1 : 0, count = full ? t.layers : array ? 2 : t.layers;
                if (depth) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
                    vd.Format = DXGI_FORMAT_D32_FLOAT;
                    vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                    vd.Texture2DArray = {mip, first, count};
                    ComPtr<ID3D11DepthStencilView> v;
                    checked(d->CreateDepthStencilView(target.Get(), &vd, &v));
                    if (full)
                        seeds.push_back(v);
                    else
                        view = v;
                } else if (mode == 5) {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
                    vd.Format = t.format;
                    vd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                    vd.Texture2DArray = {mip, first, count};
                    ComPtr<ID3D11UnorderedAccessView> v;
                    checked(d->CreateUnorderedAccessView(target.Get(), &vd, &v));
                    if (full)
                        seeds.push_back(v);
                    else
                        view = v;
                } else {
                    D3D11_RENDER_TARGET_VIEW_DESC vd{};
                    vd.Format = t.format;
                    vd.ViewDimension = msaa   ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                       : oneD ? D3D11_RTV_DIMENSION_TEXTURE1DARRAY
                                              : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    if (!msaa)
                        vd.Texture2DArray = {mip, first, count};
                    ComPtr<ID3D11RenderTargetView> v;
                    checked(d->CreateRenderTargetView(target.Get(), &vd, &v));
                    if (full)
                        seeds.push_back(v);
                    else
                        view = v;
                }
            }
        } else {
            target = chain.buffer;
            view = chain.rtv;
            seeds.push_back(chain.rtv);
        }
        ComPtr<ID3D11Resource> staging;
        if (buffer) {
            D3D11_BUFFER_DESC bd{48, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            ComPtr<ID3D11Buffer> b;
            checked(d->CreateBuffer(&bd, nullptr, &b));
            staging = b;
        } else {
            auto readDesc = t;
            readDesc.samples = 1;
            staging = create(d.Get(), readDesc, true);
        }
        ComPtr<ID3D11Resource> resolved;
        if (msaa) {
            auto rt = t;
            rt.samples = 1;
            resolved = create(d.Get(), rt);
        }
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        auto screenStage = create(d.Get(), screen, true);
        std::vector<D3D11_RECT> rects;
        if (mode != 0 && mode != 11 && mode != 13) {
            rects.push_back(buffer ? D3D11_RECT{1, 0, 6, 1}
                            : oneD ? D3D11_RECT{2, 0, 6, 1}
                                   : D3D11_RECT{2, 1, 6, 7});
            if (mode == 2)
                rects = {{0, 0, 4, 4}, {2, 2, 6, 6}};
            if (mode == 3)
                rects = {{2, 1, 2, 7}, {1, 4, 6, 4}};
            if (mode == 12)
                rects = {{-2, -1, 4, 5}, {4, 5, 12, 12}};
        }
        float color[4]{0, 1, 0, 1};
        if (depth)
            color[0] = .25f;
        if (mode == 7)
            color[0] = 1.25f;
        if (mode == 8)
            color[0] = 235.9f;
        if (mode == 11) {
            color[0] = -std::numeric_limits<float>::infinity();
            color[1] = std::numeric_limits<float>::infinity();
            color[2] = std::numeric_limits<float>::quiet_NaN();
        }
        auto subs = storage(t, false);
        std::vector<uint8_t> expected;
        auto word = [&](std::vector<uint8_t> &bytes, size_t at, auto value) {
            memcpy(bytes.data() + at, &value, 4);
        };
        if (buffer) {
            expected.resize(48);
            for (unsigned i = 0; i < 12; i++) {
                bool hit = i >= 3 && i < 8;
                if (mode == 7)
                    word(expected, i * 4, hit ? 1.25f : 7.f);
                else
                    word(expected, i * 4, hit ? 235u : 9u);
            }
        } else {
            for (UINT s = 0; s < subs.size(); s++) {
                auto &sub = subs[s];
                const UINT mip = s % t.mips, layer = s / t.mips;
                for (UINT y = 0; y < sub.height; y++)
                    for (UINT x = 0; x < sub.width; x++) {
                        bool selected = !array || (mip == 1 && layer >= 1);
                        bool hit = rects.empty();
                        for (auto r : rects)
                            hit |=
                                int(x) >= r.left && int(x) < r.right && int(y) >= r.top && int(y) < r.bottom;
                        hit = hit && selected && mode != 14;
                        size_t at = (size_t(y) * sub.width + x) * 4;
                        if (depth)
                            word(sub.bytes, at, hit ? .25f : .75f);
                        else {
                            sub.bytes[at] = sub.bytes[at + 2] = hit ? 0 : 255;
                            sub.bytes[at + 1] = hit ? 255 : 0;
                            sub.bytes[at + 3] = 255;
                        }
                    }
                expected.insert(expected.end(), sub.bytes.begin(), sub.bytes.end());
            }
        }
        auto image = storage(screen, true)[0].bytes;
        if (!buffer && !depth && !oneD)
            image = subs[array ? 5 : 0].bytes;
        save(output / L"expected.bin", expected);
        save(output / L"expected.rgba", image);
        ComPtr<ID3D11Predicate> predicate;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        if (mode >= 14) {
            D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
            checked(d->CreatePredicate(&q, &predicate));
            auto shader = [&](const char *code, const char *profile) {
                ComPtr<ID3DBlob> b, e;
                checked(
                    D3DCompile(code, strlen(code), nullptr, nullptr, nullptr, "main", profile, 0, 0, &b, &e));
                return b;
            };
            auto v =
                shader("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                       "vs_5_0");
            auto p = shader("float4 main():SV_Target{return float4(0,1,0,1);}", "ps_5_0");
            checked(d->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs));
            checked(d->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps));
        }
        DXGI_ADAPTER_DESC ad{};
        checked(adapter->GetDesc(&ad));
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"vendor_id\":" << ad.VendorId << ",\"device_id\":" << ad.DeviceId
               << ",\"frames\":[";
        for (unsigned frame = 0; frame < 12; frame++) {
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
            if (predicate) {
                auto rt = chain.rtv.Get();
                c->OMSetRenderTargets(1, &rt, nullptr);
                D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
                c->RSSetViewports(1, &vp);
                c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c->VSSetShader(vs.Get(), nullptr, 0);
                c->PSSetShader(ps.Get(), nullptr, 0);
                c->Begin(predicate.Get());
                c->Draw(3, 0);
                c->End(predicate.Get());
                BOOL value = FALSE;
                auto deadline = GetTickCount64() + 5000;
                HRESULT hr;
                while ((hr = c->GetData(predicate.Get(), &value, 4, 0)) == S_FALSE) {
                    if (GetTickCount64() > deadline)
                        throw std::runtime_error("Predicate timeout");
                    Sleep(1);
                }
                checked(hr);
                if (!value)
                    throw std::runtime_error("Predicate unexpectedly false");
            }
            const float magenta[]{1, 0, 1, 1}, green[]{0, 1, 0, 1}, seven[]{7, 7, 7, 7};
            const UINT nine[]{9, 9, 9, 9};
            c->ClearRenderTargetView(chain.rtv.Get(), green);
            for (auto &seed : seeds) {
                ComPtr<ID3D11DepthStencilView> ds;
                ComPtr<ID3D11UnorderedAccessView> ua;
                ComPtr<ID3D11RenderTargetView> rt;
                if (depth) {
                    checked(seed.As(&ds));
                    c->ClearDepthStencilView(ds.Get(), D3D11_CLEAR_DEPTH, .75f, 0);
                } else if (mode == 5 || mode == 8) {
                    checked(seed.As(&ua));
                    if (mode == 8)
                        c->ClearUnorderedAccessViewUint(ua.Get(), nine);
                    else
                        c->ClearUnorderedAccessViewFloat(ua.Get(), magenta);
                } else {
                    checked(seed.As(&rt));
                    c->ClearRenderTargetView(rt.Get(), buffer ? seven : magenta);
                }
            }
            if (predicate)
                c->SetPredication(predicate.Get(), mode == 14);
            D3D11_RECT ignored{1, 1, 2, 2};
            c1->ClearView(view.Get(), color,
                          mode == 13      ? &ignored
                          : rects.empty() ? nullptr
                                          : rects.data(),
                          UINT(rects.size()));
            if (predicate)
                c->SetPredication(nullptr, FALSE);
            c->OMSetRenderTargets(0, nullptr, nullptr);
            if (msaa) {
                c->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, t.format);
                c->CopyResource(staging.Get(), resolved.Get());
            } else
                c->CopyResource(staging.Get(), target.Get());
            std::vector<uint8_t> actual;
            if (buffer) {
                D3D11_MAPPED_SUBRESOURCE m{};
                checked(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
                actual.assign(static_cast<uint8_t *>(m.pData), static_cast<uint8_t *>(m.pData) + 48);
                c->Unmap(staging.Get(), 0);
            } else
                actual = read(c.Get(), staging.Get(), subs);
            save(output / L"actual.bin", actual);
            if (actual != expected)
                throw std::runtime_error("ClearView resource bytes differ");
            if (array && !depth)
                c->CopySubresourceRegion(chain.buffer.Get(), 0, 0, 0, 0, target.Get(), 5, nullptr);
            if (msaa)
                c->CopyResource(chain.buffer.Get(), resolved.Get());
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), screenStage.Get(), storage(screen, true));
            save(output / L"frame.rgba", rgba);
            if (rgba != image)
                throw std::runtime_error("ClearView frame image differs");
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
