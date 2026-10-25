// Independent pixel-quad observations using only public D3D11 interfaces.
#define NOMINMAX
#include <array>
#include <bit>
#include <chrono>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Derivative probe D3D11 failure: " + std::to_string(hr));
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "Independent pixel derivatives", nullptr,
                               nullptr, "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(hr);
    return code;
}
void save(const std::filesystem::path &path, ID3DBlob *blob) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize());
    if (!file)
        throw std::runtime_error("Cannot save derivative probe shader");
}
void validate(ID3D11InfoQueue *queue) {
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) {
        std::cerr << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP\n";
        return 2;
    }
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        const uint32_t vendor = std::stoul(argv[2]);
        ComPtr<IDXGIAdapter1> selected;
        if (vendor) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(index, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr);
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc));
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested group probe adapter is unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        check(device.As(&dxgi));
        check(dxgi->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC actual{};
        check(adapter->GetDesc(&actual));
        ComPtr<ID3D11InfoQueue> queue;
        check(device.As(&queue));
        Json report{{"schema", 1},
                    {"scope", "independent_pixel_derivatives"},
                    {"vendor", vendor},
                    {"adapter_vendor", actual.VendorId},
                    {"adapter_device", actual.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                    {"cases", Json::array()}};

        const auto vsCode = compile(
            "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==1?3:-1,i==2?3:-1,0,1);}", "vs_5_0");
        ComPtr<ID3D11VertexShader> vs;
        check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = true;
        rd.ScissorEnable = true;
        ComPtr<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rd, &raster));
        D3D11_TEXTURE2D_DESC td{
            2, 2, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET,
            0, 0};
        ComPtr<ID3D11Texture2D> output, staging;
        check(device->CreateTexture2D(&td, nullptr, &output));
        ComPtr<ID3D11RenderTargetView> target;
        check(device->CreateRenderTargetView(output.Get(), nullptr, &target));
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&td, nullptr, &staging));
        const D3D11_BUFFER_DESC bd{80, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&bd, nullptr, &constants));
        const std::array<std::array<float, 4>, 4> finite{
            {{1, -1, .25f, 0}, {3, -4, .5f, 2}, {8, -9, 1, 8}, {20, -16, 2, 20}}};
        std::array<std::array<uint32_t, 4>, 4> ordinary;
        for (unsigned i = 0; i < 4; ++i)
            ordinary[i] = std::bit_cast<std::array<uint32_t, 4>>(finite[i]);
        const std::array<std::array<uint32_t, 4>, 4> special{
            {{0x80000000, 1, 0x7f800000, 0x7fc12345},
             {0, 0x007fffff, 0x7f800000, 0x3f800000},
             {0x80000000, 0x00800000, 0xff800000, 0x40000000},
             {0, 0x80800000, 0xff800000, 0x40400000}}};
        const std::array<std::array<uint32_t, 4>, 4> sentinel{
            {{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
             {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
             {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
             {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef}}};
        for (const std::string operation : {"ddx", "ddy", "ddx_coarse", "ddx_fine", "ddy_coarse", "ddy_fine"})
            for (const std::string flow : {"uniform", "discard", "reconverge", "loop", "divergent"}) {
                const std::string expression = operation + "(v)";
                std::string source = "cbuffer Inputs:register(b0){uint4 bits[4];uint discarded;}"
                                     "uint4 main(float4 position:SV_Position):SV_Target{uint "
                                     "i=uint(position.x)+2*uint(position.y);float4 v=asfloat(bits[i]);";
                if (flow == "discard")
                    source += "if(discarded&(1u<<i))discard;v+=float4(i,2*i,3*i,4*i);";
                if (flow == "reconverge")
                    source += "[branch]if(i&1)v*=2;else v-=3;";
                if (flow == "loop")
                    source += "[loop]for(uint k=0;k<i;++k)v+=float4(1,2,3,4);";
                source += flow == "divergent" ? "float4 d;[branch]if(i&1)d=" + expression +
                                                    ";else d=" + operation + "(v*2);return asuint(d);}"
                                              : "return asuint(" + expression + ");}";
                const std::string profile = operation == "ddx" || operation == "ddy" ? "ps_4_0" : "ps_5_0";
                ComPtr<ID3DBlob> code;
                try {
                    code = compile(source, profile.c_str());
                } catch (const std::runtime_error &error) {
                    if (flow != "divergent" || std::string(error.what()).find("X4014") == std::string::npos)
                        throw;
                    report["compiler_rejections"].push_back({{"operation", operation},
                                                             {"flow", flow},
                                                             {"profile", profile},
                                                             {"diagnostic", error.what()}});
                    continue;
                }
                const auto name = operation + "-" + flow;
                save(directory / (name + ".dxbc"), code.Get());
                ComPtr<ID3DBlob> assembly;
                check(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &assembly));
                save(directory / (name + ".asm.txt"), assembly.Get());
                ComPtr<ID3D11PixelShader> ps;
                check(
                    device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
                for (unsigned dataset = 0; dataset < 2; ++dataset)
                    for (unsigned coverage : {15u, 3u, 5u, 1u})
                        for (unsigned discarded : (flow == "discard" ? std::vector<unsigned>{1, 2, 5, 15}
                                                                     : std::vector<unsigned>{0})) {
                            const auto &inputs = dataset ? special : ordinary;
                            std::array<std::array<uint32_t, 4>, 5> values{};
                            std::copy(inputs.begin(), inputs.end(), values.begin());
                            values[4][0] = discarded;
                            context->ClearState();
                            context->UpdateSubresource(output.Get(), 0, nullptr, sentinel.data(), 32, 0);
                            context->UpdateSubresource(constants.Get(), 0, nullptr, values.data(), 0, 0);
                            auto *cb = constants.Get();
                            auto *rtv = target.Get();
                            const D3D11_VIEWPORT viewport{0, 0, 2, 2, 0, 1};
                            const D3D11_RECT rect{0, 0, coverage == 5 || coverage == 1 ? 1 : 2,
                                                  coverage == 3 || coverage == 1 ? 1 : 2};
                            context->VSSetShader(vs.Get(), nullptr, 0);
                            context->PSSetShader(ps.Get(), nullptr, 0);
                            context->PSSetConstantBuffers(0, 1, &cb);
                            context->RSSetState(raster.Get());
                            context->RSSetViewports(1, &viewport);
                            context->RSSetScissorRects(1, &rect);
                            context->OMSetRenderTargets(1, &rtv, nullptr);
                            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                            context->Draw(3, 0);
                            context->ClearState();
                            context->CopyResource(staging.Get(), output.Get());
                            context->Flush();
                            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            for (;;) {
                                const auto status = context->Map(staging.Get(), 0, D3D11_MAP_READ,
                                                                 D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                                if (status == S_OK)
                                    break;
                                if (status != DXGI_ERROR_WAS_STILL_DRAWING)
                                    check(status);
                                if (std::chrono::steady_clock::now() >= deadline)
                                    throw std::runtime_error("Derivative readback timed out");
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                            std::array<std::array<uint32_t, 4>, 4> results;
                            for (unsigned y = 0; y < 2; ++y)
                                std::memcpy(results.data() + 2 * y,
                                            static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch,
                                            32);
                            context->Unmap(staging.Get(), 0);
                            validate(queue.Get());
                            report["cases"].push_back({{"operation", operation},
                                                       {"flow", flow},
                                                       {"profile", profile},
                                                       {"shader", name + ".dxbc"},
                                                       {"dataset", dataset},
                                                       {"coverage", coverage},
                                                       {"discarded", discarded},
                                                       {"inputs", inputs},
                                                       {"output", results},
                                                       {"derivative_defined", flow != "divergent"}});
                        }
            }
        report["validation_errors"] = 0;
        std::ofstream file(directory / "results.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot save derivative observations");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
