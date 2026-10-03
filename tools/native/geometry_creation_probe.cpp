// Original GPA geometry-stage creation fixtures. Development only; never shipped with FloraGPA.
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // kind*10 + variant: GS, HS, DS, GS+SO, VS passthrough+SO; success/fail/validate/repeat.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int scenario = std::stoi(argv[2]), kind = scenario / 10, variant = scenario % 10;
        if (scenario < 0 || kind > 4 || variant > 3)
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

        const bool tess = kind == 1 || kind == 2, so = kind >= 3;
        auto compile = [&](const std::string &src, const char *entry, const char *target) {
            ComPtr<ID3DBlob> b, e;
            auto hr =
                D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, 0, 0, &b, &e);
            if (FAILED(hr) && e)
                throw std::runtime_error(static_cast<const char *>(e->GetBufferPointer()));
            checked(hr);
            return b;
        };
        const std::string common = "struct V{float4 p:SV_Position;float4 c:COLOR;};";
        auto vb = compile(common + "V vs(uint i:SV_VertexID){V "
                                   "o;o.p=float4(i==2?3:-1,i==1?3:-1,.5,1);o.c=float4(0,1,0,1);return o;}",
                          "vs", "vs_5_0");
        auto pb = compile(common + "float4 ps(V v):SV_Target{return v.c;}", "ps", "ps_5_0");
        auto gb = compile(common + "[maxvertexcount(3)]void gs(triangle V v[3],inout TriangleStream<V> "
                                   "dst){for(uint i=0;i<3;i++)dst.Append(v[i]);}",
                          "gs", "gs_5_0");
        const std::string patch = "struct C{float e[3]:SV_TessFactor;float inside:SV_InsideTessFactor;};";
        auto hb = compile(common + patch +
                              "C constants(InputPatch<V,3> p){C o;o.e[0]=o.e[1]=o.e[2]=o.inside=1;return "
                              "o;}[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_"
                              "cw\")][outputcontrolpoints(3)][patchconstantfunc(\"constants\")]V "
                              "hs(InputPatch<V,3> p,uint i:SV_OutputControlPointID){return p[i];}",
                          "hs", "hs_5_0");
        auto db = compile(common + patch +
                              "[domain(\"tri\")]V ds(C c,float3 uv:SV_DomainLocation,const OutputPatch<V,3> "
                              "p){V o;o.p=uv.x*p[0].p+uv.y*p[1].p+uv.z*p[2].p;o.c=float4(0,1,0,1);return o;}",
                          "ds", "ds_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> ds;
        checked(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps));
        if (tess && kind != 1)
            checked(d->CreateHullShader(hb->GetBufferPointer(), hb->GetBufferSize(), nullptr, &hs));
        if (tess && kind != 2)
            checked(d->CreateDomainShader(db->GetBufferPointer(), db->GetBufferSize(), nullptr, &ds));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        checked(d->CreateRasterizerState(&rd, &rs));
        std::array<ComPtr<ID3D11Buffer>, 2> buffers, stages;
        const UINT strides[]{16, 20};
        std::array<std::vector<uint8_t>, 2> expectedBuffers;
        const float positions[][4]{{-1, -1, .5f, 1}, {-1, 3, .5f, 1}, {3, -1, .5f, 1}};
        const float green[]{0, 1, 0, 1};
        for (unsigned i = 0; i < 2; i++) {
            D3D11_BUFFER_DESC bd{strides[i] * 3, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
            checked(d->CreateBuffer(&bd, nullptr, &buffers[i]));
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            checked(d->CreateBuffer(&bd, nullptr, &stages[i]));
            expectedBuffers[i].resize(strides[i] * 3, 0);
            for (unsigned v = 0; v < 3; v++)
                memcpy(expectedBuffers[i].data() + v * strides[i], i ? green : positions[v], 16);
        }
        D3D11_SO_DECLARATION_ENTRY decl[]{{0, "SV_Position", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 1}};
        Tex tex;
        tex.width = tex.height = 8;
        tex.mips = tex.layers = 1;
        auto screenStage = create(d.Get(), tex, true);
        auto expected = storage(tex, true);
        save(output / L"expected.rgba", expected[0].bytes);
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
                auto req =
                    shim ? reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                               GetProcAddress(shim, "CaptureNextFrame"))
                         : nullptr;
                if (!req)
                    throw std::runtime_error("CaptureNextFrame unavailable");
                req(argv[3], captured);
            }
            c->ClearState();
            std::vector<ComPtr<IUnknown>> held;
            bool repeatedIdentity = false;
            auto make = [&]<class T>(ComPtr<T> &result, auto call) {
                result.Reset();
                if (variant == 1 || variant == 2) {
                    ComPtr<T> absent;
                    auto hr = call(variant == 1, variant == 2 ? nullptr : absent.GetAddressOf());
                    if ((variant == 1 && (SUCCEEDED(hr) || absent)) || (variant == 2 && hr != S_FALSE))
                        throw std::runtime_error("Unexpected failed/validation result");
                }
                checked(call(false, result.GetAddressOf()));
                if (variant == 3) {
                    held.emplace_back(result);
                    ComPtr<T> again;
                    checked(call(false, again.GetAddressOf()));
                    repeatedIdentity = result.Get() == again.Get();
                    result = again;
                }
            };
            if (kind == 0)
                make(gs, [&](bool bad, ID3D11GeometryShader **o) {
                    return d->CreateGeometryShader(gb->GetBufferPointer(), bad ? 4 : gb->GetBufferSize(),
                                                   nullptr, o);
                });
            if (kind == 1)
                make(hs, [&](bool bad, ID3D11HullShader **o) {
                    return d->CreateHullShader(hb->GetBufferPointer(), bad ? 4 : hb->GetBufferSize(), nullptr,
                                               o);
                });
            if (kind == 2)
                make(ds, [&](bool bad, ID3D11DomainShader **o) {
                    return d->CreateDomainShader(db->GetBufferPointer(), bad ? 4 : db->GetBufferSize(),
                                                 nullptr, o);
                });
            if (so) {
                auto code = kind == 4 ? vb : gb;
                make(gs, [&](bool bad, ID3D11GeometryShader **o) {
                    return d->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(),
                                                                   bad ? 4 : code->GetBufferSize(), decl, 2,
                                                                   strides, 2, 0, nullptr, o);
                });
            }
            const float magenta[]{1, 0, 1, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), magenta);
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(tess ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                           : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->HSSetShader(hs.Get(), nullptr, 0);
            c->DSSetShader(ds.Get(), nullptr, 0);
            c->GSSetShader(gs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            if (so) {
                ID3D11Buffer *targets[]{buffers[0].Get(), buffers[1].Get()};
                UINT offsets[]{0, 0};
                for (unsigned i = 0; i < 2; i++) {
                    std::vector<uint8_t> zero(strides[i] * 3);
                    c->UpdateSubresource(buffers[i].Get(), 0, nullptr, zero.data(), 0, 0);
                }
                c->SOSetTargets(2, targets, offsets);
            }
            c->Draw(3, 0);
            if (so) {
                c->SOSetTargets(0, nullptr, nullptr);
                for (unsigned i = 0; i < 2; i++) {
                    c->CopyResource(stages[i].Get(), buffers[i].Get());
                    D3D11_MAPPED_SUBRESOURCE m{};
                    checked(c->Map(stages[i].Get(), 0, D3D11_MAP_READ, 0, &m));
                    std::vector<uint8_t> bytes(strides[i] * 3);
                    memcpy(bytes.data(), m.pData, bytes.size());
                    c->Unmap(stages[i].Get(), 0);
                    if (bytes != expectedBuffers[i])
                        throw std::runtime_error("SO bytes mismatch");
                    save(output / (L"buffer" + std::to_wstring(i) + L".bin"), bytes);
                    save(output / (L"expected-buffer" + std::to_wstring(i) + L".bin"), expectedBuffers[i]);
                }
            }
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            Tex screen;
            screen.width = screen.height = 8;
            screen.mips = screen.layers = 1;
            auto rgba = read(c.Get(), screenStage.Get(), storage(screen, true));
            if (rgba != expected[0].bytes)
                throw std::runtime_error("Pipeline creation image mismatch");
            save(output / L"frame.rgba", rgba);
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"image_verified\":true,\"repeated_identity\":"
                   << (repeatedIdentity ? "true" : "false") << '}';
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
