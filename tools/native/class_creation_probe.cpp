// Original GPA class linkage/instance creation fixtures. Development only; never shipped with FloraGPA.
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // kind*10 + variant: new linkage, existing linkage/Get, existing linkage/Create; see audit.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int scenario = std::stoi(argv[2]), kind = scenario / 10, variant = scenario % 10;
        if (scenario < 0 || kind > 2 || variant > (kind == 0 ? 4 : 3))
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

        auto compile = [&](const std::string &src, const char *profile) {
            ComPtr<ID3DBlob> b, e;
            auto hr =
                D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile, 0, 0, &b, &e);
            if (FAILED(hr) && e)
                throw std::runtime_error(static_cast<const char *>(e->GetBufferPointer()));
            checked(hr);
            return b;
        };
        const std::string code = "interface I{uint apply(uint x);};class A:I{uint value;uint apply(uint "
                                 "x){return x+value;}};class B:I{uint value;uint apply(uint x){return "
                                 "x+value*2;}};cbuffer Classes:register(b0){A first[2];B second[2];} I "
                                 "selected;RWStructuredBuffer<uint> dst:register(u0);[numthreads(4,1,1)]void "
                                 "main(uint3 id:SV_DispatchThreadID){dst[id.x]=selected.apply(id.x);}";
        auto cb = compile(code, "cs_5_0");
        auto vb = compile(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        unsigned expectedValue = kind == 2                                     ? (variant == 3 ? 20 : 40)
                                 : kind == 0 && (variant == 1 || variant == 4) ? 40
                                                                               : 20;
        auto pb =
            compile("StructuredBuffer<uint> data:register(t0);float4 main():SV_Target{return (data[0]==" +
                        std::to_string(expectedValue) + "&&data[1]==" + std::to_string(expectedValue + 1) +
                        "&&data[2]==" + std::to_string(expectedValue + 2) + "&&data[3]==" +
                        std::to_string(expectedValue + 3) + ")?float4(0,1,0,1):float4(1,0,1,1);}",
                    "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11ComputeShader> cs;
        checked(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps));
        ComPtr<ID3D11ClassLinkage> linkage;
        auto shader = [&] {
            checked(d->CreateComputeShader(cb->GetBufferPointer(), cb->GetBufferSize(), linkage.Get(), &cs));
        };
        if (kind) {
            checked(d->CreateClassLinkage(&linkage));
            shader();
        }
        D3D11_BUFFER_DESC bd{64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> constants, data, staging;
        checked(d->CreateBuffer(&bd, nullptr, &constants));
        bd = {16,
              D3D11_USAGE_DEFAULT,
              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
              0,
              D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
              4};
        checked(d->CreateBuffer(&bd, nullptr, &data));
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags = 0;
        bd.StructureByteStride = 0;
        checked(d->CreateBuffer(&bd, nullptr, &staging));
        ComPtr<ID3D11UnorderedAccessView> uav;
        checked(d->CreateUnorderedAccessView(data.Get(), nullptr, &uav));
        ComPtr<ID3D11ShaderResourceView> srv;
        checked(d->CreateShaderResourceView(data.Get(), nullptr, &srv));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        checked(d->CreateRasterizerState(&rd, &rs));
        Tex tex;
        tex.width = tex.height = 8;
        tex.mips = tex.layers = 1;
        auto screenStage = create(d.Get(), tex, true);
        auto expected = storage(tex, true);
        save(output / L"expected.rgba", expected[0].bytes);
        DXGI_ADAPTER_DESC ad{};
        checked(adapter->GetDesc(&ad));
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << scenario << ",\"vendor_id\":" << ad.VendorId
               << ",\"device_id\":" << ad.DeviceId << ",\"expected_value\":" << expectedValue
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
            std::vector<ComPtr<IUnknown>> held;
            HRESULT observation = 0;
            bool repeatedIdentity = false;
            if (kind == 0) {
                cs.Reset();
                linkage.Reset();
                checked(d->CreateClassLinkage(&linkage));
                if (variant == 2) {
                    held.emplace_back(linkage);
                    ComPtr<ID3D11ClassLinkage> again;
                    checked(d->CreateClassLinkage(&again));
                    repeatedIdentity = linkage.Get() == again.Get();
                    linkage = again;
                }
                if (variant != 3 && variant != 4)
                    shader();
            }
            std::array<UINT, 16> values{};
            values[0] = 10;
            values[4] = 20;
            values[8] = 30;
            values[12] = 40;
            c->UpdateSubresource(constants.Get(), 0, nullptr, values.data(), 0, 0);
            std::array<UINT, 4> zero{};
            c->UpdateSubresource(data.Get(), 0, nullptr, zero.data(), 0, 0);
            const bool created = kind == 2 || (kind == 0 && (variant == 1 || variant == 4));
            auto instance = [&](bool alternate, bool bad, ID3D11ClassInstance **out) {
                return created ? linkage->CreateClassInstance(bad         ? "Missing"
                                                              : alternate ? "B"
                                                                          : "A",
                                                              2, alternate ? 0 : 3, 0, 0, out)
                               : linkage->GetClassInstance(bad ? "Missing" : "first", alternate ? 0 : 1, out);
            };
            ComPtr<ID3D11ClassInstance> selected;
            if (kind && variant == 1) {
                observation = instance(false, true, selected.GetAddressOf());
                checked(observation);
                if (!selected)
                    throw std::runtime_error("Unregistered name did not yield an instance");
                held.emplace_back(selected);
                selected.Reset();
            }
            checked(instance(false, false, selected.GetAddressOf()));
            if (kind && variant == 2) {
                held.emplace_back(selected);
                ComPtr<ID3D11ClassInstance> again;
                checked(instance(false, false, again.GetAddressOf()));
                repeatedIdentity = selected.Get() == again.Get();
                selected = again;
            }
            if (kind == 0 && (variant == 3 || variant == 4))
                shader();
            auto dispatch = [&](ID3D11ClassInstance *obj, UINT expectedValue, unsigned phase) {
                D3D11_CLASS_INSTANCE_DESC desc{};
                obj->GetDesc(&desc);
                char name[256]{};
                SIZE_T size = 256;
                obj->GetInstanceName(name, &size);
                size = 256;
                obj->GetTypeName(name, &size);
                auto buffer = constants.Get();
                c->CSSetConstantBuffers(created ? 2 : 0, 1, &buffer);
                auto view = uav.Get();
                c->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
                c->CSSetShader(cs.Get(), &obj, 1);
                c->Dispatch(1, 1, 1);
                view = nullptr;
                c->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
                c->CopyResource(staging.Get(), data.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                checked(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
                std::vector<uint8_t> bytes(16);
                memcpy(bytes.data(), m.pData, 16);
                c->Unmap(staging.Get(), 0);
                for (unsigned i = 0; i < 4; i++) {
                    UINT v;
                    memcpy(&v, bytes.data() + 4 * i, 4);
                    if (v != expectedValue + i)
                        throw std::runtime_error("Class dispatch bytes mismatch");
                }
                save(output / (L"buffer" + std::to_wstring(phase) + L".bin"), bytes);
            };
            if (kind && variant == 3) {
                ComPtr<ID3D11ClassInstance> alternate;
                checked(instance(true, false, alternate.GetAddressOf()));
                dispatch(created ? selected.Get() : alternate.Get(), created ? 40 : 10, 0);
                if (created)
                    selected = alternate;
            }
            dispatch(selected.Get(), expectedValue, 1);
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
            auto v = srv.Get();
            c->PSSetShaderResources(0, 1, &v);
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
                   << (repeatedIdentity ? "true" : "false") << ",\"observation_hresult\":" << observation
                   << '}';
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
