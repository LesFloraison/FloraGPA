// Independent D3D11 control-flow observations for explicit-gradient sampling.
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
        throw std::runtime_error("D3D11 group probe failure: " + std::to_string(hr));
}
void save(const std::filesystem::path &path, ID3DBlob *blob) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize());
    if (!file)
        throw std::runtime_error("Cannot save group probe shader");
}
void validate(ID3D11InfoQueue *queue) {
    for (UINT64 index = 0; index < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
        SIZE_T size = 0;
        check(queue->GetMessage(index, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(index, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
std::string source(const std::string &flow, std::array<uint32_t, 3> size, bool offset) {
    const auto count = size[0] * size[1] * size[2];
    const auto limit = std::to_string(count);
    const std::string sample = "asuint(t.SampleGrad(s,p[q*3].xy,p[q*3+1].xy,p[q*3+2].xy" +
                               std::string(offset ? ",int2(-1,1)" : "") + "))";
    std::string body;
    if (flow == "uniform")
        body = "uint q=i;dst[i*4]=" + sample + ";";
    else if (flow == "early-return")
        body = "if(i%3==1)return;uint q=i;dst[i*4]=" + sample + ";";
    else if (flow == "branches")
        body = "[branch]if(i&1){uint q=i;dst[i*4]=" + sample +
               ";}"
               "else{uint q=(i+1)%" +
               limit + ";dst[i*4]=" + sample + ";}";
    else if (flow == "uneven-loop")
        body = "[loop]for(uint j=0;j<1+i%3;++j){uint q=(i+j)%" + limit + ";dst[i*4+j]=" + sample + ";}";
    else if (flow == "barrier-loop")
        body = "[loop]for(uint j=0;j<3;++j){g[i]=(i+j)%" + limit +
               ";GroupMemoryBarrierWithGroupSync();uint q=g[(i+1)%" + limit + "];dst[i*4+j]=" + sample +
               ";GroupMemoryBarrierWithGroupSync();}";
    else if (flow == "staggered")
        body = "uint q=i;[branch]if(i&1){[loop]for(uint k=0;k<3;++k)q=(q+1)%" + limit +
               ";}dst[i*4]=" + sample + ";";
    else
        throw std::runtime_error("Unknown sampling flow");
    return "Texture2D<float4> t:register(t0);StructuredBuffer<float4> p:register(t1);"
           "SamplerState s:register(s3);RWStructuredBuffer<uint4> dst:register(u0);"
           "groupshared uint g[1024];[numthreads(" +
           std::to_string(size[0]) + "," + std::to_string(size[1]) + "," + std::to_string(size[2]) +
           ")]void main(uint i:SV_GroupIndex){" + body + "}";
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
                    {"scope", "independent_sample_group"},
                    {"vendor", vendor},
                    {"adapter_vendor", actual.VendorId},
                    {"adapter_device", actual.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                    {"cases", Json::array()}};
        std::array<std::vector<std::array<float, 4>>, 4> texels;
        std::array<D3D11_SUBRESOURCE_DATA, 4> initial;
        std::vector<uint32_t> textureBits;
        for (UINT mip = 0; mip < 4; ++mip) {
            const auto width = 8u >> mip;
            for (UINT y = 0; y < width; ++y)
                for (UINT x = 0; x < width; ++x) {
                    const std::array<float, 4> texel{float(mip) + .25f, float(x) / 8, float(y) / 8, 1};
                    texels[mip].push_back(texel);
                    for (float v : texel)
                        textureBits.push_back(std::bit_cast<uint32_t>(v));
                }
            initial[mip] = {texels[mip].data(), width * 16, 0};
        }
        report["texture_bits"] = textureBits;
        const D3D11_TEXTURE2D_DESC td{8,
                                      8,
                                      4,
                                      1,
                                      DXGI_FORMAT_R32G32B32A32_FLOAT,
                                      {1, 0},
                                      D3D11_USAGE_IMMUTABLE,
                                      D3D11_BIND_SHADER_RESOURCE,
                                      0,
                                      0};
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> textureView;
        check(device->CreateTexture2D(&td, initial.data(), &texture));
        check(device->CreateShaderResourceView(texture.Get(), nullptr, &textureView));
        const std::array<std::array<float, 4>, 9> gradients{{{0, 0, 0, 0},
                                                             {.03125f, 0, 0, .03125f},
                                                             {.125f, 0, 0, .125f},
                                                             {.25f, 0, 0, .25f},
                                                             {1, 0, 0, 1},
                                                             {-.25f, 0, 0, -.25f},
                                                             {1, 0, 0, .03125f},
                                                             {0, 1, -.03125f, 0},
                                                             {.25f, .125f, -.125f, .25f}}};
        const std::array<float, 7> coordinates{-1.25f, -.125f, 0, .25f, .5f, 1, 1.25f};
        for (const std::array<uint32_t, 3> size :
             {std::array<uint32_t, 3>{64, 1, 1}, {8, 4, 2}, {4, 4, 4}, {32, 2, 2}}) {
            const auto count = size[0] * size[1] * size[2];
            std::vector<std::array<float, 4>> parameters;
            for (UINT index = 0; index < count; ++index) {
                const auto &g = gradients[index % gradients.size()];
                parameters.push_back({coordinates[(index / 9) % coordinates.size()], .375f, 0, 0});
                parameters.push_back({g[0], g[1], 0, 0});
                parameters.push_back({g[2], g[3], 0, 0});
            }
            D3D11_BUFFER_DESC bd{UINT(parameters.size() * 16),
                                 D3D11_USAGE_IMMUTABLE,
                                 D3D11_BIND_SHADER_RESOURCE,
                                 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                 16};
            D3D11_SUBRESOURCE_DATA data{parameters.data(), 0, 0};
            ComPtr<ID3D11Buffer> input, output, staging;
            check(device->CreateBuffer(&bd, &data, &input));
            ComPtr<ID3D11ShaderResourceView> inputView;
            check(device->CreateShaderResourceView(input.Get(), nullptr, &inputView));
            bd.ByteWidth = count * 4 * 16;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            check(device->CreateBuffer(&bd, nullptr, &output));
            ComPtr<ID3D11UnorderedAccessView> outputView;
            check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputView));
            bd = {bd.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            check(device->CreateBuffer(&bd, nullptr, &staging));
            for (const std::string flow :
                 {"uniform", "early-return", "branches", "uneven-loop", "barrier-loop", "staggered"})
                for (UINT offset = 0; offset < 2; ++offset) {
                    const auto name = flow + "-" + std::to_string(size[0]) + "-" + std::to_string(size[1]) +
                                      "-" + std::to_string(size[2]) + "-" + std::to_string(offset);
                    const auto hlsl = source(flow, size, offset);
                    ComPtr<ID3DBlob> binary, errors;
                    const auto hr =
                        D3DCompile(hlsl.data(), hlsl.size(), name.c_str(), nullptr, nullptr, "main", "cs_5_0",
                                   D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
                    if (FAILED(hr) && errors)
                        throw std::runtime_error(std::string(
                            static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
                    check(hr);
                    save(directory / (name + ".dxbc"), binary.Get());
                    ComPtr<ID3DBlob> assembly;
                    check(D3DDisassemble(binary->GetBufferPointer(), binary->GetBufferSize(), 0, nullptr,
                                         &assembly));
                    save(directory / (name + ".asm.txt"), assembly.Get());
                    ComPtr<ID3D11ComputeShader> shader;
                    check(device->CreateComputeShader(binary->GetBufferPointer(), binary->GetBufferSize(),
                                                      nullptr, &shader));
                    for (UINT filter : {0u, 85u}) {
                        context->ClearState();
                        D3D11_SAMPLER_DESC sd{};
                        sd.Filter = D3D11_FILTER(filter);
                        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                        sd.MaxAnisotropy = 16;
                        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
                        sd.MaxLOD = D3D11_FLOAT32_MAX;
                        ComPtr<ID3D11SamplerState> sampler;
                        check(device->CreateSamplerState(&sd, &sampler));
                        sampler->GetDesc(&sd);
                        std::array<uint32_t, 13> samplerBits;
                        std::memcpy(samplerBits.data(), &sd, sizeof sd);
                        auto *target = outputView.Get();
                        auto *state = sampler.Get();
                        const UINT sentinel[]{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
                        context->ClearUnorderedAccessViewUint(target, sentinel);
                        ID3D11ShaderResourceView *views[]{textureView.Get(), inputView.Get()};
                        context->CSSetShader(shader.Get(), nullptr, 0);
                        context->CSSetShaderResources(0, 2, views);
                        context->CSSetSamplers(3, 1, &state);
                        context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                        context->Dispatch(1, 1, 1);
                        context->ClearState();
                        context->CopyResource(staging.Get(), output.Get());
                        context->Flush();
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        for (;;) {
                            const auto status = context->Map(staging.Get(), 0, D3D11_MAP_READ,
                                                             D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                            if (status == S_OK)
                                break;
                            if (status != DXGI_ERROR_WAS_STILL_DRAWING)
                                check(status);
                            if (std::chrono::steady_clock::now() >= deadline)
                                throw std::runtime_error("Group observation readback timed out");
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        std::vector<std::array<uint32_t, 4>> values(count * 4);
                        std::memcpy(values.data(), mapped.pData, values.size() * 16);
                        context->Unmap(staging.Get(), 0);
                        validate(queue.Get());
                        report["cases"].push_back({{"flow", flow},
                                                   {"group_size", size},
                                                   {"offset", offset},
                                                   {"filter", filter},
                                                   {"shader", name + ".dxbc"},
                                                   {"parameters", parameters},
                                                   {"sampler_descriptor", samplerBits},
                                                   {"output", values}});
                    }
                }
        }
        report["validation_errors"] = 0;
        std::ofstream file(directory / "results.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot save group observations");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
