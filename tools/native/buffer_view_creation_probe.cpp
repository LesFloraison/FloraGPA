// Original GPA buffer-view fixtures; development only.
#include "original_capture_control.h"
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    // Modes: 0..3 SRV; 10..18 UAV; 20 RTV; 30..35 failed/validation views.
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int scenario = std::stoi(argv[2]);
        if (!((scenario >= 0 && scenario <= 3) || (scenario >= 10 && scenario <= 18) || scenario == 20 ||
              (scenario >= 30 && scenario <= 35)))
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

        const bool repeated = scenario == 16 || scenario == 17;
        const bool unusedCounter = scenario == 18;
        const int mode = (scenario == 16 || unusedCounter) ? 13
                         : scenario == 17 ? 14
                         : scenario < 30  ? scenario
                         : scenario < 32  ? 0
                         : scenario < 34  ? 10
                                          : 20;
        const bool failed = scenario >= 30 && scenario % 2 == 0,
                   validation = scenario >= 30 && scenario % 2 == 1;
        const bool srvOnly = mode < 10, raw = mode == 2 || mode == 11,
                   structured = mode == 1 || mode == 3 || (mode >= 12 && mode <= 15),
                   defaults = mode == 3 || mode == 15, counter = (mode == 13 || mode == 14) && !unusedCounter;
        D3D11_BUFFER_DESC bd{64, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        if (mode >= 10)
            bd.BindFlags |= mode == 20 ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = raw          ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                       : structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                    : 0;
        bd.StructureByteStride = structured ? 4 : 0;
        ComPtr<ID3D11Buffer> buffer, stage, countBuffer, countStage;
        checked(d->CreateBuffer(&bd, nullptr, &buffer));
        auto staging = bd;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = staging.StructureByteStride = 0;
        checked(d->CreateBuffer(&staging, nullptr, &stage));
        D3D11_BUFFER_DESC cd{4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        checked(d->CreateBuffer(&cd, nullptr, &countBuffer));
        cd.Usage = D3D11_USAGE_STAGING;
        cd.BindFlags = 0;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked(d->CreateBuffer(&cd, nullptr, &countStage));
        std::array<uint32_t, 16> initial{}, expected{};
        for (UINT i = 0; i < 16; ++i)
            initial[i] = 0x11110000u + i;
        expected = initial;
        const UINT first = defaults ? 0 : 4, count = defaults ? 16 : 8,
                   sampleIndex = unusedCounter ? 5
                                 : counter      ? 6
                                 : mode == 12 ? 5
                                 : defaults   ? 0
                                              : 4;
        if (mode == 10 || mode == 11 || mode == 20)
            for (UINT i = first; i < first + count; ++i)
                expected[i] = mode == 20 ? 0x3f800000u : 123;
        if (mode >= 12 && mode <= 15)
            expected[sampleIndex] = 123;
        if (repeated)
            expected[7] = 123;
        save(output / L"expected.bin",
             std::vector<uint8_t>(reinterpret_cast<uint8_t *>(expected.data()),
                                  reinterpret_cast<uint8_t *>(expected.data()) + 64));
        auto srvDesc = [&](bool full) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = raw          ? DXGI_FORMAT_R32_TYPELESS
                        : structured ? DXGI_FORMAT_UNKNOWN
                        : mode == 20 ? DXGI_FORMAT_R32_FLOAT
                                     : DXGI_FORMAT_R32_UINT;
            sd.ViewDimension = raw ? D3D11_SRV_DIMENSION_BUFFEREX : D3D11_SRV_DIMENSION_BUFFER;
            sd.BufferEx = {full ? 0u : first, full ? 16u : count, raw ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0u};
            return sd;
        };
        ComPtr<ID3D11ShaderResourceView> input, countSrv;
        if (!srvOnly) {
            auto sd = srvDesc(true);
            checked(d->CreateShaderResourceView(buffer.Get(), &sd, &input));
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC csd{};
        csd.Format = DXGI_FORMAT_R32_UINT;
        csd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        csd.Buffer.NumElements = 1;
        checked(d->CreateShaderResourceView(countBuffer.Get(), &csd, &countSrv));
        std::string type = raw          ? "ByteAddressBuffer"
                           : structured ? "StructuredBuffer<uint>"
                           : mode == 20 ? "Buffer<float>"
                                        : "Buffer<uint>";
        const UINT lookup = srvOnly ? 0 : sampleIndex;
        std::string load =
            raw ? "tex.Load(" + std::to_string(lookup * 4) + ")" : "tex[" + std::to_string(lookup) + "]";
        const std::string value = srvOnly      ? std::to_string(expected[first]) + "u"
                                  : mode == 20 ? "1.0"
                                               : "123u";
        std::string shader =
            "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}" + type +
            " tex:register(t0);Buffer<uint> counts:register(t1);float4 ps():SV_Target{return (" + load +
            "==" + value + (counter ? (repeated ? " && counts[0]==4" : " && counts[0]==3") : "") +
            ")?float4(0,1,0,1):float4(1,0,1,1);}";
        auto compile = [&](const std::string &source, const char *entry, const char *profile) {
            ComPtr<ID3DBlob> b, errors;
            auto hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, profile, 0,
                                 0, &b, &errors);
            if (FAILED(hr) && errors)
                throw std::runtime_error(static_cast<const char *>(errors->GetBufferPointer()));
            checked(hr);
            return b;
        };
        auto vsCode = compile(shader, "vs", "vs_5_0"), psCode = compile(shader, "ps", "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11ComputeShader> cs;
        checked(d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        if (mode >= 12 && mode <= 15) {
            std::string source =
                mode == 14
                    ? "AppendStructuredBuffer<uint> dest:register(u0);[numthreads(1,1,1)]void "
                      "main(){dest.Append(123);}"
                    : std::string(
                          "RWStructuredBuffer<uint> dest:register(u0);[numthreads(1,1,1)]void main(){dest[") +
                          (mode == 13 && !unusedCounter ? "dest.IncrementCounter()"
                           : (mode == 12 || unusedCounter) ? "1"
                                        : "0") +
                          "]=123;}";
            auto code = compile(source, "main", "cs_5_0");
            checked(d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
        }
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
        DXGI_ADAPTER_DESC ad{};
        checked(adapter->GetDesc(&ad));
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << scenario << ",\"vendor_id\":" << ad.VendorId
               << ",\"device_id\":" << ad.DeviceId << ",\"frames\":[";
        auto readBuffer = [&](ID3D11Buffer *source, ID3D11Buffer *staging, UINT size) {
            c->CopyResource(staging, source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(c->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
            std::vector<uint8_t> b(static_cast<uint8_t *>(mapped.pData),
                                   static_cast<uint8_t *>(mapped.pData) + size);
            c->Unmap(staging, 0);
            return b;
        };
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
                flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                request(argv[3], captured);
            }
            c->ClearState();
            c->UpdateSubresource(buffer.Get(), 0, nullptr, initial.data(), 0, 0);
            UINT zero = 0;
            c->UpdateSubresource(countBuffer.Get(), 0, nullptr, &zero, 0, 0);
            auto make = [&]<class View, class Desc>(auto method, Desc desc, ComPtr<View> &result) {
                if (failed || validation) {
                    auto bad = desc;
                    if (failed)
                        reinterpret_cast<UINT *>(&bad)[3] = 99;
                    ComPtr<View> absent;
                    auto hr =
                        (d.Get()->*method)(buffer.Get(), &bad, failed ? absent.GetAddressOf() : nullptr);
                    if ((failed && (SUCCEEDED(hr) || absent)) || (validation && hr != S_FALSE))
                        throw std::runtime_error("Unexpected failed/validation result");
                }
                checked((d.Get()->*method)(buffer.Get(), defaults ? nullptr : &desc, result.GetAddressOf()));
                Desc actual{};
                result->GetDesc(&actual);
                ComPtr<ID3D11Resource> parent;
                result->GetResource(&parent);
                if (parent.Get() != buffer.Get())
                    throw std::runtime_error("Wrong view parent");
            };
            ComPtr<ID3D11ShaderResourceView> late;
            ComPtr<ID3D11UnorderedAccessView> uav;
            ComPtr<ID3D11RenderTargetView> rtv;
            if (srvOnly) {
                make(&ID3D11Device::CreateShaderResourceView, srvDesc(false), late);
            } else if (mode == 20) {
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT_R32_FLOAT;
                desc.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
                desc.Buffer = {first, count};
                make(&ID3D11Device::CreateRenderTargetView, desc, rtv);
                const float color[]{1, 0, 0, 0};
                c->ClearRenderTargetView(rtv.Get(), color);
            } else {
                D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
                desc.Format = raw          ? DXGI_FORMAT_R32_TYPELESS
                              : structured ? DXGI_FORMAT_UNKNOWN
                                           : DXGI_FORMAT_R32_UINT;
                desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                desc.Buffer = {first, count,
                               raw          ? D3D11_BUFFER_UAV_FLAG_RAW
                               : mode == 13 ? D3D11_BUFFER_UAV_FLAG_COUNTER
                               : mode == 14 ? D3D11_BUFFER_UAV_FLAG_APPEND
                                            : 0u};
                make(&ID3D11Device::CreateUnorderedAccessView, desc, uav);
                if (cs) {
                    auto bound = uav.Get();
                    UINT initialCount = counter ? 2 : UINT_MAX;
                    c->CSSetUnorderedAccessViews(0, 1, &bound, &initialCount);
                    c->CSSetShader(cs.Get(), nullptr, 0);
                    c->Dispatch(1, 1, 1);
                    if (repeated) {
                        UINT keep = UINT_MAX;
                        c->CSSetUnorderedAccessViews(0, 1, &bound, &keep);
                        c->Dispatch(1, 1, 1);
                    }
                    bound = nullptr;
                    UINT keep = UINT_MAX;
                    c->CSSetUnorderedAccessViews(0, 1, &bound, &keep);
                    if (counter) {
                        c->CopyStructureCount(countBuffer.Get(), 0, uav.Get());
                        auto bytes = readBuffer(countBuffer.Get(), countStage.Get(), 4);
                        uint32_t value;
                        memcpy(&value, bytes.data(), 4);
                        if (value != (repeated ? 4u : 3u))
                            throw std::runtime_error("Counter oracle mismatch");
                        save(output / L"counter.bin", bytes);
                    }
                } else {
                    const UINT value[]{123, 0, 0, 0};
                    c->ClearUnorderedAccessViewUint(uav.Get(), value);
                }
            }
            auto bytes = readBuffer(buffer.Get(), stage.Get(), 64);
            if (memcmp(bytes.data(), expected.data(), 64))
                throw std::runtime_error("Buffer oracle mismatch");
            save(output / L"buffer.bin", bytes);
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
            ID3D11ShaderResourceView *inputs[]{srvOnly ? late.Get() : input.Get(), countSrv.Get()};
            c->PSSetShaderResources(0, 2, inputs);
            c->Draw(3, 0);
            c->CopyResource(screenStage.Get(), chain.buffer.Get());
            auto rgba = read(c.Get(), screenStage.Get(), screenStorage);
            if (rgba != screenStorage[0].bytes)
                throw std::runtime_error("View-dependent image mismatch");
            save(output / L"frame.rgba", rgba);
            checked(chain.swap->Present(0, 0));
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"bytes_verified\":true,\"image_verified\":true}";
            Sleep(25);
        }
        report << "],\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
