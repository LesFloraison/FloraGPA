// Development-only originals; defined storage after discard has an independent CPU oracle.
#include "original_capture_control.h"
#include "texture_probe_helpers.h"
#include <d3d11_1.h>
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 16)
            throw std::runtime_error("Invalid discard mode");
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
        if (auto shim = GetModuleHandleW(L"shimd3d64.dll")) {
            std::ofstream addresses(output / "wrapper-rvas.txt");
            auto table = *reinterpret_cast<void ***>(c1.Get());
            for (auto slot : {117, 118, 133}) {
                HMODULE owner{};
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(table[slot]), &owner);
                addresses << slot << ' ' << std::hex << (uintptr_t(table[slot]) - uintptr_t(shim))
                          << " pinned_module " << (owner == shim) << std::dec << '\n';
            }
        }
        const bool buffer = mode == 2 || mode == 13, depth = mode == 6, array = mode == 12;
        Tex t;
        t.width = t.height = 8;
        t.mips = t.layers = array ? 2 : 1;
        t.bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        if (depth) {
            t.format = DXGI_FORMAT_R32_TYPELESS;
            t.bind = D3D11_BIND_DEPTH_STENCIL;
        }
        if (mode == 14) {
            t.dimension = 1;
            t.height = 1;
        }
        if (mode == 15) {
            t.dimension = 3;
            t.depth = 4;
        }
        auto seed = storage(t, false), final = seed;
        if (buffer)
            seed = final = {{16, 1, 1, 64, 1, 64, std::vector<uint8_t>(64)}};
        for (size_t sub = 0; sub < seed.size(); ++sub)
            for (size_t byte = 0; byte < seed[sub].bytes.size(); ++byte) {
                seed[sub].bytes[byte] = uint8_t((byte * 7 + sub * 11 + 9) % 251);
                final[sub].bytes[byte] = uint8_t((byte * 13 + sub * 17 + 83) % 251);
            }
        if (depth)
            for (size_t at = 0; at < seed[0].bytes.size(); at += 4) {
                const float before = .75f, after = .25f;
                memcpy(seed[0].bytes.data() + at, &before, 4);
                memcpy(final[0].bytes.data() + at, &after, 4);
            }
        ComPtr<ID3D11Resource> target, staging;
        if (buffer) {
            D3D11_BUFFER_DESC desc{64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
            ComPtr<ID3D11Buffer> b;
            checked(d->CreateBuffer(&desc, nullptr, &b));
            target = b;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            b.Reset();
            checked(d->CreateBuffer(&desc, nullptr, &b));
            staging = b;
        } else {
            target = create(d.Get(), t);
            staging = create(d.Get(), t, true);
        }
        ComPtr<ID3D11View> view;
        ComPtr<ID3D11DepthStencilView> dsv;
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_D32_FLOAT;
            desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            checked(d->CreateDepthStencilView(target.Get(), &desc, &dsv));
            view = dsv;
        } else if (mode == 4) {
            ComPtr<ID3D11ShaderResourceView> srv;
            checked(d->CreateShaderResourceView(target.Get(), nullptr, &srv));
            view = srv;
        } else if (mode == 5 || mode == 13) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = buffer ? DXGI_FORMAT_R32_UINT : t.format;
            desc.ViewDimension = buffer ? D3D11_UAV_DIMENSION_BUFFER : D3D11_UAV_DIMENSION_TEXTURE2D;
            if (buffer)
                desc.Buffer.NumElements = 16;
            ComPtr<ID3D11UnorderedAccessView> uav;
            checked(d->CreateUnorderedAccessView(target.Get(), &desc, &uav));
            view = uav;
        } else if (!buffer && (mode < 14 || mode == 16)) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = t.format;
            desc.ViewDimension = array ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
            if (array)
                desc.Texture2DArray = {1, 1, 1};
            ComPtr<ID3D11RenderTargetView> rtv;
            checked(d->CreateRenderTargetView(target.Get(), &desc, &rtv));
            view = rtv;
        }
        std::vector<D3D11_RECT> rects;
        if (mode == 7)
            rects = {{2, 1, 6, 7}};
        if (mode == 8)
            rects = {{1, 1, 5, 5}, {3, 3, 7, 7}};
        if (mode == 11 || mode == 16)
            rects = {{2, 2, 2, 6}}; // Explicit empty rectangle: outside bytes remain defined.
        if (mode == 13)
            rects = {{4, 0, 12, 1}};
        auto expected = seed;
        if (rects.empty()) {
            if (array)
                expected[3] = final[3];
            else
                expected = final;
        } else {
            for (auto rect : rects)
                for (LONG y = rect.top; y < rect.bottom; ++y)
                    for (LONG x = rect.left; x < rect.right; ++x) {
                        auto at = (size_t(y) * expected[0].width + size_t(x)) * 4;
                        std::copy_n(final[0].bytes.begin() + at, 4, expected[0].bytes.begin() + at);
                    }
        }
        std::vector<uint8_t> expectedBytes;
        for (auto &sub : expected)
            expectedBytes.insert(expectedBytes.end(), sub.bytes.begin(), sub.bytes.end());
        save(output / "expected.bin", expectedBytes);
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        const bool imageFromTarget = !buffer && !depth && !array && (mode < 14 || mode == 16);
        const auto image = imageFromTarget ? expected[0].bytes : storage(screen, true)[0].bytes;
        save(output / "expected.rgba", image);
        auto screenStage = create(d.Get(), screen, true);
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11RasterizerState> rasterizer;
        if (mode == 4) {
            auto compile = [&](const char *source, const char *profile) {
                ComPtr<ID3DBlob> code, error;
                checked(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0,
                                   &code, &error));
                return code;
            };
            const auto vs = compile(
                "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                "vs_5_0");
            const auto ps = compile("Texture2D<float4> t:register(t0);float4 main(float4 "
                                    "p:SV_Position):SV_Target{return t.Load(int3(p.xy,0));}",
                                    "ps_5_0");
            checked(d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
            checked(d->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            checked(d->CreateRasterizerState(&rd, &rasterizer));
        }
        DXGI_ADAPTER_DESC ad{};
        checked(adapter->GetDesc(&ad));
        std::ofstream report(output / "oracle.json");
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
                flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(shim, "CaptureNextFrame"));
                if (!request)
                    throw std::runtime_error("CaptureNextFrame unavailable");
                request(argv[3], captured);
            }
            c->ClearState();
            const float green[]{0, 1, 0, 1};
            c->ClearRenderTargetView(chain.rtv.Get(), green);
            // A view used only by Discard can have an ID but no saved descriptor.
            // Real GPU use materializes the view; the earlier missing-view originals remain separate.
            if (view && mode != 4 && !depth && mode != 0) {
                if (mode == 5 || mode == 13) {
                    ComPtr<ID3D11UnorderedAccessView> uav;
                    checked(view.As(&uav));
                    const UINT zero[4]{};
                    c->ClearUnorderedAccessViewUint(uav.Get(), zero);
                } else {
                    ComPtr<ID3D11RenderTargetView> rtv;
                    checked(view.As(&rtv));
                    c->ClearRenderTargetView(rtv.Get(), green);
                }
            }
            for (UINT sub = 0; sub < seed.size(); ++sub)
                if (depth)
                    c->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, .75f, 0);
                else
                    c->UpdateSubresource(target.Get(), sub, nullptr, seed[sub].bytes.data(), seed[sub].row,
                                         seed[sub].slice);
            if (mode == 4) {
                ComPtr<ID3D11ShaderResourceView> srv;
                checked(view.As(&srv));
                auto input = srv.Get();
                c->PSSetShaderResources(0, 1, &input);
                c->VSSetShader(vertex.Get(), nullptr, 0);
                c->PSSetShader(pixel.Get(), nullptr, 0);
                c->RSSetState(rasterizer.Get());
                const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
                c->RSSetViewports(1, &viewport);
                c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                auto rt = chain.rtv.Get();
                c->OMSetRenderTargets(1, &rt, nullptr);
                c->Draw(3, 0);
                c->CopyResource(screenStage.Get(), chain.buffer.Get());
                const auto observed = read(c.Get(), screenStage.Get(), storage(screen, true));
                save(output / "sampled-before-discard.rgba", observed);
                if (observed != seed[0].bytes)
                    throw std::runtime_error("Pre-discard SRV draw differs from source bytes");
                c->ClearState();
                c->ClearRenderTargetView(chain.rtv.Get(), green);
            }
            if (mode == 1 || mode == 2 || mode == 14 || mode == 15)
                c1->DiscardResource(target.Get());
            else if (mode >= 3 && mode <= 6 || mode == 12)
                c1->DiscardView(view.Get());
            else if (mode >= 7)
                c1->DiscardView1(view.Get(), rects.empty() ? nullptr : rects.data(),
                                 mode == 10   ? 2
                                 : mode == 16 ? 0
                                              : UINT(rects.size()));
            if (depth)
                c->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, .25f, 0);
            else if (rects.empty()) {
                for (UINT sub = 0; sub < final.size(); ++sub)
                    if (!array || sub == 3)
                        c->UpdateSubresource(target.Get(), sub, nullptr, final[sub].bytes.data(),
                                             final[sub].row, final[sub].slice);
            } else {
                for (auto rect : rects) {
                    if (rect.left == rect.right || rect.top == rect.bottom)
                        continue;
                    D3D11_BOX box{UINT(rect.left) * (buffer ? 4u : 1u),  UINT(rect.top),    0,
                                  UINT(rect.right) * (buffer ? 4u : 1u), UINT(rect.bottom), 1};
                    auto at = size_t(rect.top) * final[0].row + size_t(rect.left) * 4;
                    c->UpdateSubresource(target.Get(), 0, &box, final[0].bytes.data() + at, final[0].row,
                                         final[0].slice);
                }
            }
            c->CopyResource(staging.Get(), target.Get());
            auto actual = read(c.Get(), staging.Get(), seed);
            save(output / "actual.bin", actual);
            if (actual != expectedBytes)
                throw std::runtime_error("Defined bytes after discard/rewrite differ from CPU oracle");
            if (imageFromTarget)
                c->CopyResource(chain.buffer.Get(), target.Get());
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), screenStage.Get(), storage(screen, true));
            save(output / "frame.rgba", rgba);
            if (rgba != image)
                throw std::runtime_error("Discard output image differs from CPU oracle");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"storage_verified\":true,\"image_verified\":true}";
            report.flush();
            Sleep(50);
        }
        if (argc == 5 && (!fs::exists(argv[3]) || !fs::file_size(argv[3])))
            throw std::runtime_error("Original workload completed without capture delivery");
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / "error.txt") << error.what();
        return 1;
    }
}
