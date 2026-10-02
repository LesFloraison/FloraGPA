// Development-only unmodified GPA texture transfer fixtures with independent CPU byte oracles.
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cstring>
#include <d3dcompiler.h>
#define wmain UnusedPresentCaptureMain
#include "present_capture_probe.cpp"
#include <d3d11sdklayers.h>
#undef wmain
namespace {
struct Tex {
    UINT dimension = 2, width = 16, height = 16, depth = 1, mips = 2, layers = 2, samples = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
};
struct Sub {
    UINT width, height, depth, row, rows, slice;
    std::vector<uint8_t> bytes;
};
std::vector<Sub> storage(const Tex &t, bool green) {
    std::vector<Sub> result;
    for (UINT layer = 0; layer < t.layers; ++layer)
        for (UINT mip = 0; mip < t.mips; ++mip) {
            UINT w = std::max(1u, t.width >> mip), h = std::max(1u, t.height >> mip),
                 d = std::max(1u, t.depth >> mip);
            const bool bc = t.format == DXGI_FORMAT_BC1_UNORM;
            UINT row = bc ? ((w + 3) / 4) * 8 : w * 4, rows = bc ? (h + 3) / 4 : h;
            Sub sub{w, h, d, row, rows, row * rows, std::vector<uint8_t>(size_t(row) * rows * d)};
            if (green)
                for (size_t i = 0; i < sub.bytes.size(); i += bc ? 8 : 4) {
                    if (bc) {
                        sub.bytes[i] = 0xe0;
                        sub.bytes[i + 1] = 7;
                        sub.bytes[i + 2] = 0xe0;
                        sub.bytes[i + 3] = 7;
                    } else if (t.format == DXGI_FORMAT_R32_TYPELESS) {
                        float v = .25f;
                        memcpy(sub.bytes.data() + i, &v, 4);
                    } else {
                        sub.bytes[i + 1] = 255;
                        sub.bytes[i + 3] = 255;
                    }
                }
            result.push_back(std::move(sub));
        }
    return result;
}
ComPtr<ID3D11Resource> create(ID3D11Device *d, const Tex &t, bool staging = false) {
    const auto usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    UINT bind = staging ? 0 : t.bind, cpu = staging ? D3D11_CPU_ACCESS_READ : 0;
    ComPtr<ID3D11Resource> result;
    if (t.dimension == 1) {
        D3D11_TEXTURE1D_DESC desc{t.width, t.mips, t.layers, t.format, usage, bind, cpu, 0};
        ComPtr<ID3D11Texture1D> r;
        checked(d->CreateTexture1D(&desc, nullptr, &r));
        result = r;
    } else if (t.dimension == 3) {
        D3D11_TEXTURE3D_DESC desc{t.width, t.height, t.depth, t.mips, t.format, usage, bind, cpu, 0};
        ComPtr<ID3D11Texture3D> r;
        checked(d->CreateTexture3D(&desc, nullptr, &r));
        result = r;
    } else {
        D3D11_TEXTURE2D_DESC desc{t.width,        t.height, t.mips, t.layers, t.format,
                                  {t.samples, 0}, usage,    bind,   cpu,      0};
        ComPtr<ID3D11Texture2D> r;
        checked(d->CreateTexture2D(&desc, nullptr, &r));
        result = r;
    }
    return result;
}
std::vector<uint8_t> read(ID3D11DeviceContext *c, ID3D11Resource *r, const std::vector<Sub> &subs) {
    std::vector<uint8_t> bytes;
    for (UINT i = 0; i < subs.size(); ++i) {
        D3D11_MAPPED_SUBRESOURCE m{};
        checked(c->Map(r, i, D3D11_MAP_READ, 0, &m));
        const auto &s = subs[i];
        for (UINT z = 0; z < s.depth; ++z)
            for (UINT y = 0; y < s.rows; ++y) {
                auto p =
                    static_cast<const uint8_t *>(m.pData) + size_t(z) * m.DepthPitch + size_t(y) * m.RowPitch;
                bytes.insert(bytes.end(), p, p + s.row);
            }
        c->Unmap(r, i);
    }
    return bytes;
}
void save(const fs::path &p, const std::vector<uint8_t> &bytes) {
    std::ofstream f(p, std::ios::binary);
    f.exceptions(std::ios::badbit | std::ios::failbit);
    f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 10)
            throw std::runtime_error("Invalid mode");
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("RegisterClass failed");
        ComPtr<ID3D11Device> d;
        ComPtr<ID3D11DeviceContext> c;
        const bool debug = GetEnvironmentVariableW(L"FLORA_PROBE_DEBUG", nullptr, 0) != 0;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  debug ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                  nullptr, &c));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(d.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, d.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        Tex t;
        UINT srcSub = 0, dstSub = 0, x = 0, y = 0, z = 0;
        D3D11_BOX box{};
        bool boxed = false;
        if (mode == 1) {
            srcSub = 3;
            dstSub = 1;
            x = y = 2;
            box = {1, 1, 0, 5, 5, 1};
            boxed = true;
        }
        if (mode == 2) {
            srcSub = 2;
            dstSub = 0;
        }
        if (mode == 3) {
            t.dimension = 3;
            t.width = t.height = t.depth = 8;
            t.layers = 1;
            srcSub = dstSub = 1;
            x = y = z = 1;
            box = {0, 0, 0, 2, 2, 2};
            boxed = true;
        }
        if (mode == 4) {
            t.dimension = 1;
            t.height = 1;
            t.layers = 1;
            srcSub = dstSub = 1;
            x = 2;
            box = {1, 0, 0, 4, 1, 1};
            boxed = true;
        }
        if ((mode == 5 || mode == 10)) {
            t.width = t.height = 8;
            t.layers = 1;
            t.mips = 4;
            t.format = DXGI_FORMAT_BC1_UNORM;
            srcSub = dstSub = 3;
            box = {0, 0, 0, 4, 4, 1};
            boxed = mode == 5;
        }
        if (mode == 6) {
            t.layers = t.mips = 1;
            t.format = DXGI_FORMAT_R32_TYPELESS;
            t.bind |= D3D11_BIND_DEPTH_STENCIL;
        }
        if ((mode >= 7 && mode <= 9)) {
            t.layers = t.mips = 1;
            t.width = t.height = 8;
            if (mode == 9)
                t.format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        }
        Tex sourceDesc = t;
        if ((mode >= 7 && mode <= 9)) {
            sourceDesc.samples = 4;
            sourceDesc.bind = D3D11_BIND_RENDER_TARGET;
        }
        auto source = create(d.Get(), sourceDesc), dest = create(d.Get(), t),
             staging = create(d.Get(), t, true);
        ComPtr<ID3D11Resource> middle;
        ComPtr<ID3D11RenderTargetView> sourceRtv, middleRtv;
        ComPtr<ID3D11DepthStencilView> sourceDsv, destDsv;
        if ((mode >= 7 && mode <= 9)) {
            D3D11_RENDER_TARGET_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            checked(d->CreateRenderTargetView(source.Get(), &v, &sourceRtv));
            if (mode == 7) {
                middle = create(d.Get(), sourceDesc);
                checked(d->CreateRenderTargetView(middle.Get(), &v, &middleRtv));
            }
        }
        if (mode == 6) {
            D3D11_DEPTH_STENCIL_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_D32_FLOAT;
            v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            checked(d->CreateDepthStencilView(source.Get(), &v, &sourceDsv));
            checked(d->CreateDepthStencilView(dest.Get(), &v, &destDsv));
        }
        auto sourceData = storage(t, true), initial = storage(t, false), expected = initial;
        if (mode == 0 || (mode >= 6 && mode <= 9))
            expected = sourceData;
        else if (mode == 2) {
            expected[2] = sourceData[2];
            expected[0] = sourceData[2];
            initial[2] = sourceData[2];
        } else if ((mode == 5 || mode == 10))
            expected[dstSub] = sourceData[srcSub];
        else {
            const auto &s = sourceData[srcSub];
            auto &e = expected[dstSub];
            for (UINT dz = 0; dz < box.back - box.front; ++dz)
                for (UINT dy = 0; dy < box.bottom - box.top; ++dy)
                    memcpy(e.bytes.data() + size_t(z + dz) * e.slice + size_t(y + dy) * e.row + x * 4,
                           s.bytes.data() + size_t(box.front + dz) * s.slice + size_t(box.top + dy) * s.row +
                               box.left * 4,
                           (box.right - box.left) * 4);
        }
        std::vector<uint8_t> expectedBytes;
        for (const auto &s : expected)
            expectedBytes.insert(expectedBytes.end(), s.bytes.begin(), s.bytes.end());
        save(output / L"expected.bin", expectedBytes);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = mode == 6 ? DXGI_FORMAT_R32_FLOAT : mode == 9 ? DXGI_FORMAT_R8G8B8A8_UNORM : t.format;
        UINT mip = dstSub % t.mips, layer = dstSub / t.mips;
        if (t.dimension == 1) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            sd.Texture1D = {mip, 1};
        } else if (t.dimension == 3) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            sd.Texture3D = {mip, 1};
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray = {mip, 1, layer, 1};
        }
        ComPtr<ID3D11ShaderResourceView> srv;
        checked(d->CreateShaderResourceView(dest.Get(), &sd, &srv));
        std::string textureType = t.dimension == 1   ? "Texture1D<float4>"
                                  : t.dimension == 3 ? "Texture3D<float4>"
                                                     : "Texture2DArray<float4>";
        std::string coord = t.dimension == 1   ? "int2(2,0)"
                            : t.dimension == 3 ? "int4(1,1,1,0)"
                            : mode == 1        ? "int4(2,2,0,0)"
                                               : "int4(0,0,0,0)";
        std::string predicate = mode == 6 ? "v.x==0.25" : "all(v==float4(0,1,0,1))";
        std::string shader = textureType +
                             " tex:register(t0);float4 vs(uint i:SV_VertexID):SV_Position{return "
                             "float4(i==2?3:-1,i==1?3:-1,0,1);}float4 ps():SV_Target{float4 v=tex.Load(" +
                             coord + ");return " + predicate + "?float4(0,1,0,1):float4(1,0,1,1);}";
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
        Tex screen;
        screen.width = screen.height = 8;
        screen.layers = screen.mips = 1;
        auto screenStage = create(d.Get(), screen, true);
        auto screenStorage = storage(screen, true);
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"frames\":[";
        bool requested = false;
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
                requested = true;
            }
            c->ClearState();
            const float green[]{0, 1, 0, 1}, magenta[]{1, 0, 1, 1};
            if (mode == 6) {
                c->ClearDepthStencilView(sourceDsv.Get(), D3D11_CLEAR_DEPTH, .25f, 0);
                c->ClearDepthStencilView(destDsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
            } else {
                for (UINT sub = 0; sub < initial.size(); ++sub)
                    c->UpdateSubresource(dest.Get(), sub, nullptr, initial[sub].bytes.data(),
                                         initial[sub].row, initial[sub].slice);
                if ((mode >= 7 && mode <= 9)) {
                    c->ClearRenderTargetView(sourceRtv.Get(), green);
                    if (middleRtv)
                        c->ClearRenderTargetView(middleRtv.Get(), magenta);
                } else
                    for (UINT sub = 0; sub < sourceData.size(); ++sub)
                        c->UpdateSubresource(source.Get(), sub, nullptr, sourceData[sub].bytes.data(),
                                             sourceData[sub].row, sourceData[sub].slice);
            }
            if (mode == 0)
                c->CopyResource(dest.Get(), source.Get());
            else if (mode == 7) {
                c->CopyResource(middle.Get(), source.Get());
                c->ResolveSubresource(dest.Get(), 0, middle.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            } else if ((mode == 8 || mode == 9))
                c->ResolveSubresource(dest.Get(), 0, source.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            else
                c->CopySubresourceRegion(dest.Get(), dstSub, x, y, z, mode == 2 ? dest.Get() : source.Get(),
                                         srcSub, boxed ? &box : nullptr);
            c->CopyResource(staging.Get(), dest.Get());
            auto actual = read(c.Get(), staging.Get(), expected);
            save(output / L"texture.bin", actual);
            if (actual != expectedBytes) {
                if (debug) {
                    ComPtr<ID3D11InfoQueue> queue;
                    if (SUCCEEDED(d.As(&queue))) {
                        std::ofstream messages(output / L"debug.txt");
                        for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
                            SIZE_T size = 0;
                            queue->GetMessage(i, nullptr, &size);
                            std::vector<uint8_t> storage(size);
                            auto m = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
                            if (SUCCEEDED(queue->GetMessage(i, m, &size)))
                                messages << m->ID << ": " << m->pDescription << '\n';
                        }
                    }
                }
                throw std::runtime_error("Texture byte oracle mismatch");
            }
            c->ClearRenderTargetView(chain.rtv.Get(), magenta);
            auto view = chain.rtv.Get();
            c->OMSetRenderTargets(1, &view, nullptr);
            D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
            c->RSSetViewports(1, &vp);
            c->RSSetState(rs.Get());
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(vs.Get(), nullptr, 0);
            c->PSSetShader(ps.Get(), nullptr, 0);
            auto input = srv.Get();
            c->PSSetShaderResources(0, 1, &input);
            c->Draw(3, 0);
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), screenStage.Get(), screenStorage);
            save(output / L"frame.rgba", rgba);
            if (rgba != screenStorage[0].bytes)
                throw std::runtime_error("Texture-dependent image oracle mismatch");
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"bytes_verified\":true,\"image_verified\":true}";
            Sleep(25);
        }
        report << "],\"capture_requested\":" << (requested ? "true" : "false") << ",\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
