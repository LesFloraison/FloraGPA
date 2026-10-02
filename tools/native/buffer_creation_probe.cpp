// Development-only producer using the same hidden-window/GPA request helpers.
#include <array>
#include <d3dcompiler.h>
#define wmain UnusedPresentCaptureMain
#include "present_capture_probe.cpp"
#undef wmain
int wmain(int argc, wchar_t **argv) {
    // output mode(0=initial,1=upload,2=validation,3=failed,4=initial-then-update) [capture shim]
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output)))
        return 2;
    fs::create_directories(output);
    try {
        int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 4)
            throw std::runtime_error("Invalid mode");
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load research shimloader");
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
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, device.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        const auto shaderText =
            std::string("struct V{float4 p:SV_Position;float4 c:COLOR;}; V vs(uint v:DATA,uint "
                        "i:SV_VertexID){V o;o.p=float4(i==2?3:-1,i==1?3:-1,0,1);o.c=(v==") +
            std::to_string(mode == 0 ? 0x11110000u : 0x22220000u) +
            "u+i)?float4(0,1,0,1):float4(1,0,1,1);return o;}float4 ps(V i):SV_Target{return i.c;}";
        ComPtr<ID3DBlob> vsCode, psCode, errors;
        checked(D3DCompile(shaderText.data(), shaderText.size(), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0,
                           0, &vsCode, &errors));
        errors.Reset();
        checked(D3DCompile(shaderText.data(), shaderText.size(), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0,
                           0, &psCode, &errors));
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        checked(
            device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        D3D11_INPUT_ELEMENT_DESC element{"DATA", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA,
                                         0};
        checked(device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                          &layout));
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rs;
        checked(device->CreateRasterizerState(&raster, &rs));
        D3D11_TEXTURE2D_DESC td{};
        chain.buffer->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = td.MiscFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        checked(device->CreateTexture2D(&td, nullptr, &readback));
        D3D11_BUFFER_DESC desc{64, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        auto readDesc = desc;
        readDesc.Usage = D3D11_USAGE_STAGING;
        readDesc.BindFlags = 0;
        readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        checked(device->CreateBuffer(&readDesc, nullptr, &staging));
        std::array<uint32_t, 16> initial{}, updated{};
        for (unsigned i = 0; i < 16; ++i) {
            initial[i] = 0x11110000 + i;
            updated[i] = 0x22220000 + i;
        }
        D3D11_SUBRESOURCE_DATA init{initial.data(), 0, 0};
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
            auto shim = GetModuleHandleW(L"shimd3d64.dll");
            if (argc == 5 && frame == 5 && shim) {
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(shim, "CaptureNextFrame"));
                if (!request)
                    throw std::runtime_error("CaptureNextFrame unavailable");
                request(argv[3], captured);
                requested = true;
            }
            context->ClearState();
            auto description = desc;
            if (mode == 3)
                description.ByteWidth = 0;
            ComPtr<ID3D11Buffer> buffer;
            ID3D11Buffer *created = nullptr;
            auto hr = device->CreateBuffer(&description, mode == 0 || mode == 4 ? &init : nullptr,
                                           mode == 2 ? nullptr : &created);
            buffer.Attach(created);
            if ((mode == 2 && hr != S_FALSE) || (mode == 3 && SUCCEEDED(hr)) ||
                (mode != 2 && mode != 3 && hr != S_OK))
                throw std::runtime_error("Unexpected CreateBuffer result");
            if (buffer) {
                const GUID name = {
                    0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};
                const char text[] = "Flora creation probe";
                checked(buffer->SetPrivateData(name, sizeof(text) - 1, text));
                if (mode == 1 || mode == 4)
                    context->UpdateSubresource(buffer.Get(), 0, nullptr, updated.data(), 0, 0);
                context->CopyResource(staging.Get(), buffer.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                auto expected = mode == 0 ? initial : updated;
                bool same = memcmp(mapped.pData, expected.data(), sizeof(expected)) == 0;
                std::ofstream bytes(output / L"buffer.bin", std::ios::binary);
                bytes.write(static_cast<const char *>(mapped.pData), sizeof(expected));
                context->Unmap(staging.Get(), 0);
                if (!same)
                    throw std::runtime_error("Buffer CPU oracle mismatch");
            }
            const float green[]{0, 1, 0, 1};
            context->ClearRenderTargetView(chain.rtv.Get(), green);
            if (buffer) {
                const float magenta[]{1, 0, 1, 1};
                context->ClearRenderTargetView(chain.rtv.Get(), magenta);
                auto view = chain.rtv.Get();
                context->OMSetRenderTargets(1, &view, nullptr);
                D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
                context->RSSetViewports(1, &viewport);
                context->RSSetState(rs.Get());
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->PSSetShader(ps.Get(), nullptr, 0);
                context->IASetInputLayout(layout.Get());
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                auto vb = buffer.Get();
                UINT stride = 4, offset = 0;
                context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                context->Draw(3, 0);
            }
            context->CopyResource(readback.Get(), chain.buffer.Get());
            D3D11_MAPPED_SUBRESOURCE pixels{};
            checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &pixels));
            std::array<uint8_t, 256> rgba{};
            for (unsigned y = 0; y < 8; ++y)
                memcpy(rgba.data() + y * 32, static_cast<uint8_t *>(pixels.pData) + y * pixels.RowPitch, 32);
            context->Unmap(readback.Get(), 0);
            for (unsigned i = 0; i < rgba.size(); ++i)
                if (rgba[i] != ((i % 4 == 1 || i % 4 == 3) ? 255 : 0))
                    throw std::runtime_error("GPU buffer-dependent image mismatch");
            std::ofstream(output / L"frame.rgba", std::ios::binary)
                .write(reinterpret_cast<const char *>(rgba.data()), rgba.size());
            auto terminal = chain.swap->Present(0, 0);
            checked(terminal);
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"hresult\":" << int32_t(hr)
                   << ",\"buffer_created\":" << (buffer ? "true" : "false") << '}';
            Sleep(25);
        }
        report << "],\"capture_requested\":" << (requested ? "true" : "false") << ",\"completed\":true}\n";
        return 0;
    } catch (const std::exception &e) {
        std::ofstream(output / L"error.txt") << e.what();
        return 1;
    }
}
