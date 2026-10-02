// Development-only original captures: whole/byte region/NULL region/empty region/APPEND/COUNTER copies.
#include <array>
#include <d3dcompiler.h>
#define wmain UnusedPresentCaptureMain
#include "present_capture_probe.cpp"
#undef wmain
int wmain(int argc, wchar_t **argv) {
    // output mode(0..5) [capture shim]
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output)))
        return 2;
    fs::create_directories(output);
    try {
        int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 5)
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
        std::array<uint8_t, 64> sourceBytes{}, expected{}, zero{};
        for (unsigned i = 0; i < 64; ++i)
            sourceBytes[i] = uint8_t(i + 1);
        if (mode == 0 || mode == 2)
            expected = sourceBytes;
        if (mode == 1)
            memcpy(expected.data() + 3, sourceBytes.data() + 1, 11);
        const UINT counterOffset = mode == 4 ? 60 : 0, counterValue = 9;
        if (mode >= 4)
            memcpy(expected.data() + counterOffset, &counterValue, 4);
        std::array<uint32_t, 3> expectedVertices{};
        if (mode >= 4)
            expectedVertices.fill(counterValue);
        else
            memcpy(expectedVertices.data(), expected.data(), 12);
        const auto shaderText =
            std::string(
                "struct V{float4 p:SV_Position;float4 c:COLOR;}; V vs(uint v:DATA,uint i:SV_VertexID){"
                "V o;o.p=float4(i==2?3:-1,i==1?3:-1,0,1);uint e=i==0?") +
            std::to_string(expectedVertices[0]) + "u:i==1?" + std::to_string(expectedVertices[1]) +
            "u:" + std::to_string(expectedVertices[2]) +
            "u;o.c=v==e?float4(0,1,0,1):float4(1,0,1,1);return o;}"
            "float4 ps(V i):SV_Target{return i.c;}";
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
        ComPtr<ID3D11Buffer> source, buffer, counterBuffer;
        D3D11_SUBRESOURCE_DATA init{sourceBytes.data(), 0, 0};
        checked(device->CreateBuffer(&desc, &init, &source));
        init.pSysMem = zero.data();
        checked(device->CreateBuffer(&desc, &init, &buffer));
        D3D11_BUFFER_DESC counterDesc{
            64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            4};
        checked(device->CreateBuffer(&counterDesc, &init, &counterBuffer));
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 16;
        ud.Buffer.Flags = mode == 4 ? D3D11_BUFFER_UAV_FLAG_APPEND : D3D11_BUFFER_UAV_FLAG_COUNTER;
        ComPtr<ID3D11UnorderedAccessView> counter;
        checked(device->CreateUnorderedAccessView(counterBuffer.Get(), &ud, &counter));
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
            context->UpdateSubresource(buffer.Get(), 0, nullptr, zero.data(), 0, 0);
            if (mode == 0)
                context->CopyResource(buffer.Get(), source.Get());
            if (mode == 1) {
                D3D11_BOX box{1, 0, 0, 12, 1, 1};
                context->CopySubresourceRegion(buffer.Get(), 0, 3, 0, 0, source.Get(), 0, &box);
            }
            if (mode == 2)
                context->CopySubresourceRegion(buffer.Get(), 0, 0, 0, 0, source.Get(), 0, nullptr);
            if (mode == 3) {
                D3D11_BOX box{4, 0, 0, 4, 1, 1};
                context->CopySubresourceRegion(buffer.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
            }
            if (mode >= 4) {
                auto view = counter.Get();
                context->CSSetUnorderedAccessViews(0, 1, &view, &counterValue);
                context->CopyStructureCount(buffer.Get(), counterOffset, counter.Get());
            }
            context->CopyResource(staging.Get(), buffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            bool same = memcmp(mapped.pData, expected.data(), 64) == 0;
            std::ofstream(output / L"buffer.bin", std::ios::binary)
                .write(static_cast<const char *>(mapped.pData), 64);
            context->Unmap(staging.Get(), 0);
            if (!same)
                throw std::runtime_error("Copy byte oracle mismatch");
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
                UINT stride = mode >= 4 ? 0 : 4, offset = mode >= 4 ? counterOffset : 0;
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
