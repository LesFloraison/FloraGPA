// Development-only original captures: nonempty and optional pipeline getter outputs.
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
        if (mode < 8 || mode > 11)
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
        D3D11_TEXTURE2D_DESC td{
            8, 8, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE,
            0, 0};
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
        D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const std::array<UINT, 16> zeros{};
        D3D11_SUBRESOURCE_DATA initialBuffer{zeros.data(), 0, 0};
        ComPtr<ID3D11Buffer> cb, vb;
        checked(d->CreateBuffer(&cbd, &initialBuffer, &cb));
        cbd.ByteWidth = 64;
        cbd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
        checked(d->CreateBuffer(&cbd, &initialBuffer, &vb));
        ComPtr<ID3D11Buffer> uavBuffer, soBuffer, soStage;
        D3D11_BUFFER_DESC more{64, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        checked(d->CreateBuffer(&more, &initialBuffer, &soBuffer));
        if (mode == 10) {
            auto stageDesc = more;
            stageDesc.Usage = D3D11_USAGE_STAGING;
            stageDesc.BindFlags = 0;
            stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            checked(d->CreateBuffer(&stageDesc, nullptr, &soStage));
        }
        more.ByteWidth = 16;
        more.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        checked(d->CreateBuffer(&more, &initialBuffer, &uavBuffer));
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 4;
        ComPtr<ID3D11UnorderedAccessView> uav;
        checked(d->CreateUnorderedAccessView(uavBuffer.Get(), &uavDesc, &uav));
        D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        ComPtr<ID3D11Predicate> query;
        checked(d->CreatePredicate(&predicateDesc, &query));
        const std::string shader =
            "Texture2D<float4> tex:register(t0);SamplerState sam:register(s0);"
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}"
            "float4 constant():SV_Target{return float4(0,0,1,1);}"
            "float4 sampled():SV_Target{return tex.Sample(sam,float2(0.5,0.5));}";
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
        auto constantCode = compile("constant", "ps_5_0", false);
        auto sampledCode = compile("sampled", "ps_5_0", false);
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> constant, sampled;
        checked(d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(constantCode->GetBufferPointer(), constantCode->GetBufferSize(), nullptr,
                                     &constant));
        checked(d->CreatePixelShader(sampledCode->GetBufferPointer(), sampledCode->GetBufferSize(), nullptr,
                                     &sampled));
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
        const auto expected = colors[0];
        for (size_t p = 0; p < pixels[0].bytes.size(); p += 4)
            memcpy(pixels[0].bytes.data() + p, expected.data(), 4);
        save(output / L"expected.rgba", pixels[0].bytes);
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
            const float black[]{0, 0, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), black);
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(sampled.Get(), nullptr, 0);
            auto input = srv.Get();
            auto sam = sampler.Get();
            using SRVSet = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT,
                                                                           ID3D11ShaderResourceView *const *);
            const SRVSet srvSet[]{
                &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::HSSetShaderResources,
                &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::GSSetShaderResources,
                &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::CSSetShaderResources};
            using CBSet = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer *const *);
            const CBSet cbSet[]{
                &ID3D11DeviceContext::VSSetConstantBuffers, &ID3D11DeviceContext::HSSetConstantBuffers,
                &ID3D11DeviceContext::DSSetConstantBuffers, &ID3D11DeviceContext::GSSetConstantBuffers,
                &ID3D11DeviceContext::PSSetConstantBuffers, &ID3D11DeviceContext::CSSetConstantBuffers};
            using SamSet =
                void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState *const *);
            const SamSet samSet[]{&ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::HSSetSamplers,
                                  &ID3D11DeviceContext::DSSetSamplers, &ID3D11DeviceContext::GSSetSamplers,
                                  &ID3D11DeviceContext::PSSetSamplers, &ID3D11DeviceContext::CSSetSamplers};
            ID3D11ShaderResourceView *inputs[]{input, nullptr};
            ID3D11Buffer *constants[]{cb.Get(), nullptr};
            ID3D11SamplerState *samplers[]{sam, nullptr};
            for (unsigned i = 0; i < 6; ++i) {
                (c.Get()->*srvSet[i])(0, 2, inputs);
                (c.Get()->*cbSet[i])(1, 2, constants);
                (c.Get()->*samSet[i])(0, 2, samplers);
            }
            ID3D11Buffer *vertices[]{vb.Get(), nullptr};
            UINT strides[]{4, 0}, offsets[]{8, 0};
            c->IASetVertexBuffers(2, 2, vertices, strides, offsets);
            c->IASetIndexBuffer(vb.Get(), DXGI_FORMAT_R32_UINT, 4);
            const D3D11_RECT scissors[]{{1, 2, 6, 7}, {0, 0, 8, 8}};
            c->RSSetScissorRects(2, scissors);
            unsigned observations = 0;
            auto same = [&](bool value) {
                if (!value)
                    throw std::runtime_error("Getter observation disagrees with producer bindings");
                ++observations;
            };
            if (mode == 8) {
                ComPtr<ID3D11VertexShader> observed;
                c->VSGetShader(&observed, nullptr, nullptr);
                same(observed.Get() == vs.Get());
                UINT classes = 99;
                observed.Reset();
                c->VSGetShader(&observed, nullptr, &classes);
                same(observed.Get() == vs.Get());
                same(classes == 0);
                c->IAGetVertexBuffers(2, 2, nullptr, nullptr, nullptr);
                c->IAGetIndexBuffer(nullptr, nullptr, nullptr);
                c->GetPredication(nullptr, nullptr);
                c->RSSetScissorRects(0, nullptr);
                D3D11_RECT rects[16]{};
                UINT count = 16;
                c->RSGetScissorRects(&count, rects);
                same(count == 0);
                count = 0;
                c->RSGetScissorRects(&count, nullptr);
                same(count == 0);
            } else if (mode == 9) {
                auto view = uav.Get();
                ID3D11UnorderedAccessView *values[2]{};
                c->CSSetUnorderedAccessViews(1, 1, &view, nullptr);
                c->CSGetUnorderedAccessViews(1, 2, values);
                same(values[0] == view && !values[1]);
                for (auto v : values)
                    if (v)
                        v->Release();
                ID3D11UnorderedAccessView *none = nullptr;
                c->CSSetUnorderedAccessViews(1, 1, &none, nullptr);
                c->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr, 1, 1, &view, nullptr);
                ComPtr<ID3D11RenderTargetView> observed;
                c->OMGetRenderTargetsAndUnorderedAccessViews(1, &observed, nullptr, 1, 2, values);
                same(observed.Get() == target && values[0] == view && !values[1]);
                for (auto v : values)
                    if (v)
                        v->Release();
                c->OMGetRenderTargetsAndUnorderedAccessViews(1, nullptr, nullptr, 1, 1, nullptr);
                c->OMSetRenderTargets(1, &target, nullptr);
            } else if (mode == 10) {
                auto buffer = soBuffer.Get();
                UINT offset = 4;
                c->SOSetTargets(1, &buffer, &offset);
                ID3D11Buffer *values[4]{};
                c->SOGetTargets(4, values);
                same(values[0] == buffer && !values[1] && !values[2] && !values[3]);
                for (auto v : values)
                    if (v)
                        v->Release();
                // A nonzero count with a null SO output array crashes the local native runtime.
                // Keep that failed development probe separate from this valid fixture.
                c->SOSetTargets(0, nullptr, nullptr);
                c->CopyResource(soStage.Get(), soBuffer.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(c->Map(soStage.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                std::vector<uint8_t> raw(64);
                memcpy(raw.data(), mapped.pData, raw.size());
                c->Unmap(soStage.Get(), 0);
                same(raw == std::vector<uint8_t>(64));
                save(output / L"so.bin", raw);
            } else if (mode == 11) {
                c->Begin(query.Get());
                c->End(query.Get());
                c->SetPredication(query.Get(), TRUE);
                ComPtr<ID3D11Predicate> observed;
                BOOL value = FALSE;
                c->GetPredication(&observed, &value);
                same(observed.Get() == query.Get() && value == TRUE);
                c->GetPredication(nullptr, &value);
                same(value == TRUE);
                c->SetPredication(nullptr, FALSE);
            }
            c->Draw(3, 0);
            c->CopyResource(stage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), stage.Get(), pixels);
            save(output / L"frame.rgba", rgba);
            if (rgba != pixels[0].bytes)
                throw std::runtime_error("Getter image mismatch");
            const auto presentStatus = chain.swap->Present(0, 0);
            checked(presentStatus);
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"observations\":" << observations
                   << ",\"image_verified\":true,\"present_status\":" << uint32_t(presentStatus) << "}";
            Sleep(100);
        }
        report << "],\"completed\":true}\n";
        if (argc == 5) {
            for (unsigned attempt = 0; attempt < 100 && !fs::exists(argv[3]); ++attempt)
                Sleep(50);
            if (!fs::exists(argv[3]) || !fs::file_size(argv[3]))
                throw std::runtime_error("Original producer completed but no capture was saved");
        }
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
