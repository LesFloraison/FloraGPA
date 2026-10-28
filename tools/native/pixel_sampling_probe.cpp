// Independent implicit pixel sampling observations using only public D3D11 interfaces.
#define NOMINMAX
#include "core/Dxbc.h"
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
        throw std::runtime_error("Pixel sampling probe D3D11 failure: " + std::to_string(hr));
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
        throw std::runtime_error("Cannot save pixel sampling probe shader");
}
Json validate(ID3D11InfoQueue *queue, bool missingComparisonSampler = false) {
    Json diagnostics = Json::array();
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        check(queue->GetMessage(i, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
            if (!missingComparisonSampler || message->ID != D3D11_MESSAGE_ID_DEVICE_DRAW_SAMPLER_MISMATCH)
                throw std::runtime_error(std::to_string(message->ID) + ": " + message->pDescription);
            diagnostics.push_back({{"id", message->ID}, {"message", message->pDescription}});
        }
    }
    queue->ClearStoredMessages();
    if (missingComparisonSampler && diagnostics.empty())
        throw std::runtime_error("Expected missing comparison sampler diagnostic was absent");
    return diagnostics;
}
struct Configuration {
    const char *name;
    uint32_t filter = 0, address = 3;
    float bias = 0, minimum = -D3D11_FLOAT32_MAX, maximum = D3D11_FLOAT32_MAX, resourceMinimum = 0;
    uint32_t firstMip = 0, mipCount = 4;
    bool unboundSampler = false, unboundResource = false;
    uint32_t comparison = 4;
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr
            << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP [PRECISION_SCENARIO_1_TO_6_OR_LOD]\n";
        return 2;
    }
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        const uint32_t vendor = std::stoul(argv[2]);
        const bool lod = argc == 4 && std::wstring(argv[3]) == L"lod";
        const uint32_t precision = argc == 4 && !lod ? std::stoul(argv[3]) : 0;
        if (precision > 6 || (argc == 4 && !lod && precision == 0))
            throw std::runtime_error("Invalid minimum precision scenario");
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
                throw std::runtime_error("Requested sampling probe adapter is unavailable");
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
                    {"scope", "independent_pixel_sampling"},
                    {"vendor", vendor},
                    {"precision_scenario", precision},
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

        std::array<std::vector<float>, 4> texels;
        std::array<D3D11_SUBRESOURCE_DATA, 4> subresources;
        for (unsigned mip = 0; mip < 4; ++mip) {
            const unsigned width = 8u >> mip;
            for (unsigned y = 0; y < width; ++y)
                for (unsigned x = 0; x < width; ++x)
                    texels[mip].push_back(float(mip) + .25f + float(x) / 8 + float(y) / 64);
            subresources[mip] = {texels[mip].data(), width * 4, 0};
        }
        std::vector<Configuration> configurations{
            {"point"},
            {"linear", 21},
            {"anisotropic", 85},
            {"bias", 0, 3, 2},
            {"minimum", 0, 3, 0, 1.5f},
            {"maximum", 0, 3, 0, -D3D11_FLOAT32_MAX, .5f},
            {"resource-minimum", 0, 3, 0, -D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX, 1.25f},
            {"view-mips", 0, 3, 0, -D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX, 0, 1, 2},
            {"border", 0, 4},
            {"wrap", 0, 1},
            {"mirror", 0, 2},
            {"mirror-once", 0, 5},
            {"null-resource"},
            {"null-sampler"}};
        configurations[12].unboundResource = true;
        configurations[13].unboundSampler = true;
        for (unsigned function : {1u, 2u, 3u, 5u, 6u, 7u, 8u}) {
            Configuration item{"comparison-function"};
            item.comparison = function;
            configurations.push_back(item);
        }
        const std::array<std::array<uint32_t, 4>, 4> sentinel{
            {{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
             {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
             {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
             {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef}}};
        for (const std::string operation : lod ? std::vector<std::string>{"lod"}
                                               : std::vector<std::string>{"sample", "sample_b", "sample_c"})
            for (unsigned offset = 0; offset < (lod ? 1u : 2u); ++offset)
                for (const std::string flow : {"uniform", "discard", "reconverge", "loop"}) {
                    const bool comparison = operation == "sample_c";
                    const std::string reduced = precision >= 5 ? "min10float" : "min16float";
                    const bool destinationPrecision = precision == 1 || precision >= 4;
                    std::string source =
                        "Texture2D<" + (destinationPrecision ? reduced : "float") + "4> image:register(t0);";
                    source += comparison ? "SamplerComparisonState state:register(s0);"
                                         : "SamplerState state:register(s0);";
                    source += "cbuffer Inputs:register(b0){uint4 bits[4];uint discarded;}"
                              "uint4 main(float4 position:SV_Position):SV_Target{"
                              "uint i=uint(position.x)+2*uint(position.y);float4 v=asfloat(bits[i]);";
                    if (flow == "discard")
                        source += "if(discarded&(1u<<i))discard;v.xy+=float2(i,2*i)*0.03125;";
                    if (flow == "reconverge")
                        source += "[branch]if(i&1)v.xy*=1.5;else v.xy-=0.125;";
                    if (flow == "loop")
                        source += "[loop]for(uint k=0;k<i;++k)v.xy+=float2(0.015625,0.03125);";
                    const bool addressPrecision = precision == 2 || precision == 4 || precision == 6;
                    const bool parameterPrecision = precision == 3 || precision == 4 || precision == 6;
                    if (addressPrecision)
                        source +=
                            reduced + "2 coordinates=" + reduced + "2(v.xy)*" + reduced + "2(1.0007,0.9991);";
                    if (parameterPrecision)
                        source += reduced + " parameter=" + reduced + "(v.z)*" + reduced + "(1.0007);";
                    if (lod) {
                        source += "float4 sampled=float4(image.CalculateLevelOfDetail(state,v.xy),"
                                  "image.CalculateLevelOfDetailUnclamped(state,v.xy),0,0);";
                    } else {
                        source += (destinationPrecision ? reduced : "float") + "4 sampled=image.";
                        source += comparison                ? "SampleCmp(state,"
                                  : operation == "sample_b" ? "SampleBias(state,"
                                                            : "Sample(state,";
                        source += addressPrecision ? "coordinates" : "v.xy";
                        if (comparison || operation == "sample_b")
                            source += parameterPrecision ? ",parameter" : ",v.z";
                        source += offset ? ",int2(-1,1)).xxxx;" : ").xxxx;";
                    }
                    if (destinationPrecision)
                        source += "sampled *= " + reduced + "(1.0007);";
                    source += "return asuint(float4(sampled));}";
                    auto code = compile(source, "ps_5_0");
                    const auto name = operation + "-" + flow + "-" + std::to_string(offset);
                    const bool patchedDestination = comparison && (precision == 1 || precision == 5);
                    if (patchedDestination) {
                        // FXC promotes a comparison sample with full-precision inputs. Exercise
                        // the legal reduced destination directly, retaining the compiled source
                        // and using the owned container codec only for binary serialization.
                        save(directory / (name + ".compiler.dxbc"), code.Get());
                        auto parts = flora::readDxbcParts(flora::Bytes(
                            static_cast<const uint8_t *>(code->GetBufferPointer()), code->GetBufferSize()));
                        std::vector<uint8_t> programBytes;
                        unsigned patched = 0;
                        for (auto &[kind, bytes] : parts)
                            if (kind == 0x58454853 || kind == 0x52444853) {
                                auto program = flora::readDxbcProgram(bytes);
                                std::optional<uint32_t> pendingResult;
                                for (auto &instruction : program.instructions) {
                                    if (pendingResult) {
                                        // The compiled multiply still reads the default-precision
                                        // register. Read the newly reduced sample with its own precision.
                                        if ((instruction[0] & 2047) != 56)
                                            throw std::runtime_error(
                                                "Unexpected comparison fixture result use");
                                        const size_t source = instruction.at(1) >> 31 ? 4 : 3;
                                        if ((instruction.at(source) >> 31) ||
                                            ((instruction[source] >> 12) & 255) != 0 ||
                                            instruction.at(source + 1) != *pendingResult)
                                            throw std::runtime_error("Unexpected comparison fixture source");
                                        instruction[source] |= 0x80000000u;
                                        instruction.insert(instruction.begin() + source + 1,
                                                           1u | ((precision == 5 ? 2u : 1u) << 14));
                                        instruction[0] = (instruction[0] & 0x80ffffff) |
                                                         (uint32_t(instruction.size()) << 24);
                                        pendingResult.reset();
                                        continue;
                                    }
                                    if ((instruction[0] & 2047) != 70)
                                        continue;
                                    size_t first = 1;
                                    while (instruction.at(first - 1) >> 31)
                                        ++first;
                                    if ((instruction.at(first) >> 31) ||
                                        ((instruction.at(first) >> 12) & 255) != 0)
                                        throw std::runtime_error("Unexpected comparison fixture destination");
                                    pendingResult = instruction.at(first + 1);
                                    instruction[first] |= 0x80000000u;
                                    instruction.insert(instruction.begin() + first + 1,
                                                       1u | ((precision == 5 ? 2u : 1u) << 14));
                                    instruction[0] =
                                        (instruction[0] & 0x80ffffff) | (uint32_t(instruction.size()) << 24);
                                    ++patched;
                                }
                                if (pendingResult)
                                    throw std::runtime_error("Missing comparison fixture result use");
                                programBytes = flora::writeDxbcProgram(program);
                                bytes = programBytes;
                            }
                        if (patched != 1)
                            throw std::runtime_error("Missing comparison fixture sample");
                        const auto binary = flora::makeDxbc(parts);
                        ComPtr<ID3DBlob> replacement;
                        check(D3DCreateBlob(binary.size(), &replacement));
                        std::memcpy(replacement->GetBufferPointer(), binary.data(), binary.size());
                        code = replacement;
                    }
                    save(directory / (name + ".dxbc"), code.Get());
                    ComPtr<ID3DBlob> assembly;
                    check(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr,
                                         &assembly));
                    save(directory / (name + ".asm.txt"), assembly.Get());
                    ComPtr<ID3D11PixelShader> ps;
                    check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                    &ps));
                    for (unsigned configuration = 0; configuration < (precision    ? 3
                                                                      : comparison ? configurations.size()
                                                                                   : 14);
                         ++configuration) {
                        const auto &config = configurations[configuration];
                        D3D11_TEXTURE2D_DESC descriptor{
                            8,
                            8,
                            4,
                            1,
                            DXGI_FORMAT_R32_FLOAT,
                            {1, 0},
                            D3D11_USAGE_IMMUTABLE,
                            D3D11_BIND_SHADER_RESOURCE,
                            0,
                            config.resourceMinimum ? D3D11_RESOURCE_MISC_RESOURCE_CLAMP : 0u};
                        ComPtr<ID3D11Texture2D> texture;
                        check(device->CreateTexture2D(&descriptor, subresources.data(), &texture));
                        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                        vd.Format = descriptor.Format;
                        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        vd.Texture2D.MostDetailedMip = config.firstMip;
                        vd.Texture2D.MipLevels = config.mipCount;
                        ComPtr<ID3D11ShaderResourceView> view;
                        check(device->CreateShaderResourceView(texture.Get(), &vd, &view));
                        D3D11_SAMPLER_DESC sd{};
                        sd.Filter = D3D11_FILTER(config.filter | (comparison ? 128 : 0));
                        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MODE(config.address);
                        sd.MipLODBias = config.bias;
                        sd.MinLOD = config.minimum;
                        sd.MaxLOD = config.maximum;
                        sd.MaxAnisotropy = 8;
                        sd.ComparisonFunc = D3D11_COMPARISON_FUNC(config.comparison);
                        sd.BorderColor[0] = .375f;
                        ComPtr<ID3D11SamplerState> sampler;
                        check(device->CreateSamplerState(&sd, &sampler));
                        std::array<uint32_t, 13> samplerBits{};
                        std::memcpy(samplerBits.data(), &sd, sizeof sd);
                        for (unsigned dataset = 0; dataset < 8; ++dataset) {
                            const std::string expectedFlow = dataset < 4    ? "uniform"
                                                             : dataset < 6  ? "discard"
                                                             : dataset == 6 ? "reconverge"
                                                                            : "loop";
                            if (flow != expectedFlow)
                                continue;
                            const unsigned discarded = dataset == 4 ? 1 : dataset == 5 ? 5 : 0;
                            std::array<std::array<uint32_t, 4>, 4> inputs;
                            for (unsigned i = 0; i < 4; ++i) {
                                const float scale = dataset == 0   ? 0.f
                                                    : dataset == 1 ? .03125f
                                                    : dataset == 2 ? .5f
                                                                   : .25f;
                                const float u = .375f + (i % 2) * scale + (precision ? .00137f : 0);
                                const float v = .375f + (i / 2) * scale + (precision ? .00271f : 0);
                                const std::array<float, 4> parameters{
                                    u, dataset == 3 && i == 3 ? v + 1 : v,
                                    comparison ? std::array<float, 4>{.125f, .5f, 1.f, 2.f}[i]
                                               : std::array<float, 4>{-2.f, -.5f, .5f, 2.f}[i],
                                    0};
                                inputs[i] = std::bit_cast<std::array<uint32_t, 4>>(parameters);
                            }
                            if (comparison && dataset == 6) {
                                inputs[2][2] = 0x7f800000;
                                inputs[3][2] = 0x7fc12345;
                            }
                            for (unsigned coverage : {15u, 3u, 5u, 1u}) {
                                std::array<std::array<uint32_t, 4>, 5> values{};
                                std::copy(inputs.begin(), inputs.end(), values.begin());
                                values[4][0] = discarded;
                                context->ClearState();
                                if (config.resourceMinimum)
                                    context->SetResourceMinLOD(texture.Get(), config.resourceMinimum);
                                context->UpdateSubresource(output.Get(), 0, nullptr, sentinel.data(), 32, 0);
                                context->UpdateSubresource(constants.Get(), 0, nullptr, values.data(), 0, 0);
                                auto *cb = constants.Get();
                                auto *rtv = target.Get();
                                auto *srv = config.unboundResource ? nullptr : view.Get();
                                auto *state = config.unboundSampler ? nullptr : sampler.Get();
                                const D3D11_VIEWPORT viewport{0, 0, 2, 2, 0, 1};
                                const D3D11_RECT rect{0, 0, coverage == 5 || coverage == 1 ? 1 : 2,
                                                      coverage == 3 || coverage == 1 ? 1 : 2};
                                context->VSSetShader(vs.Get(), nullptr, 0);
                                context->PSSetShader(ps.Get(), nullptr, 0);
                                context->PSSetConstantBuffers(0, 1, &cb);
                                context->PSSetShaderResources(0, 1, &srv);
                                context->PSSetSamplers(0, 1, &state);
                                context->RSSetState(raster.Get());
                                context->RSSetViewports(1, &viewport);
                                context->RSSetScissorRects(1, &rect);
                                context->OMSetRenderTargets(1, &rtv, nullptr);
                                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                                context->Draw(3, 0);
                                context->ClearState();
                                context->CopyResource(staging.Get(), output.Get());
                                context->Flush();
                                const auto deadline =
                                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
                                D3D11_MAPPED_SUBRESOURCE mapped{};
                                for (;;) {
                                    const auto status = context->Map(staging.Get(), 0, D3D11_MAP_READ,
                                                                     D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                                    if (status == S_OK)
                                        break;
                                    if (status != DXGI_ERROR_WAS_STILL_DRAWING)
                                        check(status);
                                    if (std::chrono::steady_clock::now() >= deadline)
                                        throw std::runtime_error("Implicit sampling readback timed out");
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                                std::array<std::array<uint32_t, 4>, 4> results;
                                for (unsigned y = 0; y < 2; ++y)
                                    std::memcpy(
                                        results.data() + 2 * y,
                                        static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch, 32);
                                context->Unmap(staging.Get(), 0);
                                const bool undefined = comparison && config.unboundSampler;
                                const auto diagnostics = validate(queue.Get(), undefined);
                                report["cases"].push_back(
                                    {{"operation", operation},
                                     {"flow", flow},
                                     {"offset", offset},
                                     {"configuration", configuration},
                                     {"name", config.name},
                                     {"shader", name + ".dxbc"},
                                     {"patched_comparison_destination", patchedDestination},
                                     {"dataset", dataset},
                                     {"coverage", coverage},
                                     {"discarded", discarded},
                                     {"inputs", inputs},
                                     {"output", results},
                                     {"comparison", config.comparison},
                                     {"sampler_descriptor",
                                      config.unboundSampler ? Json(nullptr) : Json(samplerBits)},
                                     {"resource_bound", !config.unboundResource},
                                     {"first_mip", config.firstMip},
                                     {"mip_count", config.mipCount},
                                     {"resource_min_lod", config.resourceMinimum},
                                     {"status",
                                      undefined ? "undefined_missing_comparison_sampler" : "observed"},
                                     {"diagnostics", diagnostics}});
                            }
                        }
                    }
                }
        report["unexpected_validation_errors"] = 0;
        std::ofstream file(directory / "results.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot save implicit sampling observations");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
