// Self-owned original SO binding/lifetime corpus. Development only.
#include "original_capture_control.h"
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
        if (mode < 0 || mode > 8)
            throw std::runtime_error("Invalid SO lifetime mode");
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
        unsigned guardChecks = 0;
        for (auto module : {HMODULE{}, GetModuleHandleW(L"kernel32.dll")}) {
            try {
                flora::research::selectOriginalPrimarySwapChain(module, chain.swap.Get());
            } catch (const std::runtime_error &) {
                ++guardChecks;
            }
        }
        if (guardChecks != 2)
            throw std::runtime_error("Original capture identity guard accepted a wrong module");
        const std::array<uint8_t, 64> zero{};
        D3D11_SUBRESOURCE_DATA initial{zero.data(), 0, 0};
        D3D11_BUFFER_DESC desc{64, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        ComPtr<ID3D11Buffer> missing[2], known, staging;
        for (auto &buffer : missing)
            checked(d->CreateBuffer(&desc, &initial, &buffer));
        desc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        checked(d->CreateBuffer(&desc, &initial, &known));
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked(d->CreateBuffer(&desc, nullptr, &staging));
        auto compile = [&](const std::string &source, const char *entry, const char *profile) {
            ComPtr<ID3DBlob> blob, errors;
            checked(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, profile, 0, 0,
                               &blob, &errors));
            return blob;
        };
        const std::string code =
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}"
            "float4 ps():SV_Target{return float4(1,0,0,1);}";
        auto vsCode = compile(code, "vs", "vs_5_0");
        auto psCode = compile(code, "ps", "ps_5_0");
        auto gsCode = compile("struct V{float4 p:SV_Position;};[maxvertexcount(1)] "
                              "void gs(point V input[1],inout PointStream<V> s){s.Append(input[0]);}",
                              "gs", "gs_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11GeometryShader> gs;
        checked(d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        const D3D11_SO_DECLARATION_ENTRY declaration{0, "SV_Position", 0, 0, 4, 0};
        const UINT stride = 16;
        checked(d->CreateGeometryShaderWithStreamOutput(gsCode->GetBufferPointer(), gsCode->GetBufferSize(),
                                                        &declaration, 1, &stride, 1,
                                                        D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs));
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        checked(d->CreateRasterizerState(&raster, &rs));
        ComPtr<ID3D11Query> soQuery;
        const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_SO_STATISTICS, 0};
        checked(d->CreateQuery(&queryDesc, &soQuery));
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        auto screenStage = create(d.Get(), screen, true);
        auto pixels = storage(screen, false);
        for (size_t p = 0; p < pixels[0].bytes.size(); p += 4) {
            pixels[0].bytes[p] = pixels[0].bytes[p + 3] = 255;
            pixels[0].bytes[p + 1] = pixels[0].bytes[p + 2] = 0;
        }
        save(output / L"expected.rgba", pixels[0].bytes);
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"identity_guard_checks\":" << guardChecks << ",\"frames\":[";
        bool selected = false;
        auto graphics = [&] {
            auto target = chain.rtv.Get();
            c->OMSetRenderTargets(1, &target, nullptr);
            const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &viewport);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->GSSetShader(nullptr, nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
        };
        for (unsigned frame = 0; frame < 12; ++frame) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (argc == 5 && frame == 5) {
                auto module = GetModuleHandleW(L"shimd3d64.dll");
                selected = flora::research::selectOriginalPrimarySwapChain(module, chain.swap.Get());
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(module, "CaptureNextFrame"));
                request(argv[3], captured);
            }
            c->ClearState();
            const float black[]{0, 0, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), black);
            graphics();
            const bool mixed = mode == 6;
            ID3D11Buffer *targets[]{mode == 7 || mixed ? known.Get() : missing[0].Get(),
                                    mixed ? missing[0].Get() : missing[1].Get()};
            UINT offsets[]{mode == 7 || mixed ? 16u : 4u, mixed ? 4u : 20u};
            UINT count = mode == 2 || mixed ? 2u : 1u;
            c->SOSetTargets(count, targets, offsets);
            auto observe = [&](UINT expectedCount) {
                ID3D11Buffer *observed[4]{};
                c->SOGetTargets(4, observed);
                bool equal = true;
                for (UINT i = 0; i < 4; ++i) {
                    equal &= observed[i] == (i < expectedCount ? targets[i] : nullptr);
                    if (observed[i])
                        observed[i]->Release();
                }
                if (!equal)
                    throw std::runtime_error("SO getter identity mismatch");
            };
            observe(count);
            if (mode == 3) {
                offsets[0] = 20;
                c->SOSetTargets(1, targets, offsets);
                observe(1);
            }
            if (mixed) {
                const UINT append = UINT_MAX;
                c->SOSetTargets(1, targets, &append);
                observe(1);
            }
            D3D11_QUERY_DATA_SO_STATISTICS statistics{};
            if (mode >= 6) {
                c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
                c->GSSetShader(gs.Get(), nullptr, 0);
                c->Begin(soQuery.Get());
                c->Draw(1, 0);
                c->End(soQuery.Get());
                bool ready = false;
                for (unsigned poll = 0; poll < 10000; ++poll) {
                    auto hr = c->GetData(soQuery.Get(), &statistics, sizeof(statistics), 0);
                    checked(hr);
                    if (hr == S_OK) {
                        ready = true;
                        break;
                    }
                    Sleep(1);
                }
                if (!ready || statistics.NumPrimitivesWritten != 1 || statistics.PrimitivesStorageNeeded != 1)
                    throw std::runtime_error("SO statistics oracle mismatch or timeout");
            }
            if (mode == 1) {
                c->ClearState();
            } else if (mode == 4) {
                ID3D11Buffer *nulls[4]{};
                const UINT resets[4]{};
                c->SOSetTargets(4, nulls, resets);
            } else {
                c->SOSetTargets(0, nullptr, nullptr);
            }
            if (mode >= 5) {
                c->CopyResource(staging.Get(), targets[0]);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                std::vector<uint8_t> actual(64);
                std::memcpy(actual.data(), mapped.pData, actual.size());
                c->Unmap(staging.Get(), 0);
                auto expected = std::vector<uint8_t>(64);
                if (mode >= 6) {
                    const float position[]{-1, -1, 0, 1};
                    std::memcpy(expected.data() + offsets[0], position, sizeof(position));
                }
                if (actual != expected)
                    throw std::runtime_error("SO cursor/storage oracle mismatch");
                save(output / L"so.bin", actual);
            }
            graphics();
            c->Draw(3, 0);
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            const auto rgba = read(c.Get(), screenStage.Get(), pixels);
            save(output / L"frame.rgba", rgba);
            if (rgba != pixels[0].bytes)
                throw std::runtime_error("Final SO probe image mismatch");
            const auto present = chain.swap->Present(0, 0);
            checked(present);
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame
                   << ",\"image_verified\":true,\"getter_checks\":" << (mode == 3 || mixed ? 2 : 1)
                   << ",\"storage_verified\":" << (mode >= 5 ? "true" : "false")
                   << ",\"so_primitives_written\":" << statistics.NumPrimitivesWritten
                   << ",\"so_primitives_needed\":" << statistics.PrimitivesStorageNeeded
                   << ",\"present_status\":" << uint32_t(present) << '}';
            Sleep(50);
        }
        if (argc == 5) {
            for (unsigned i = 0; i < 100 && !fs::exists(argv[3]); ++i)
                Sleep(50);
            if (!fs::exists(argv[3]) || !fs::file_size(argv[3]))
                throw std::runtime_error("Original workload completed without delivering a capture");
        }
        report << "],\"primary_selected_by_original_method\":" << (selected ? "true" : "false")
               << ",\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
