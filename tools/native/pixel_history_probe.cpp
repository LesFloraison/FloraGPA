// Independent draw observations and optional original GPA capture; research only.
#include "original_capture_control.h"
#include "texture_probe_helpers.h"
#include <nlohmann/json.hpp>
using Json = nlohmann::json;
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    Json report{{"schema", 1},
                {"scope", "independent_pixel_history_draw_observations"},
                {"completed", false},
                {"frames", Json::array()}};
    const auto saveReport = [&] { std::ofstream(output / "observations.json") << report.dump(2) << '\n'; };
    try {
        const auto vendor = unsigned(std::stoul(argv[2]));
        if (vendor != 0 && vendor != 4318 && vendor != 32902)
            throw std::runtime_error("Unsupported explicit adapter");
        report["requested_vendor"] = vendor;
        report["capture_requested"] = argc == 5;
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load original research shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("Register probe window failed");
        ComPtr<IDXGIFactory1> enumeration;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&enumeration)));
        ComPtr<IDXGIAdapter1> selected;
        if (vendor) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                auto hr = enumeration->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                checked(hr);
                DXGI_ADAPTER_DESC1 desc{};
                checked(candidate->GetDesc1(&desc));
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested adapter unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                  nullptr, argc == 5 ? 0 : D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_ADAPTER_DESC desc{};
        checked(adapter->GetDesc(&desc));
        report["adapter"] = {{"vendor", desc.VendorId},
                             {"device", desc.DeviceId},
                             {"luid", uint64_t(uint32_t(desc.AdapterLuid.LowPart)) |
                                          uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32}};
        Chain chain;
        createChain(chain, device.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        auto compile = [&](const std::string &source, const char *profile) {
            ComPtr<ID3DBlob> code, errors;
            const auto hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main",
                                       profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr) && errors)
                throw std::runtime_error(static_cast<const char *>(errors->GetBufferPointer()));
            checked(hr);
            return code;
        };
        const auto vsCode = compile("cbuffer P:register(b0){float4 color;float z;}"
                                    "float4 main(uint n:SV_VertexID):SV_Position{uint i=n%3;"
                                    "return float4(i==2?3:-1,i==1?3:-1,z,1);}",
                                    "vs_5_0");
        const auto psCode = compile("cbuffer P:register(b0){float4 color;float z;uint reject;}"
                                    "float4 main():SV_Target{if(reject)discard;return color;}",
                                    "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        checked(
            device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        screen.bind = D3D11_BIND_RENDER_TARGET;
        auto color = create(device.Get(), screen);
        auto stage = create(device.Get(), screen, true);
        ComPtr<ID3D11RenderTargetView> rtv;
        checked(device->CreateRenderTargetView(color.Get(), nullptr, &rtv));
        D3D11_TEXTURE2D_DESC depthDesc{
            8, 8, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL,
            0, 0};
        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> dsv;
        checked(device->CreateTexture2D(&depthDesc, nullptr, &depth));
        checked(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
        const std::array<const char *, 12> names{
            "write",  "unchanged_write", "scissor_reject", "depth_reject",    "stencil_reject",   "discard",
            "culled", "clipped",         "blend",          "color_mask_zero", "sample_mask_zero", "overlap"};
        std::array<ComPtr<ID3D11Buffer>, 12> params;
        std::array<ComPtr<ID3D11RasterizerState>, 12> raster;
        std::array<ComPtr<ID3D11DepthStencilState>, 12> ds;
        std::array<ComPtr<ID3D11BlendState>, 12> blend;
        for (unsigned i = 0; i < names.size(); ++i) {
            struct Params {
                float color[4];
                float z;
                UINT reject;
                UINT pad[2];
            } p{{1, 0, 0, 1}, .5f, i == 5, {}};
            if (i == 1) {
                p.color[0] = 0;
                p.color[2] = 1;
            }
            if (i == 7)
                p.z = 2;
            if (i == 8)
                p.color[3] = .5f;
            D3D11_BUFFER_DESC bd{sizeof(p), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA data{&p, 0, 0};
            checked(device->CreateBuffer(&bd, &data, &params[i]));
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = i == 6 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
            rd.FrontCounterClockwise = FALSE;
            rd.DepthClipEnable = TRUE;
            rd.ScissorEnable = i == 2;
            checked(device->CreateRasterizerState(&rd, &raster[i]));
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = i == 3;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_LESS;
            dd.StencilEnable = i == 4;
            dd.StencilReadMask = 255;
            dd.StencilWriteMask = 0;
            dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                          D3D11_COMPARISON_EQUAL};
            checked(device->CreateDepthStencilState(&dd, &ds[i]));
            D3D11_BLEND_DESC b{};
            b.RenderTarget[0] = {
                i == 8,          D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                D3D11_BLEND_ONE, D3D11_BLEND_ZERO,      D3D11_BLEND_OP_ADD,        BYTE(i == 9 ? 0 : 15)};
            checked(device->CreateBlendState(&b, &blend[i]));
        }
        ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        checked(device->CreateQuery(&qd, &query));
        for (unsigned frame = 0; frame < 8; ++frame) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (argc == 5 && frame == 4) {
                auto shim = GetModuleHandleW(L"shimd3d64.dll");
                flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(shim, "CaptureNextFrame"));
                if (!request)
                    throw std::runtime_error("Original capture entry unavailable");
                request(argv[3], captured);
            }
            Json rows = Json::array();
            for (unsigned i = 0; i < names.size(); ++i) {
                context->ClearState();
                const float blue[]{0, 0, 1, 1};
                context->ClearRenderTargetView(rtv.Get(), blue);
                context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .25f, 0);
                auto target = rtv.Get();
                context->OMSetRenderTargets(1, &target, dsv.Get());
                auto cb = params[i].Get();
                context->VSSetConstantBuffers(0, 1, &cb);
                context->PSSetConstantBuffers(0, 1, &cb);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->PSSetShader(ps.Get(), nullptr, 0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                const D3D11_VIEWPORT vp{0, 0, 8, 8, 0, 1};
                context->RSSetViewports(1, &vp);
                const D3D11_RECT scissor{0, 0, 2, 2};
                context->RSSetScissorRects(1, &scissor);
                context->RSSetState(raster[i].Get());
                context->OMSetDepthStencilState(ds[i].Get(), 1);
                context->OMSetBlendState(blend[i].Get(), nullptr, i == 10 ? 0 : UINT_MAX);
                context->Begin(query.Get());
                context->Draw(i == 11 ? 6 : 3, 0);
                context->End(query.Get());
                uint64_t samples = 0;
                HRESULT hr;
                const auto deadline = GetTickCount64() + 5000;
                while ((hr = context->GetData(query.Get(), &samples, sizeof(samples), 0)) == S_FALSE) {
                    if (GetTickCount64() > deadline)
                        throw std::runtime_error("Pixel history reference timeout");
                    Sleep(1);
                }
                checked(hr);
                context->CopyResource(stage.Get(), color.Get());
                const auto rgba = read(context.Get(), stage.Get(), storage(screen, false));
                const bool rejected = (i >= 2 && i <= 7) || i == 10;
                const uint64_t expectedSamples = i == 2 ? 4 : rejected ? 0 : i == 11 ? 128 : 64;
                if (samples != expectedSamples)
                    throw std::runtime_error("Unexpected reference coverage: " + std::string(names[i]) + " " +
                                             std::to_string(samples));
                for (unsigned y = 0; y < 8; ++y)
                    for (unsigned x = 0; x < 8; ++x) {
                        const bool write = (!rejected || (i == 2 && x < 2 && y < 2)) && i != 1 && i != 9;
                        const std::array<unsigned, 4> expected =
                            i == 8  ? std::array<unsigned, 4>{128, 0, 128, 128}
                            : write ? std::array<unsigned, 4>{255, 0, 0, 255}
                                    : std::array<unsigned, 4>{0, 0, 255, 255};
                        for (unsigned c = 0; c < 4; ++c)
                            if (std::abs(int(rgba[(y * 8 + x) * 4 + c]) - int(expected[c])) >
                                (i == 8 ? 1 : 0))
                                throw std::runtime_error("Unexpected reference pixel: " +
                                                         std::string(names[i]));
                    }
                save(output / ("frame" + std::to_string(frame) + "-" + names[i] + ".rgba"), rgba);
                rows.push_back(
                    {{"case", i},
                     {"name", names[i]},
                     {"samples", samples},
                     {"expected_samples", expectedSamples},
                     {"selected_pixel", std::vector<uint8_t>(rgba.begin() + 144, rgba.begin() + 148)},
                     {"image_verified", true}});
            }
            context->CopyResource(chain.buffer.Get(), color.Get());
            checked(chain.swap->Present(0, 0));
            report["frames"].push_back({{"frame", frame}, {"cases", rows}});
            saveReport();
            Sleep(25);
        }
        if (argc != 5) {
            ComPtr<ID3D11InfoQueue> diagnostics;
            checked(device.As(&diagnostics));
            const auto count = diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
            std::ofstream log(output / "debug.txt");
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T size = 0;
                checked(diagnostics->GetMessage(i, nullptr, &size));
                std::vector<uint8_t> bytes(size);
                auto message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                checked(diagnostics->GetMessage(i, message, &size));
                log << message->pDescription << '\n';
            }
            if (count)
                throw std::runtime_error("Reference D3D11 debug messages present");
        }
        report["completed"] = true;
        saveReport();
        return 0;
    } catch (const std::exception &error) {
        report["error"] = error.what();
        saveReport();
        return 1;
    }
}
