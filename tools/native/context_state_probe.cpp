// Self-owned immediate-context state-swap evidence; development only.
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
        if (mode < 0 || mode > 7)
            throw std::runtime_error("Invalid context-state mode");
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load development shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("RegisterClass failed");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext1> context1;
        checked(device.As(&device1));
        checked(context.As(&context1));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, device.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        auto compile = [&](const std::string &source, const char *entry, const char *target) {
            ComPtr<ID3DBlob> code, errors;
            checked(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target, 0, 0,
                               &code, &errors));
            return code;
        };
        auto vsCode =
            compile("float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                    "vs", "vs_5_0");
        ComPtr<ID3D11VertexShader> vs;
        checked(
            device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        ComPtr<ID3D11PixelShader> ps[3];
        const char *colors[]{"1,0,0,1", "0,1,0,1", "0,0,1,1"};
        for (unsigned i = 0; i < 3; ++i) {
            auto code = compile(std::string("float4 ps():SV_Target{return float4(") + colors[i] + ");}", "ps",
                                "ps_5_0");
            checked(
                device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps[i]));
        }
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        checked(device->CreateRasterizerState(&rd, &raster));
        auto graphics = [&](unsigned color) {
            auto view = chain.rtv.Get();
            context->OMSetRenderTargets(1, &view, nullptr);
            const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(raster.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps[color].Get(), nullptr, 0);
        };
        unsigned observations = 0;
        auto observe = [&](int color) {
            ComPtr<ID3D11PixelShader> pixel;
            ComPtr<ID3D11VertexShader> vertex;
            ComPtr<ID3D11RenderTargetView> target;
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->PSGetShader(&pixel, nullptr, nullptr);
            context->VSGetShader(&vertex, nullptr, nullptr);
            context->OMGetRenderTargets(1, &target, nullptr);
            context->IAGetPrimitiveTopology(&topology);
            if (pixel.Get() != (color < 0 ? nullptr : ps[color].Get()) ||
                vertex.Get() != (color < 0 ? nullptr : vs.Get()) ||
                target.Get() != (color < 0 ? nullptr : chain.rtv.Get()) ||
                topology !=
                    (color < 0 ? D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST))
                throw std::runtime_error("Context-state binding oracle mismatch");
            observations += 4;
        };
        auto newState = [&] {
            ComPtr<ID3DDeviceContextState> state;
            const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
            D3D_FEATURE_LEVEL chosen{};
            checked(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                      &chosen, &state));
            if (!state || chosen != level)
                throw std::runtime_error("Context-state creation oracle mismatch");
            return state;
        };
        ComPtr<ID3DDeviceContextState> original, saved, blue;
        if (mode && mode != 3 && mode != 4 && mode != 6) {
            graphics(0);
            saved = newState();
            context1->SwapDeviceContextState(saved.Get(), &original);
            observe(-1);
            graphics(1);
            if (mode == 5) {
                blue = newState();
                context1->SwapDeviceContextState(blue.Get(), nullptr);
                observe(-1);
                graphics(2);
            }
            context1->SwapDeviceContextState(original.Get(), nullptr);
            observe(0);
        }
        Tex screen;
        screen.width = screen.height = 8;
        screen.mips = screen.layers = 1;
        auto staging = create(device.Get(), screen, true);
        auto pixels = storage(screen, false);
        auto expected = [&](unsigned color) {
            std::vector<uint8_t> raw(8 * 8 * 4);
            for (size_t p = 0; p < raw.size(); p += 4)
                raw[p + color] = raw[p + 3] = 255;
            return raw;
        };
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"frames\":[";
        bool selected = false;
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
            observations = 0;
            context->ClearState();
            const float black[]{0, 0, 0, 1};
            context->ClearRenderTargetView(chain.rtv.Get(), black);
            graphics(0);
            observe(0);
            unsigned draws = 0;
            auto draw = [&](unsigned color) {
                observe(int(color));
                context->Draw(3, 0);
                context->CopyResource(staging.Get(), chain.buffer.Get());
                auto rgba = read(context.Get(), staging.Get(), pixels);
                auto golden = expected(color);
                if (rgba != golden)
                    throw std::runtime_error("State-dependent draw image mismatch");
                save(output / ("step-" + std::to_string(draws) + ".rgba"), rgba);
                save(output / L"frame.rgba", rgba);
                save(output / L"expected.rgba", golden);
                ++draws;
            };
            if (!mode || mode == 6) {
                if (mode == 6)
                    context1->SwapDeviceContextState(nullptr, nullptr);
                draw(0);
            } else if (mode == 3 || mode == 4) {
                auto state = newState();
                ComPtr<ID3DDeviceContextState> previous;
                context1->SwapDeviceContextState(state.Get(), &previous);
                observe(-1);
                graphics(1);
                draw(1);
                context1->SwapDeviceContextState(previous.Get(), nullptr);
                observe(0);
                if (mode == 4)
                    draw(0);
            } else {
                ComPtr<ID3DDeviceContextState> previous;
                context1->SwapDeviceContextState(saved.Get(),
                                                 mode == 7 ? nullptr : previous.ReleaseAndGetAddressOf());
                if (mode == 2) {
                    context->ClearState();
                    observe(-1);
                    graphics(1);
                }
                draw(1);
                if (mode == 5) {
                    context1->SwapDeviceContextState(blue.Get(), nullptr);
                    draw(2);
                }
                if (mode != 7 && !previous)
                    throw std::runtime_error("Swap did not return the previous context state");
                context1->SwapDeviceContextState(mode == 7 ? original.Get() : previous.Get(), nullptr);
                observe(0);
                if (mode == 5)
                    draw(0);
            }
            const auto present = chain.swap->Present(0, 0);
            checked(present);
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"binding_checks\":" << observations
                   << ",\"draws_verified\":" << draws
                   << ",\"image_verified\":true,\"present_status\":" << uint32_t(present) << '}';
            report.flush();
            Sleep(50);
        }
        if (argc == 5) {
            for (unsigned i = 0; i < 100 && !fs::exists(argv[3]); ++i)
                Sleep(50);
            if (!fs::exists(argv[3]) || !fs::file_size(argv[3]))
                throw std::runtime_error("Original workload completed without a capture");
        }
        report << "],\"primary_selected_by_original_method\":" << (selected ? "true" : "false")
               << ",\"completed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
