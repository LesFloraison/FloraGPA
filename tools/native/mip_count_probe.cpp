// Development-only originals: mip-count metadata versus missing resource LOD.
#include "texture_probe_helpers.h"
#include "original_capture_control.h"
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 35)
            throw std::runtime_error("Invalid mode");
        const int switchBehaviors[]{0, 1, 2, 2, 4, 5, 0, 0};
        const int loopBehaviors[]{0, 1, 2, 4, 4, 5, 0, 0};
        const int behavior = mode >= 28 ? loopBehaviors[mode - 28] : mode >= 20 ? switchBehaviors[mode - 20] : mode < 18 ? mode % 6 : 0;
        const char *profile = mode < 6 ? "ps_5_0" : "ps_4_0";
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
        ComPtr<ID3D11Texture2D> texture, ordinary;
        checked(d->CreateTexture2D(&td, initial.data(), &texture));
        td.MiscFlags = 0;
        checked(d->CreateTexture2D(&td, initial.data(), &ordinary));
        ComPtr<ID3D11ShaderResourceView> srv, normal;
        checked(d->CreateShaderResourceView(texture.Get(), nullptr, &srv));
        checked(d->CreateShaderResourceView(ordinary.Get(), nullptr, &normal));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        checked(d->CreateSamplerState(&sd, &sampler));
        const std::string shader =
            "Texture2D<float4> tex:register(t0);Texture2D<float4> normal:register(t1);"
            "SamplerState sam:register(s0);"
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}"
            "float4 count():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);return float4(n/4.0,0,0,1);}"
            "float4 dimensions():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);return "
            "float4(w==4,h==4,n==4,1);}"
            "float4 mixed():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);return "
            "float4(n/4.0,normal.Sample(sam,float2(.5,.5)).r,0,1);}"
            "float4 dependent():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);return "
            "float4(n/4.0,tex.Sample(sam,float2(.5,.5)).g,0,1);}"
            "float4 branched():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[branch]if(n==4)r=float4(n/4.0,0,0,1);else r=float4(0,n/4.0,0,1);return r;}"
            "float4 branchWidth():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);float4 r;"
            "[branch]if(n==4)r=float4(w==4,h==4,n==4,1);else r=float4(0,0,0,1);return r;}"
            "float4 elseWidth():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);float4 r;"
            "[branch]if(n==0)r=float4(0,0,0,1);else r=float4(w==4,h==4,n==4,1);return r;}"
            "float4 branchSample():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[branch]if(n==4)r=float4(n/4.0,tex.Sample(sam,float2(.5,.5)).g,0,1);"
            "else r=float4(0,0,0,1);return r;}"
            "float4 nested():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[branch]if(n>2){[branch]if(n==4)r=float4(n/4.0,0,0,1);else r=float4(0,0,1,1);}"
            "else r=float4(0,1,0,1);return r;}"
            "float4 noElse():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r=float4(0,1,0,1);"
            "[branch]if(n==4)r=float4(n/4.0,0,0,1);return r;}"
            "float4 switched():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[forcecase]switch(n){case 4:r=float4(n/4.0,0,0,1);break;"
            "case 3:r=float4(0,n/4.0,0,1);break;default:r=float4(0,0,n/4.0,1);break;}return r;}"
            "float4 switchWidth():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);float4 r;"
            "[forcecase]switch(n){case 4:r=float4(w==4,h==4,n==4,1);break;"
            "case 3:r=float4(0,n/4.0,0,1);break;default:r=float4(0,0,0,1);break;}return r;}"
            "float4 defaultWidth():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);float4 r;"
            "[forcecase]switch(n){case 0:r=float4(0,0,0,1);break;case 3:r=float4(0,0,1,1);break;"
            "default:r=float4(w==4,h==4,n==4,1);break;}return r;}"
            "float4 switchSample():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[forcecase]switch(n){case 4:r=float4(n/4.0,tex.Sample(sam,float2(.5,.5)).g,0,1);break;"
            "case 3:r=float4(0,n/4.0,0,1);break;default:r=float4(0,0,0,1);break;}return r;}"
            "float4 nestedSwitch():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[forcecase]switch(n){case 4:{uint a,b,m;normal.GetDimensions(0,a,b,m);"
            "[forcecase]switch(m){case 4:r=float4(m/4.0,0,0,1);break;case 3:r=float4(0,0,1,1);break;"
            "default:r=float4(0,1,0,1);break;}}break;case 3:r=float4(0,0,n/4.0,1);break;"
            "default:r=float4(0,1,0,1);break;}return r;}"
            "float4 sharedCase():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float4 r;"
            "[forcecase]switch(n){case 3:case 4:r=float4(n/4.0,0,0,1);break;"
            "default:r=float4(0,n/4.0,0,1);break;}return r;}"
            "float4 loopCount():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);uint a=0;"
            "[loop]for(uint i=0;i<n;++i){a+=1;}return float4(a/4.0,0,0,1);}"
            "float4 loopWidth():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);uint a=0;"
            "[loop]for(uint i=0;i<n;++i){a+=w;}return float4(a==16,h==4,n==4,1);}"
            "float4 loopCondition():SV_Target{uint w,h,n;tex.GetDimensions(1,w,h,n);uint a=0;"
            "[loop]for(uint i=0;i<w;++i){a+=1;}return float4(a==4,n==4,0,1);}"
            "float4 loopSample():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);float a=0;"
            "[loop]for(uint i=0;i<n;++i){a+=tex.SampleLevel(sam,float2(.5,.5),0).g;}"
            "return float4(n/4.0,a/4.0,0,1);}"
            "float4 nestedLoop():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);uint a=0;"
            "[loop]for(uint i=0;i<n;++i){[loop]for(uint j=0;j<n;++j){a+=1;}}return float4(a/16.0,0,0,1);}"
            "float4 continueLoop():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);uint a=0;"
            "[loop]for(uint i=0;i<n;++i){[branch]if(i==1)continue;a+=1;}return float4(a/3.0,0,0,1);}";
        auto compile = [&](const char *entry, const char *target, bool strip) {
            ComPtr<ID3DBlob> code, errors;
            checked(D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, entry, target, 0, 0,
                               &code, &errors));
            if (strip) {
                ComPtr<ID3DBlob> stripped;
                checked(D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                                       D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped));
                code = stripped;
            }
            auto ptr = static_cast<const uint8_t *>(code->GetBufferPointer());
            save(output / (std::string(entry) + ".dxbc"),
                 std::vector<uint8_t>(ptr, ptr + code->GetBufferSize()));
            ComPtr<ID3DBlob> assembly;
            checked(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &assembly));
            auto text = static_cast<const uint8_t *>(assembly->GetBufferPointer());
            save(output / (std::string(entry) + ".asm"),
                 std::vector<uint8_t>(text, text + assembly->GetBufferSize()));
            return code;
        };
        auto vsCode = compile("vs", "vs_5_0", false);
        const char *branchEntries[]{"branched", "branched", "branchWidth", "elseWidth",
                                    "branchSample", "branchSample", "nested", "noElse"};
        const char *switchEntries[]{"switched", "switched", "switchWidth", "defaultWidth",
                                   "switchSample", "switchSample", "nestedSwitch", "sharedCase"};
        const char *loopEntries[]{"loopCount", "loopCount", "loopWidth", "loopCondition",
                                  "loopSample", "loopSample", "nestedLoop", "continueLoop"};
        auto psCode = compile(mode >= 28 ? loopEntries[mode - 28] : mode >= 20 ? switchEntries[mode - 20] : mode >= 12 ? branchEntries[mode - 12] : behavior < 2    ? "count"
                              : behavior == 2 ? "dimensions"
                              : behavior == 3 ? "mixed"
                                          : "dependent",
                              profile, behavior == 1);
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
        const std::array<uint8_t, 4> expected = behavior < 2    ? std::array<uint8_t, 4>{255, 0, 0, 255}
                                                : behavior == 2 || mode == 15 ? std::array<uint8_t, 4>{255, 255, 255, 255}
                                                            : std::array<uint8_t, 4>{255, 255, 0, 255};
        for (size_t p = 0; p < pixels[0].bytes.size(); p += 4)
            memcpy(pixels[0].bytes.data() + p, expected.data(), 4);
        save(output / L"expected.rgba", pixels[0].bytes);
        // Intentionally outside the captured frame; no getter is saved.
        c->SetResourceMinLOD(texture.Get(), 1);
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        DXGI_ADAPTER_DESC adapterDesc{};
        checked(adapter->GetDesc(&adapterDesc));
        report << "{\"mode\":" << mode << ",\"warp\":" << (warp ? "true" : "false")
               << ",\"profile\":\"" << profile << '"'
               << ",\"vendor_id\":" << adapterDesc.VendorId << ",\"device_id\":" << adapterDesc.DeviceId
               << ",\"frames\":[";
        bool primaryChanged = false;
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
                primaryChanged = flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                request(argv[3], captured);
            }
            c->ClearState();
            const float black[]{0, 0, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), black);
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            ID3D11ShaderResourceView *inputs[]{srv.Get(), normal.Get()};
            c->PSSetShaderResources(0, 2, inputs);
            auto sam = sampler.Get();
            c->PSSetSamplers(0, 1, &sam);
            if (behavior == 5)
                c->SetResourceMinLOD(texture.Get(), 1);
            c->Draw(3, 0);
            c->CopyResource(stage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), stage.Get(), pixels);
            save(output / L"frame.rgba", rgba);
            if (rgba != pixels[0].bytes)
                throw std::runtime_error("Resource usage image mismatch");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"image_verified\":true}";
            Sleep(25);
        }
        if (argc == 5 && !fs::is_regular_file(argv[3]))
            throw std::runtime_error("Original capture request produced no frame file");
        report << "],\"capture_requested\":" << (argc == 5 ? "true" : "false")
               << ",\"primary_changed\":" << (primaryChanged ? "true" : "false")
               << ",\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
