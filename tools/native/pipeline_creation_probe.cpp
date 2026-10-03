// Original GPA view-creation fixtures. Development only; never shipped with FloraGPA.
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // kind*10 + variant: VS, PS, CS, layout, blend, depth, raster, sampler; success/fail/validate/repeat.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int scenario = std::stoi(argv[2]), kind = scenario / 10, variant = scenario % 10;
        if (scenario < 0 || kind > 7 || variant > 3)
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

        const bool layoutMode = kind == 3, computeMode = kind == 2;
        std::string shader = "struct V{float4 p:SV_Position;float4 c:COLOR;};V vs(";
        shader += layoutMode ? "uint value:DATA," : "";
        shader += "uint i:SV_VertexID){V o;o.p=float4(i==2?3:-1,i==1?3:-1,0.5,1);o.c=";
        shader += layoutMode  ? "value==123?float4(0,1,0,1):float4(1,0,1,1)"
                  : kind == 4 ? "float4(1,1,1,1)"
                              : "float4(0,1,0,1)";
        shader += ";return o;}";
        if (kind == 2)
            shader += "Buffer<uint> data:register(t0);float4 ps(V i):SV_Target{return "
                      "data[0]==123?float4(0,1,0,1):float4(1,0,1,1);}";
        else if (kind == 7)
            shader += "Texture2D<float4> tex:register(t0);SamplerState samp:register(s0);float4 ps(V "
                      "i):SV_Target{return tex.SampleLevel(samp,float2(2,2),0);}";
        else
            shader += "float4 ps(V i):SV_Target{return i.c;}";
        auto compile = [&](const std::string &src, const char *entry, const char *target) {
            ComPtr<ID3DBlob> b, e;
            auto hr =
                D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, 0, 0, &b, &e);
            if (FAILED(hr) && e)
                throw std::runtime_error(static_cast<const char *>(e->GetBufferPointer()));
            checked(hr);
            return b;
        };
        auto vb = compile(shader, "vs", "vs_5_0"), pb = compile(shader, "ps", "ps_5_0"),
             cb = compile("RWBuffer<uint> dest:register(u0);[numthreads(1,1,1)]void main(){dest[0]=123;}",
                          "main", "cs_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11ComputeShader> cs;
        if (kind != 0)
            checked(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs));
        if (kind != 1)
            checked(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps));
        D3D11_INPUT_ELEMENT_DESC ie{"DATA", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        ComPtr<ID3D11InputLayout> layout;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = 15;
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_GREATER;
        depth.StencilReadMask = depth.StencilWriteMask = 255;
        depth.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                           D3D11_COMPARISON_ALWAYS};
        depth.BackFace = depth.FrontFace;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = kind == 6;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.BorderColor[1] = sampler.BorderColor[3] = 1;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11BlendState> bs;
        ComPtr<ID3D11DepthStencilState> ds;
        ComPtr<ID3D11RasterizerState> rs;
        ComPtr<ID3D11SamplerState> ss;
        if (kind != 6)
            checked(d->CreateRasterizerState(&raster, &rs));
        D3D11_BUFFER_DESC bd{12, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> vertices;
        checked(d->CreateBuffer(&bd, nullptr, &vertices));
        bd = {4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
        ComPtr<ID3D11Buffer> data, stage;
        checked(d->CreateBuffer(&bd, nullptr, &data));
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked(d->CreateBuffer(&bd, nullptr, &stage));
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 1;
        ComPtr<ID3D11UnorderedAccessView> uav;
        checked(d->CreateUnorderedAccessView(data.Get(), &ud, &uav));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = 1;
        ComPtr<ID3D11ShaderResourceView> srv;
        checked(d->CreateShaderResourceView(data.Get(), &sd, &srv));
        Tex tex;
        tex.width = tex.height = 8;
        tex.mips = tex.layers = 1;
        auto screenStage = create(d.Get(), tex, true);
        auto expected = storage(tex, true);
        if (kind == 6)
            for (UINT y = 0; y < 8; ++y)
                for (UINT x = 4; x < 8; ++x) {
                    auto i = (y * 8 + x) * 4;
                    expected[0].bytes[i] = 255;
                    expected[0].bytes[i + 1] = 0;
                    expected[0].bytes[i + 2] = 255;
                }
        save(output / L"expected.rgba", expected[0].bytes);
        auto sampleTex = create(d.Get(), tex);
        ComPtr<ID3D11ShaderResourceView> sampleSrv;
        checked(d->CreateShaderResourceView(sampleTex.Get(), nullptr, &sampleSrv));
        tex.format = DXGI_FORMAT_D32_FLOAT;
        tex.bind = D3D11_BIND_DEPTH_STENCIL;
        auto depthTex = create(d.Get(), tex);
        ComPtr<ID3D11DepthStencilView> dsv;
        checked(d->CreateDepthStencilView(depthTex.Get(), nullptr, &dsv));
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
            std::array<UINT, 3> vertexData{123, 123, 123};
            c->UpdateSubresource(vertices.Get(), 0, nullptr, vertexData.data(), 0, 0);
            UINT zero = 0;
            c->UpdateSubresource(data.Get(), 0, nullptr, &zero, 0, 0);
            std::array<uint8_t, 256> black{};
            c->UpdateSubresource(sampleTex.Get(), 0, nullptr, black.data(), 32, 256);
            c->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, .25f, 0);
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
                make(vs, [&](bool bad, ID3D11VertexShader **o) {
                    return d->CreateVertexShader(vb->GetBufferPointer(), bad ? 4 : vb->GetBufferSize(),
                                                 nullptr, o);
                });
            if (kind == 1)
                make(ps, [&](bool bad, ID3D11PixelShader **o) {
                    return d->CreatePixelShader(pb->GetBufferPointer(), bad ? 4 : pb->GetBufferSize(),
                                                nullptr, o);
                });
            if (kind == 2)
                make(cs, [&](bool bad, ID3D11ComputeShader **o) {
                    return d->CreateComputeShader(cb->GetBufferPointer(), bad ? 4 : cb->GetBufferSize(),
                                                  nullptr, o);
                });
            if (kind == 3)
                make(layout, [&](bool bad, ID3D11InputLayout **o) {
                    auto desc = ie;
                    if (bad)
                        desc.Format = DXGI_FORMAT_UNKNOWN;
                    return d->CreateInputLayout(&desc, 1, vb->GetBufferPointer(), vb->GetBufferSize(), o);
                });
            if (kind == 4)
                make(bs, [&](bool bad, ID3D11BlendState **o) {
                    auto desc = blend;
                    if (bad)
                        desc.RenderTarget[0].SrcBlend = D3D11_BLEND(0);
                    return d->CreateBlendState(&desc, o);
                });
            if (kind == 5)
                make(ds, [&](bool bad, ID3D11DepthStencilState **o) {
                    auto desc = depth;
                    if (bad)
                        desc.DepthFunc = D3D11_COMPARISON_FUNC(0);
                    return d->CreateDepthStencilState(&desc, o);
                });
            if (kind == 6)
                make(rs, [&](bool bad, ID3D11RasterizerState **o) {
                    auto desc = raster;
                    if (bad)
                        desc.FillMode = D3D11_FILL_MODE(0);
                    return d->CreateRasterizerState(&desc, o);
                });
            if (kind == 7)
                make(ss, [&](bool bad, ID3D11SamplerState **o) {
                    auto desc = sampler;
                    if (bad)
                        desc.Filter = D3D11_FILTER(UINT_MAX);
                    return d->CreateSamplerState(&desc, o);
                });
            if (computeMode) {
                auto v = uav.Get();
                c->CSSetUnorderedAccessViews(0, 1, &v, nullptr);
                c->CSSetShader(cs.Get(), nullptr, 0);
                c->Dispatch(1, 1, 1);
                v = nullptr;
                c->CSSetUnorderedAccessViews(0, 1, &v, nullptr);
                c->CopyResource(stage.Get(), data.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                checked(c->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m));
                UINT value;
                memcpy(&value, m.pData, 4);
                c->Unmap(stage.Get(), 0);
                if (value != 123)
                    throw std::runtime_error("Compute bytes mismatch");
                std::vector<uint8_t> b(4);
                memcpy(b.data(), &value, 4);
                save(output / L"buffer.bin", b);
            }
            const float magenta[]{1, 0, 1, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), magenta);
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, kind == 5 ? dsv.Get() : nullptr);
            if (kind == 4) {
                const float factors[]{0, 1, 0, 1};
                c->OMSetBlendState(bs.Get(), factors, UINT_MAX);
            }
            if (kind == 5)
                c->OMSetDepthStencilState(ds.Get(), 0);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            D3D11_RECT rect{0, 0, 4, 8};
            c->RSSetScissorRects(1, &rect);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            if (layoutMode) {
                auto v = vertices.Get();
                UINT stride = 4, offset = 0;
                c->IASetVertexBuffers(0, 1, &v, &stride, &offset);
                c->IASetInputLayout(layout.Get());
            }
            if (computeMode) {
                auto v = srv.Get();
                c->PSSetShaderResources(0, 1, &v);
            }
            if (kind == 7) {
                auto v = sampleSrv.Get();
                auto sampler = ss.Get();
                c->PSSetShaderResources(0, 1, &v);
                c->PSSetSamplers(0, 1, &sampler);
            }
            c->Draw(3, 0);
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
