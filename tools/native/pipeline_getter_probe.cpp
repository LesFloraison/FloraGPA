// Development-only original captures: pipeline getter observations and CPU return-value oracles.
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
        if (mode < 0 || mode > 7)
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
            if (mode == 0) {
                ComPtr<ID3D11VertexShader> observedVs;
                ComPtr<ID3D11PixelShader> observedPs;
                ComPtr<ID3D11GeometryShader> observedGs;
                ComPtr<ID3D11HullShader> observedHs;
                ComPtr<ID3D11DomainShader> observedDs;
                ComPtr<ID3D11ComputeShader> observedCs;
                UINT count = 0;
                c->VSGetShader(&observedVs, nullptr, &count);
                same(observedVs.Get() == vs.Get() && count == 0);
                c->PSGetShader(&observedPs, nullptr, &count);
                same(observedPs.Get() == sampled.Get() && count == 0);
                c->GSGetShader(&observedGs, nullptr, &count);
                same(!observedGs && count == 0);
                c->HSGetShader(&observedHs, nullptr, &count);
                same(!observedHs && count == 0);
                c->DSGetShader(&observedDs, nullptr, &count);
                same(!observedDs && count == 0);
                c->CSGetShader(&observedCs, nullptr, &count);
                same(!observedCs && count == 0);
            } else if (mode == 1) {
                using Get =
                    void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView **);
                const Get getters[]{
                    &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
                    &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
                    &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
                for (auto get : getters) {
                    ID3D11ShaderResourceView *views[2]{};
                    (c.Get()->*get)(0, 2, views);
                    same(views[0] == input && !views[1]);
                    for (auto view : views)
                        if (view)
                            view->Release();
                    (c.Get()->*get)(127, 0, nullptr);
                }
            } else if (mode == 2) {
                using Get = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer **);
                const Get getters[]{
                    &ID3D11DeviceContext::VSGetConstantBuffers, &ID3D11DeviceContext::HSGetConstantBuffers,
                    &ID3D11DeviceContext::DSGetConstantBuffers, &ID3D11DeviceContext::GSGetConstantBuffers,
                    &ID3D11DeviceContext::PSGetConstantBuffers, &ID3D11DeviceContext::CSGetConstantBuffers};
                for (auto get : getters) {
                    ID3D11Buffer *values[2]{};
                    (c.Get()->*get)(1, 2, values);
                    same(values[0] == cb.Get() && !values[1]);
                    for (auto value : values)
                        if (value)
                            value->Release();
                    (c.Get()->*get)(13, 0, nullptr);
                }
            } else if (mode == 3) {
                using Get =
                    void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState **);
                const Get getters[]{&ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::HSGetSamplers,
                                    &ID3D11DeviceContext::DSGetSamplers, &ID3D11DeviceContext::GSGetSamplers,
                                    &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::CSGetSamplers};
                for (auto get : getters) {
                    ID3D11SamplerState *values[2]{};
                    (c.Get()->*get)(0, 2, values);
                    same(values[0] == sam && !values[1]);
                    for (auto value : values)
                        if (value)
                            value->Release();
                    (c.Get()->*get)(15, 0, nullptr);
                }
            } else if (mode == 4) {
                ID3D11Buffer *values[2]{};
                UINT observedStrides[2]{}, observedOffsets[2]{};
                c->IAGetVertexBuffers(2, 2, values, observedStrides, observedOffsets);
                same(values[0] == vb.Get() && !values[1] && observedStrides[0] == 4 &&
                     observedOffsets[0] == 8);
                for (auto value : values)
                    if (value)
                        value->Release();
                c->IAGetVertexBuffers(2, 2, nullptr, observedStrides, nullptr);
                same(observedStrides[0] == 4);
                ComPtr<ID3D11Buffer> index;
                DXGI_FORMAT format{};
                UINT offset = 0;
                c->IAGetIndexBuffer(&index, &format, &offset);
                same(index.Get() == vb.Get() && format == DXGI_FORMAT_R32_UINT && offset == 4);
                c->IAGetIndexBuffer(nullptr, &format, nullptr);
                same(format == DXGI_FORMAT_R32_UINT);
                ComPtr<ID3D11InputLayout> layout;
                c->IAGetInputLayout(&layout);
                same(!layout);
                D3D11_PRIMITIVE_TOPOLOGY topology{};
                c->IAGetPrimitiveTopology(&topology);
                same(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            } else if (mode == 5) {
                ComPtr<ID3D11RasterizerState> state;
                c->RSGetState(&state);
                same(state.Get() == rs.Get());
                UINT count = 0;
                c->RSGetScissorRects(&count, nullptr);
                same(count == 2);
                D3D11_RECT values[2]{};
                count = 2;
                c->RSGetScissorRects(&count, values);
                same(count == 2 && !memcmp(values, scissors, sizeof values));
                c->RSGetViewports(&count, nullptr);
                same(count == 1);
                D3D11_VIEWPORT observed[2]{};
                count = 2;
                c->RSGetViewports(&count, observed);
                same(count == 1 && !memcmp(observed, &vp, sizeof vp));
            } else if (mode == 6) {
                ComPtr<ID3D11RenderTargetView> rtv;
                ComPtr<ID3D11DepthStencilView> depth;
                ID3D11UnorderedAccessView *values[2]{};
                c->OMGetRenderTargetsAndUnorderedAccessViews(1, &rtv, &depth, 1, 2, values);
                same(rtv.Get() == target && !depth && !values[0] && !values[1]);
                c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, values);
                same(!values[0] && !values[1]);
                c->CSGetUnorderedAccessViews(1, 2, values);
                same(!values[0] && !values[1]);
                c->CSGetUnorderedAccessViews(7, 0, nullptr);
            } else if (mode == 7) {
                ID3D11Buffer *values[2]{};
                c->SOGetTargets(2, values);
                same(!values[0] && !values[1]);
                c->SOGetTargets(0, nullptr);
                ComPtr<ID3D11Predicate> predicate;
                BOOL predicated = TRUE;
                c->GetPredication(&predicate, &predicated);
                same(!predicate);
                c->GetPredication(nullptr, &predicated);
                same(c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
                same(c->GetContextFlags() == 0);
            }
            c->Draw(3, 0);
            c->CopyResource(stage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), stage.Get(), pixels);
            save(output / L"frame.rgba", rgba);
            if (rgba != pixels[0].bytes)
                throw std::runtime_error("Getter image mismatch");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"observations\":" << observations
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
