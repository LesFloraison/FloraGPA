// Development-only sparse Map capture experiment; no GPA dependency in FloraGPA runtime.
#define wmain SharedPresentHelpersOnly
#include "present_capture_probe.cpp"
#undef wmain
#include "original_capture_control.h"
#include <array>
#include <cstring>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>

namespace {
void save(const fs::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output) && !fs::is_empty(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode > 10)
            throw std::runtime_error("Invalid sparse Map mode");
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load research shimloader");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("RegisterClass failed");
        const bool warp = GetEnvironmentVariableW(L"FLORA_SPARSE_MAP_WARP", nullptr, 0) != 0;
        const bool buffer = mode >= 4, dynamic = mode >= 5, volume = mode == 2, drawSnapshot = mode >= 6;
        const unsigned width = buffer ? 16384 : mode == 1 ? 1025 : 13;
        const unsigned height = buffer || mode == 3 ? 1 : mode == 1 ? 19 : 7;
        const unsigned depth = volume ? 5 : 1;
        std::vector<uint8_t> initial(size_t(width) * height * depth * 4);
        for (size_t i = 0; i < initial.size(); ++i)
            initial[i] = uint8_t((i * 13 + 17) % 251);
        if (drawSnapshot)
            for (unsigned vertex = 1; vertex < 3; ++vertex)
                std::memcpy(initial.data() + vertex * 4, initial.data(), 4);
        auto expected = initial;
        const std::array<size_t, 4> pixels{0, width - 1, size_t(width) * (height / 2),
                                           size_t(width) * height * depth - 1};
        for (unsigned n = 0; n < pixels.size(); ++n)
            for (unsigned channel = 0; channel < 4; ++channel)
                expected[pixels[n] * 4 + channel] = uint8_t(203 + n * 7 + channel);
        save(output / L"before.bin", initial);
        save(output / L"after.bin", expected);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const UINT deviceFlags =
            GetEnvironmentVariableW(L"FLORA_SPARSE_MAP_DEBUG", nullptr, 0) ? D3D11_CREATE_DEVICE_DEBUG : 0;
        checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  deviceFlags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_ADAPTER_DESC adapterDesc{};
        checked(adapter->GetDesc(&adapterDesc));
        Chain chain;
        createChain(chain, device.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        auto create = [&](bool readback) {
            ComPtr<ID3D11Resource> resource;
            const auto usage = dynamic && !readback ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_STAGING;
            const unsigned bind = dynamic && !readback ? D3D11_BIND_VERTEX_BUFFER : 0;
            const unsigned cpu = readback  ? D3D11_CPU_ACCESS_READ
                                 : dynamic ? D3D11_CPU_ACCESS_WRITE
                                           : D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            D3D11_SUBRESOURCE_DATA data{initial.data(), width * 4, width * height * 4};
            if (buffer) {
                D3D11_BUFFER_DESC desc{UINT(initial.size()), usage, bind, cpu, 0, 0};
                ComPtr<ID3D11Buffer> value;
                checked(device->CreateBuffer(&desc, &data, &value));
                checked(value.As(&resource));
            } else if (volume) {
                D3D11_TEXTURE3D_DESC desc{width, height, depth, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          usage, bind,   cpu,   0};
                ComPtr<ID3D11Texture3D> value;
                checked(device->CreateTexture3D(&desc, &data, &value));
                checked(value.As(&resource));
            } else {
                D3D11_TEXTURE2D_DESC desc{width,  height, 1,    1,   DXGI_FORMAT_R8G8B8A8_UNORM,
                                          {1, 0}, usage,  bind, cpu, 0};
                ComPtr<ID3D11Texture2D> value;
                checked(device->CreateTexture2D(&desc, &data, &value));
                checked(value.As(&resource));
            }
            return resource;
        };
        const auto resource = create(false), readback = create(true);
        const auto afterReadback = drawSnapshot ? create(true) : readback;
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11Texture2D> imageReadback;
        if (drawSnapshot) {
            auto compile = [](const char *text, const char *profile) {
                ComPtr<ID3DBlob> code, errors;
                checked(D3DCompile(text, std::strlen(text), "SparseMapSnapshot", nullptr, nullptr, "main",
                                   profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors));
                return code;
            };
            const auto vertex =
                compile("struct V{float4 p:SV_Position;float4 c:COLOR;};"
                        "V main(float4 c:COLOR,uint i:SV_VertexID){V v;v.p=float4(i==2?3:-1,i==1?3:-1,0,1);"
                        "v.c=c;return v;}",
                        "vs_5_0");
            const auto pixel =
                compile("float4 main(float4 p:SV_Position,float4 c:COLOR):SV_Target{return c;}", "ps_5_0");
            checked(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
                                               &vs));
            checked(
                device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps));
            const D3D11_INPUT_ELEMENT_DESC element{
                "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            checked(device->CreateInputLayout(&element, 1, vertex->GetBufferPointer(),
                                              vertex->GetBufferSize(), &layout));
            const D3D11_BUFFER_DESC desc{
                UINT(initial.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
            checked(device->CreateBuffer(&desc, nullptr, &vertexBuffer));
            D3D11_RASTERIZER_DESC rs{};
            rs.FillMode = D3D11_FILL_SOLID;
            rs.CullMode = D3D11_CULL_NONE;
            rs.DepthClipEnable = TRUE;
            checked(device->CreateRasterizerState(&rs, &raster));
            D3D11_TEXTURE2D_DESC image{};
            chain.buffer->GetDesc(&image);
            image.Usage = D3D11_USAGE_STAGING;
            image.BindFlags = image.MiscFlags = 0;
            image.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            checked(device->CreateTexture2D(&image, nullptr, &imageReadback));
        }
        ComPtr<ID3D11Query> completion;
        if (mode >= 8) {
            const D3D11_QUERY_DESC desc{mode == 10 ? D3D11_QUERY_OCCLUSION_PREDICATE : D3D11_QUERY_EVENT, 0};
            if (mode == 10) {
                ComPtr<ID3D11Predicate> predicate;
                checked(device->CreatePredicate(&desc, &predicate));
                checked(predicate.As(&completion));
            } else
                checked(device->CreateQuery(&desc, &completion));
        }
        auto address = [&](const D3D11_MAPPED_SUBRESOURCE &mapped, size_t pixel) {
            const size_t z = pixel / (size_t(width) * height), y = (pixel / width) % height,
                         x = pixel % width;
            return static_cast<uint8_t *>(mapped.pData) + z * mapped.DepthPitch + y * mapped.RowPitch + x * 4;
        };
        auto observe = [&](ID3D11Resource *target, bool copy = true) {
            if (copy)
                context->CopyResource(target, resource.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (mode == 7) {
                context->Flush();
                HRESULT result = DXGI_ERROR_WAS_STILL_DRAWING;
                for (unsigned attempt = 0; attempt < 10000 && result == DXGI_ERROR_WAS_STILL_DRAWING;
                     ++attempt) {
                    result = context->Map(target, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                    if (result == DXGI_ERROR_WAS_STILL_DRAWING)
                        Sleep(1);
                }
                checked(result);
            } else
                checked(context->Map(target, 0, D3D11_MAP_READ, 0, &mapped));
            std::vector<uint8_t> actual(initial.size());
            for (unsigned z = 0; z < depth; ++z)
                for (unsigned y = 0; y < height; ++y)
                    std::memcpy(actual.data() + (size_t(z) * height + y) * width * 4,
                                address(mapped, (size_t(z) * height + y) * width), width * 4);
            context->Unmap(target, 0);
            return actual;
        };
        unsigned failures = 0;
        bool requested = false;
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"warp\":" << (warp ? "true" : "false") << ",\"width\":" << width
               << ",\"height\":" << height << ",\"depth\":" << depth
               << ",\"vendor_id\":" << adapterDesc.VendorId << ",\"device_id\":" << adapterDesc.DeviceId
               << ",\"adapter_luid_low\":" << adapterDesc.AdapterLuid.LowPart
               << ",\"adapter_luid_high\":" << adapterDesc.AdapterLuid.HighPart << ",\"frames\":[";
        for (unsigned frame = 0; frame < 12; ++frame) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (argc == 5 && frame == 5) {
                auto shim = GetModuleHandleW(L"shimd3d64.dll");
                // False means this swap chain is already primary; failures throw.
                flora::research::selectOriginalPrimarySwapChain(shim, chain.swap.Get());
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(shim, "CaptureNextFrame"));
                if (!request)
                    throw std::runtime_error("CaptureNextFrame is unavailable");
                request(argv[3], captured);
                requested = true;
            }
            context->ClearState();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(context->Map(resource.Get(), 0, dynamic ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE, 0,
                                 &mapped));
            for (unsigned z = 0; z < depth; ++z)
                for (unsigned y = 0; y < height; ++y)
                    std::memcpy(address(mapped, (size_t(z) * height + y) * width),
                                initial.data() + (size_t(z) * height + y) * width * 4, width * 4);
            context->Unmap(resource.Get(), 0);
            std::vector<uint8_t> before;
            unsigned polls = 0;
            if (mode >= 8) {
                if (mode == 10)
                    context->Begin(completion.Get());
                context->CopyResource(readback.Get(), resource.Get());
                context->End(completion.Get());
                if (mode == 9)
                    context->Flush();
                HRESULT status = S_FALSE;
                BOOL ready = FALSE;
                for (; polls < 10000 && status == S_FALSE; ++polls) {
                    status = context->GetData(completion.Get(), mode == 9 ? nullptr : &ready,
                                              mode == 9 ? 0 : sizeof ready,
                                              mode == 9 ? D3D11_ASYNC_GETDATA_DONOTFLUSH : 0);
                    if (status == S_FALSE)
                        Sleep(1);
                }
                checked(status);
                if (status != S_OK || (mode == 8 && !ready))
                    throw std::runtime_error("Query completion timeout or invalid event result");
            } else
                before = observe(readback.Get());
            // Blocking readback completes the copy before WRITE_NO_OVERWRITE
            // changes source bytes that the GPU previously consumed.
            checked(context->Map(resource.Get(), 0,
                                 dynamic ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_READ_WRITE, 0, &mapped));
            const unsigned row = mapped.RowPitch, slice = mapped.DepthPitch;
            for (const auto pixel : pixels)
                std::memcpy(address(mapped, pixel), expected.data() + pixel * 4, 4);
            context->Unmap(resource.Get(), 0);
            // Query modes observe the OLD copy only after the later CPU write.
            // No READ Map may provide the missing synchronization before it.
            if (mode >= 8)
                before = observe(readback.Get(), false);
            if (before != initial)
                ++failures;
            save(output / (L"before-" + std::to_wstring(frame) + L".bin"), before);
            const auto actual = observe(afterReadback.Get());
            if (actual != expected)
                ++failures;
            save(output / (L"actual-" + std::to_wstring(frame) + L".bin"), actual);
            // Modes 0..5 have marker images; 6..10 draw from the earlier copy.
            const float color[]{0, 1, 0, 1};
            context->ClearRenderTargetView(chain.rtv.Get(), color);
            if (drawSnapshot) {
                context->CopyResource(vertexBuffer.Get(), readback.Get());
                auto target = chain.rtv.Get();
                context->OMSetRenderTargets(1, &target, nullptr);
                auto vb = vertexBuffer.Get();
                const UINT stride = 4, offset = 0;
                context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                context->IASetInputLayout(layout.Get());
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->PSSetShader(ps.Get(), nullptr, 0);
                context->RSSetState(raster.Get());
                const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
                context->RSSetViewports(1, &viewport);
                context->Draw(3, 0);
                context->CopyResource(imageReadback.Get(), chain.buffer.Get());
                checked(context->Map(imageReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                std::vector<uint8_t> rgba(8 * 8 * 4);
                for (unsigned y = 0; y < 8; ++y)
                    std::memcpy(rgba.data() + y * 32,
                                static_cast<uint8_t *>(mapped.pData) + y * mapped.RowPitch, 32);
                context->Unmap(imageReadback.Get(), 0);
                for (size_t pixel = 0; pixel < 64; ++pixel)
                    if (std::memcmp(rgba.data() + pixel * 4, initial.data(), 4))
                        ++failures;
                save(output / (L"image-" + std::to_wstring(frame) + L".rgba"), rgba);
            }
            checked(chain.swap->Present(0, 0));
            report << (frame ? "," : "") << "{\"frame\":" << frame << ",\"row_pitch\":" << row
                   << ",\"depth_pitch\":" << slice << ",\"query_polls\":" << polls << "}";
            Sleep(20);
        }
        report << "],\"capture_requested\":" << (requested ? "true" : "false") << ",\"failures\":" << failures
               << ",\"completed\":true}\n";
        ComPtr<ID3D11InfoQueue> messages;
        if (SUCCEEDED(device.As(&messages))) {
            std::ofstream log(output / L"debug.txt");
            for (UINT64 n = 0; n < messages->GetNumStoredMessages(); ++n) {
                SIZE_T length = 0;
                messages->GetMessage(n, nullptr, &length);
                std::vector<uint8_t> storage(length);
                auto message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
                checked(messages->GetMessage(n, message, &length));
                log << message->Severity << ": " << message->pDescription << '\n';
            }
        }
        return failures ? 1 : 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
