// Independent sampling observations; no replay or private vendor modules.
#define NOMINMAX
#include <array>
#include <chrono>
#include <d3d11_1.h>
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
        throw std::runtime_error("D3D11 operation failed: " + std::to_string(hr));
}
void save(const std::filesystem::path &path, ID3DBlob *blob) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize());
    if (!file)
        throw std::runtime_error("Cannot save probe shader");
}
void validate(ID3D11InfoQueue *queue) {
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        check(queue->GetMessage(i, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
struct Configuration {
    const char *name;
    uint32_t filter = 0, address = 3;
    float bias = 0, minimum = -D3D11_FLOAT32_MAX, maximum = D3D11_FLOAT32_MAX, resourceMinimum = 0;
    uint32_t firstMip = 0, mipCount = 4;
    bool unboundSampler = false, unboundResource = false;
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && ((argc != 4 && argc != 5) || std::wstring(argv[3]) != L"--grad")) {
        std::cerr << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP "
                     "[--grad [cs|ps|vs|cs-single|ps-single|vs-single]]\n";
        return 2;
    }
    try {
        const bool gradient = argc >= 4;
        std::wstring stage = argc == 5 ? argv[4] : L"cs";
        const bool single = stage.ends_with(L"-single");
        if (single)
            stage.resize(stage.size() - 7);
        if (stage != L"cs" && stage != L"ps" && stage != L"vs")
            throw std::runtime_error("Invalid sampling observation stage");
        const bool pixel = stage == L"ps", vertex = stage == L"vs";
        const std::string prefix = gradient ? "sample-grad-" : "sample-level-";
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        const auto vendor = std::stoul(argv[2]);
        ComPtr<IDXGIAdapter1> adapter;
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
                    adapter = candidate;
                    break;
                }
            }
            if (!adapter)
                throw std::runtime_error("Requested adapter is unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature{};
        check(D3D11CreateDevice(adapter.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                &feature, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actualAdapter;
        check(device.As(&dxgi));
        check(dxgi->GetAdapter(&actualAdapter));
        DXGI_ADAPTER_DESC actual{};
        check(actualAdapter->GetDesc(&actual));
        D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
        const bool minMax =
            SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof options)) &&
            options.MinMaxFiltering;
        ComPtr<ID3D11InfoQueue> queue;
        check(device.As(&queue));
        Json report{{"schema", 1},
                    {"scope", gradient ? "independent_sample_grad" : "independent_sample_level"},
                    {"vendor", vendor},
                    {"adapter_vendor", actual.VendorId},
                    {"adapter_device", actual.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                    {"feature_level", feature},
                    {"min_max_filtering", minMax},
                    {"cases", Json::array()}};
        std::array<ComPtr<ID3D11ComputeShader>, 2> shaders;
        std::array<ComPtr<ID3D11VertexShader>, 2> vertexShaders;
        std::array<ComPtr<ID3D11PixelShader>, 2> pixelShaders;
        std::array<ComPtr<ID3D11GeometryShader>, 2> streams;
        if (gradient)
            report["stage"] = pixel ? "ps" : vertex ? "vs" : "cs";
        if (single)
            report["invocation_mode"] = "single";
        for (unsigned offset = 0; offset < 2; ++offset) {
            std::string source =
                "Texture2D<float4> t:register(t0);StructuredBuffer<float4> p:register(t1);"
                "SamplerState s:register(s3);RWStructuredBuffer<uint4> dst:register(u0);"
                "[numthreads(64,1,1)]void main(uint3 id:SV_DispatchThreadID){if(id.x<63)" +
                std::string(gradient
                                ? "dst[id.x]=asuint(t.SampleGrad(s,p[id.x*3].xy,p[id.x*3+1].xy,p[id.x*3+2].xy"
                                : "dst[id.x]=asuint(t.SampleLevel(s,p[id.x].xy,p[id.x].z") +
                std::string(offset ? ",int2(-1,1)" : "") + "));}";
            if (pixel || vertex)
                source =
                    "Texture2D<float4> t:register(t0);StructuredBuffer<float4> p:register(t1);"
                    "SamplerState s:register(s3);" +
                    std::string(pixel ? "uint4 main(float4 pos:SV_Position):SV_Target{uint i=uint(pos.x);"
                                      : "uint4 main(uint i:SV_VertexID):TEXCOORD0{") +
                    "return asuint(t.SampleGrad(s,p[i*3].xy,p[i*3+1].xy,p[i*3+2].xy" +
                    std::string(offset ? ",int2(-1,1)" : "") + "));}";
            if (single) {
                source =
                    "Texture2D<float4> t:register(t0);StructuredBuffer<float4> p:register(t1);"
                    "SamplerState s:register(s3);cbuffer Index:register(b0){uint i;}" +
                    std::string(
                        pixel ? "uint4 main():SV_Target{"
                        : vertex
                            ? "uint4 main():TEXCOORD0{"
                            : "RWStructuredBuffer<uint4> dst:register(u0);[numthreads(1,1,1)]void main(){") +
                    std::string(pixel || vertex ? "return " : "dst[i]=") +
                    "asuint(t.SampleGrad(s,p[i*3].xy,p[i*3+1].xy,p[i*3+2].xy" +
                    std::string(offset ? ",int2(-1,1)" : "") + "));}";
            }
            ComPtr<ID3DBlob> binary, errors;
            const auto hr =
                D3DCompile(source.data(), source.size(), "Independent sample level", nullptr, nullptr, "main",
                           pixel    ? "ps_5_0"
                           : vertex ? "vs_5_0"
                                    : "cs_5_0",
                           D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
            if (FAILED(hr) && errors)
                throw std::runtime_error(std::string(static_cast<const char *>(errors->GetBufferPointer()),
                                                     errors->GetBufferSize()));
            check(hr);
            save(directory / (prefix + std::to_string(offset) + ".dxbc"), binary.Get());
            ComPtr<ID3DBlob> text;
            check(D3DDisassemble(binary->GetBufferPointer(), binary->GetBufferSize(), 0, nullptr, &text));
            save(directory / (prefix + std::to_string(offset) + ".asm.txt"), text.Get());
            if (pixel)
                check(device->CreatePixelShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr,
                                                &pixelShaders[offset]));
            else if (vertex) {
                check(device->CreateVertexShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr,
                                                 &vertexShaders[offset]));
                const D3D11_SO_DECLARATION_ENTRY declaration{0, "TEXCOORD", 0, 0, 4, 0};
                const UINT stride = 16;
                check(device->CreateGeometryShaderWithStreamOutput(
                    binary->GetBufferPointer(), binary->GetBufferSize(), &declaration, 1, &stride, 1,
                    D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &streams[offset]));
            } else
                check(device->CreateComputeShader(binary->GetBufferPointer(), binary->GetBufferSize(),
                                                  nullptr, &shaders[offset]));
        }
        ComPtr<ID3D11VertexShader> fullscreen;
        ComPtr<ID3D11RasterizerState> rasterizer;
        if (pixel) {
            const std::string source = "float4 main(uint i:SV_VertexID):SV_Position{"
                                       "return float4(i==1?3:-1,i==2?3:-1,0,1);}";
            ComPtr<ID3DBlob> binary;
            check(D3DCompile(source.data(), source.size(), "Independent fullscreen triangle", nullptr,
                             nullptr, "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, nullptr));
            check(device->CreateVertexShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr,
                                             &fullscreen));
            D3D11_RASTERIZER_DESC desc{};
            desc.FillMode = D3D11_FILL_SOLID;
            desc.CullMode = D3D11_CULL_NONE;
            desc.DepthClipEnable = true;
            check(device->CreateRasterizerState(&desc, &rasterizer));
        }
        std::vector<std::array<float, 4>> parameters;
        const std::array<std::array<float, 4>, 9> gradients{{{0, 0, 0, 0},
                                                             {.03125f, 0, 0, .03125f},
                                                             {.125f, 0, 0, .125f},
                                                             {.25f, 0, 0, .25f},
                                                             {1, 0, 0, 1},
                                                             {-.25f, 0, 0, -.25f},
                                                             {1, 0, 0, .03125f},
                                                             {0, 1, -.03125f, 0},
                                                             {.25f, .125f, -.125f, .25f}}};
        for (float u : {-1.25f, -.125f, 0.f, .25f, .5f, 1.f, 1.25f})
            if (gradient)
                for (const auto &g : gradients) {
                    parameters.push_back({u, .375f, 0, 0});
                    parameters.push_back({g[0], g[1], 0, 0});
                    parameters.push_back({g[2], g[3], 0, 0});
                }
            else
                for (float lod : {-2.f, 0.f, .25f, .5f, 1.f, 1.5f, 2.f, 3.f, 5.f})
                    parameters.push_back({u, .375f, lod, 0});
        const size_t recordCount = parameters.size() / (gradient ? 3 : 1);
        D3D11_BUFFER_DESC bd{UINT(parameters.size() * 16),
                             D3D11_USAGE_IMMUTABLE,
                             D3D11_BIND_SHADER_RESOURCE,
                             0,
                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                             16};
        D3D11_SUBRESOURCE_DATA initial{parameters.data(), 0, 0};
        ComPtr<ID3D11Buffer> params, output, staging;
        check(device->CreateBuffer(&bd, &initial, &params));
        ComPtr<ID3D11ShaderResourceView> parameterView;
        check(device->CreateShaderResourceView(params.Get(), nullptr, &parameterView));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = UINT(recordCount * 16);
        bd.BindFlags = vertex ? D3D11_BIND_STREAM_OUTPUT : D3D11_BIND_UNORDERED_ACCESS;
        if (vertex) {
            bd.MiscFlags = 0;
            bd.StructureByteStride = 0;
        }
        check(device->CreateBuffer(&bd, nullptr, &output));
        ComPtr<ID3D11UnorderedAccessView> outputView;
        if (!vertex)
            check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputView));
        bd = {UINT(recordCount * 16), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        check(device->CreateBuffer(&bd, nullptr, &staging));
        ComPtr<ID3D11Texture2D> pixelOutput, pixelReadback;
        ComPtr<ID3D11RenderTargetView> pixelTarget;
        if (pixel) {
            D3D11_TEXTURE2D_DESC desc{
                UINT(recordCount),        1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET, 0, 0};
            check(device->CreateTexture2D(&desc, nullptr, &pixelOutput));
            check(device->CreateRenderTargetView(pixelOutput.Get(), nullptr, &pixelTarget));
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateTexture2D(&desc, nullptr, &pixelReadback));
        }
        ComPtr<ID3D11Buffer> invocationIndex;
        if (single) {
            const D3D11_BUFFER_DESC desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            check(device->CreateBuffer(&desc, nullptr, &invocationIndex));
        }
        std::array<std::vector<std::array<float, 4>>, 4> texels;
        std::array<D3D11_SUBRESOURCE_DATA, 4> subresources;
        for (unsigned mip = 0; mip < 4; ++mip) {
            const auto width = 8u >> mip;
            for (unsigned y = 0; y < width; ++y)
                for (unsigned x = 0; x < width; ++x)
                    texels[mip].push_back({float(mip) + .25f, float(x) / 8, float(y) / 8, 1.f});
            subresources[mip] = {texels[mip].data(), width * 16, 0};
        }
        std::vector<Configuration> configurations{
            {"point"},
            {"linear", 21},
            {"point-bias", 0, 3, 2},
            {"linear-bias", 21, 3, 2},
            {"anisotropic", 85},
            {"minimum", 0, 3, 0, 1.5f},
            {"maximum", 0, 3, 0, -D3D11_FLOAT32_MAX, .5f},
            {"resource-minimum", 0, 3, 0, -D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX, 1.25f},
            {"view-mips", 0, 3, 0, -D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX, 0, 1, 2},
            {"border", 0, 4},
            {"wrap", 0, 1},
            {"mirror", 0, 2},
            {"mirror-once", 0, 5},
            {"minimum-filter", 0x115},
            {"maximum-filter", 0x195},
            {"null-sampler", 0},
            {"null-resource", 0}};
        configurations[15].unboundSampler = true;
        configurations[16].unboundResource = true;
        for (const auto &config : configurations)
            for (unsigned offset = 0; offset < 2; ++offset) {
                Json item{{"name", config.name}, {"offset", offset}, {"records", Json::array()}};
                if ((config.filter & 256) && !minMax) {
                    item["status"] = "unsupported_min_max_filtering";
                    report["cases"].push_back(item);
                    continue;
                }
                context->ClearState();
                D3D11_TEXTURE2D_DESC td{8,
                                        8,
                                        4,
                                        1,
                                        DXGI_FORMAT_R32G32B32A32_FLOAT,
                                        {1, 0},
                                        D3D11_USAGE_IMMUTABLE,
                                        D3D11_BIND_SHADER_RESOURCE,
                                        0,
                                        config.resourceMinimum ? D3D11_RESOURCE_MISC_RESOURCE_CLAMP : 0u};
                ComPtr<ID3D11Texture2D> texture;
                check(device->CreateTexture2D(&td, subresources.data(), &texture));
                if (config.resourceMinimum)
                    context->SetResourceMinLOD(texture.Get(), config.resourceMinimum);
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = td.Format;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D = {config.firstMip, config.mipCount};
                ComPtr<ID3D11ShaderResourceView> view;
                check(device->CreateShaderResourceView(texture.Get(), &vd, &view));
                D3D11_SAMPLER_DESC sd{};
                sd.Filter = D3D11_FILTER(config.filter);
                sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MODE(config.address);
                sd.MipLODBias = config.bias;
                sd.MaxAnisotropy = 16;
                sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
                const float border[]{.125f, .25f, .5f, .75f};
                std::memcpy(sd.BorderColor, border, sizeof border);
                sd.MinLOD = config.minimum;
                sd.MaxLOD = config.maximum;
                ComPtr<ID3D11SamplerState> sampler;
                if (!config.unboundSampler)
                    check(device->CreateSamplerState(&sd, &sampler));
                std::array<uint32_t, 13> descriptor{};
                std::memcpy(descriptor.data(), &sd, sizeof sd);
                item["sampler_descriptor"] = config.unboundSampler ? Json(nullptr) : Json(descriptor);
                if (sampler) {
                    D3D11_SAMPLER_DESC observed{};
                    sampler->GetDesc(&observed);
                    std::memcpy(descriptor.data(), &observed, sizeof observed);
                    item["observed_sampler_descriptor"] = descriptor;
                } else
                    item["observed_sampler_descriptor"] = nullptr;
                item["resource_bound"] = !config.unboundResource;
                item["resource_min_lod"] = config.resourceMinimum;
                item["observed_resource_min_lod"] =
                    config.resourceMinimum ? Json(context->GetResourceMinLOD(texture.Get())) : Json(nullptr);
                item["first_mip"] = config.firstMip;
                item["mip_count"] = config.mipCount;
                std::array<ID3D11ShaderResourceView *, 2> views{config.unboundResource ? nullptr : view.Get(),
                                                                parameterView.Get()};
                auto *state = sampler.Get();
                auto *target = outputView.Get();
                const UINT sentinel[]{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
                const auto dispatchSingle = [&](auto draw) {
                    for (UINT index = 0; index < recordCount; ++index) {
                        const UINT values[]{index, 0, 0, 0};
                        context->UpdateSubresource(invocationIndex.Get(), 0, nullptr, values, 0, 0);
                        draw(index);
                    }
                };
                if (pixel) {
                    auto *rtv = pixelTarget.Get();
                    const float clear[]{-1, -1, -1, -1};
                    context->ClearRenderTargetView(rtv, clear);
                    context->OMSetRenderTargets(1, &rtv, nullptr);
                    context->VSSetShader(fullscreen.Get(), nullptr, 0);
                    context->PSSetShader(pixelShaders[offset].Get(), nullptr, 0);
                    context->PSSetShaderResources(0, 2, views.data());
                    context->PSSetSamplers(3, 1, &state);
                    const D3D11_VIEWPORT viewport{0, 0, float(recordCount), 1, 0, 1};
                    context->RSSetViewports(1, &viewport);
                    context->RSSetState(rasterizer.Get());
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    if (single) {
                        auto *cb = invocationIndex.Get();
                        context->PSSetConstantBuffers(0, 1, &cb);
                        dispatchSingle([&](UINT index) {
                            const D3D11_VIEWPORT one{float(index), 0, 1, 1, 0, 1};
                            context->RSSetViewports(1, &one);
                            context->Draw(3, 0);
                        });
                    } else
                        context->Draw(3, 0);
                } else if (vertex) {
                    auto *destination = output.Get();
                    const UINT start = 0;
                    context->VSSetShader(vertexShaders[offset].Get(), nullptr, 0);
                    context->VSSetShaderResources(0, 2, views.data());
                    context->VSSetSamplers(3, 1, &state);
                    context->GSSetShader(streams[offset].Get(), nullptr, 0);
                    context->SOSetTargets(1, &destination, &start);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
                    if (single) {
                        auto *cb = invocationIndex.Get();
                        context->VSSetConstantBuffers(0, 1, &cb);
                        dispatchSingle([&](UINT) { context->Draw(1, 0); });
                    } else
                        context->Draw(UINT(recordCount), 0);
                } else {
                    context->ClearUnorderedAccessViewUint(target, sentinel);
                    context->CSSetShader(shaders[offset].Get(), nullptr, 0);
                    context->CSSetShaderResources(0, 2, views.data());
                    context->CSSetSamplers(3, 1, &state);
                    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                    if (single) {
                        auto *cb = invocationIndex.Get();
                        context->CSSetConstantBuffers(0, 1, &cb);
                        dispatchSingle([&](UINT) { context->Dispatch(1, 1, 1); });
                    } else
                        context->Dispatch(1, 1, 1);
                }
                context->ClearState();
                auto *readback = pixel ? static_cast<ID3D11Resource *>(pixelReadback.Get()) : staging.Get();
                context->CopyResource(readback, pixel ? static_cast<ID3D11Resource *>(pixelOutput.Get())
                                                      : output.Get());
                context->Flush();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                for (;;) {
                    const auto hr =
                        context->Map(readback, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                    if (hr == S_OK)
                        break;
                    if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                        check(hr);
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("Sample-level observation timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::vector<std::array<uint32_t, 4>> values(recordCount);
                std::memcpy(values.data(), mapped.pData, values.size() * 16);
                context->Unmap(readback, 0);
                for (size_t index = 0; index < recordCount; ++index) {
                    Json record{{"parameters", parameters[index * (gradient ? 3 : 1)]},
                                {"bits", values[index]}};
                    if (gradient) {
                        record["ddx"] = parameters[index * 3 + 1];
                        record["ddy"] = parameters[index * 3 + 2];
                    }
                    item["records"].push_back(record);
                }
                validate(queue.Get());
                item["status"] = "observed";
                report["cases"].push_back(item);
            }
        report["validation_errors"] = 0;
        std::ofstream file(directory / "results.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot write sample-level observations");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
