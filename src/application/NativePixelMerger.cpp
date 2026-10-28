#include "NativePixelMerger.h"
#include "DxbcGpuCompletion.h"
#include "HlslCompilation.h"
#include "replay/BlendState.h"
#include "replay/NativeMsaaTransfer.h"
#include <d3d11sdklayers.h>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
DXGI_ADAPTER_DESC adapter(ID3D11Device *device, Com<IDXGIAdapter> &object) {
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Query merger DXGI device");
    check(dxgi->GetAdapter(&object), "Query merger adapter");
    DXGI_ADAPTER_DESC result{};
    check(object->GetDesc(&result), "Describe merger adapter");
    return result;
}
uint64_t luid(const DXGI_ADAPTER_DESC &d) {
    return (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
}
template <typename T> auto words(const T &value) {
    std::array<uint32_t, sizeof(T) / 4> result{};
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}
// Serialize fields explicitly so native structure padding never enters results.
std::vector<uint32_t> words(const D3D11_BLEND_DESC1 &value) {
    std::vector<uint32_t> result{uint32_t(value.AlphaToCoverageEnable),
                                 uint32_t(value.IndependentBlendEnable)};
    for (const auto &rt : value.RenderTarget)
        result.insert(result.end(),
                      {uint32_t(rt.BlendEnable), uint32_t(rt.LogicOpEnable), uint32_t(rt.SrcBlend),
                       uint32_t(rt.DestBlend), uint32_t(rt.BlendOp), uint32_t(rt.SrcBlendAlpha),
                       uint32_t(rt.DestBlendAlpha), uint32_t(rt.BlendOpAlpha), uint32_t(rt.LogicOp),
                       uint32_t(rt.RenderTargetWriteMask)});
    return result;
}
std::vector<uint32_t> words(const D3D11_DEPTH_STENCIL_DESC &value) {
    std::vector<uint32_t> result{uint32_t(value.DepthEnable), uint32_t(value.DepthWriteMask),
                                 uint32_t(value.DepthFunc), uint32_t(value.StencilEnable),
                                 uint32_t(value.StencilReadMask) | (uint32_t(value.StencilWriteMask) << 8)};
    for (const auto &face : {value.FrontFace, value.BackFace})
        result.insert(result.end(), {uint32_t(face.StencilFailOp), uint32_t(face.StencilDepthFailOp),
                                     uint32_t(face.StencilPassOp), uint32_t(face.StencilFunc)});
    return result;
}
bool dual(D3D11_BLEND factor) {
    return factor >= D3D11_BLEND_SRC1_COLOR && factor <= D3D11_BLEND_INV_SRC1_ALPHA;
}
Resource cellResource(uint32_t format, DXGI_SAMPLE_DESC samples, bool depth, uint32_t width,
                      uint32_t height) {
    Resource result;
    result.type = 0x85;
    result.desc = {width,
                   height,
                   1,
                   1,
                   format,
                   samples.Count,
                   samples.Quality,
                   D3D11_USAGE_DEFAULT,
                   uint32_t(depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET),
                   0,
                   0};
    return result;
}
Com<ID3D11Resource> createCell(ID3D11Device *device, const Resource &resource, std::optional<Bytes> bytes) {
    const auto info = textureInfo(resource);
    if (resource.type != 0x85 || !info.width || !info.height || info.layers != 1 || info.mips != 1)
        throw std::invalid_argument("Private merger transfer requires a 2D target");
    D3D11_TEXTURE2D_DESC desc{};
    if (resource.desc.size() * 4 != sizeof(desc))
        throw std::invalid_argument("Private merger transfer descriptor size");
    std::memcpy(&desc, resource.desc.data(), sizeof(desc));
    const auto [pitch, rows] = pitches(info.width, info.height, info.format);
    const uint64_t size = uint64_t(pitch) * rows;
    if (bytes && (info.samples != 1 || bytes->size() != size))
        throw std::invalid_argument("Private merger transfer initial storage size");
    const D3D11_SUBRESOURCE_DATA data{bytes ? bytes->data() : nullptr, pitch, UINT(size)};
    Com<ID3D11Texture2D> result;
    check(device->CreateTexture2D(&desc, bytes ? &data : nullptr, &result), "Create merger transfer cell");
    return result;
}
std::vector<uint8_t> readCell(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *source,
                              const Resource &resource, uint32_t x, uint32_t y, const CancelCheck &cancelled,
                              uint32_t timeout) {
    checkCancellation(cancelled);
    auto stagingResource = resource;
    const auto info = textureInfo(resource);
    if (info.samples != 1)
        throw std::invalid_argument("Merger staging transfer must select one sample");
    stagingResource.desc[7] = D3D11_USAGE_STAGING;
    stagingResource.desc[8] = 0;
    stagingResource.desc[9] = D3D11_CPU_ACCESS_READ;
    const auto staging = createCell(device, stagingResource, std::nullopt);
    context->CopyResource(staging.Get(), source);
    dxbc_exec::GpuCompletion completion(device, context, cancelled, timeout);
    completion.submit();
    const auto mapped = completion.map(staging.Get());
    const auto *data = static_cast<const uint8_t *>(mapped.pData) + uint64_t(y) * mapped.RowPitch +
                       uint64_t(x) * pitches(1, 1, info.format).first;
    std::vector<uint8_t> bytes(data, data + pitches(1, 1, info.format).first);
    context->Unmap(staging.Get(), 0);
    // X24 is not storage data. Keep its explicit invalid-byte marker in results.
    if (info.format == 20)
        std::fill(bytes.begin() + 5, bytes.end(), 0);
    return bytes;
}
std::vector<uint8_t> readSamples(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Resource *source,
                                 const Resource &resource, uint32_t x, uint32_t y,
                                 const CancelCheck &cancelled, uint32_t timeout) {
    std::vector<uint8_t> result;
    for (uint32_t sample = 0; sample < textureInfo(resource).samples; ++sample) {
        checkCancellation(cancelled);
        const auto read = readNativeMsaaStorage(device, context, source, resource, sample, resource.desc[4],
                                                [&](ID3D11Resource *target, const Resource &layout) {
                                                    return readCell(device, context, target, layout, x, y,
                                                                    cancelled, timeout);
                                                });
        if (read.mode != "selected_sample")
            throw std::runtime_error("Private merger sample transfer attempted a resolve");
        result.insert(result.end(), read.bytes.begin(), read.bytes.end());
    }
    return result;
}
} // namespace
NativePixelMergerState::NativePixelMergerState() : blend(defaultBlend()) {
    depthStencil.DepthEnable = TRUE;
    depthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencil.DepthFunc = D3D11_COMPARISON_LESS;
    depthStencil.StencilReadMask = depthStencil.StencilWriteMask = 255;
    depthStencil.FrontFace = depthStencil.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                                      D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
}
Json mergeNativePixelOutput(ID3D11Device *reference, bool warp, const NativePixelMergerState &s,
                            const Json &outputs, uint32_t laneIndex, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (!reference || (!s.colorFormat && !s.depthFormat) || s.colorSlot >= 8 || laneIndex >= 4 ||
        !s.timeoutMs || s.timeoutMs > 60000 || !s.width || !s.height || s.width > 16384 || s.height > 16384 ||
        s.x >= s.width || s.y >= s.height || !s.maxBytes || !s.samples.Count || s.samples.Count > 32 ||
        (s.samples.Count & (s.samples.Count - 1)) || (s.samples.Count == 1 && s.samples.Quality != 0) ||
        (s.depthViewFlags & ~(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL)))
        throw std::invalid_argument("Invalid private pixel merger request");
    uint64_t payload = 0;
    for (const auto format : {s.colorFormat, s.depthFormat})
        if (format) {
            const auto texel = pitches(1, 1, format).first;
            // Account for both bound targets, a same-size transfer target,
            // source/readback storage and CPU initialization payloads.
            const uint64_t bytes = uint64_t(s.width) * s.height * texel * (2 * s.samples.Count + 4);
            if (bytes > s.maxBytes - payload)
                throw std::runtime_error("Private pixel merger target payload budget exceeded");
            payload += bytes;
        }
    if (outputs.at("schema") != 1 || outputs.at("kind") != "native_pixel_shader_outputs" ||
        outputs.at("backend") != "native-pixel-outputs/1" ||
        outputs.at("scope") != "one_private_pixel_quad_shader_outputs" || outputs.at("lanes").size() != 4)
        throw std::invalid_argument("Private pixel merger requires native quad outputs");
    Com<IDXGIAdapter> originalAdapter;
    const auto expected = adapter(reference, originalAdapter);
    if (outputs.at("adapter").at("vendor") != expected.VendorId ||
        outputs.at("adapter").at("device") != expected.DeviceId ||
        outputs.at("adapter").at("luid") != luid(expected))
        throw std::invalid_argument("Pixel output and merger adapters differ");
    const auto &lane = outputs.at("lanes").at(laneIndex);
    if (lane.at("lane") != laneIndex ||
        lane.at("shader_active") !=
            Json(lane.at("initially_covered").get<bool>() && !lane.at("discarded").get<bool>()))
        throw std::invalid_argument("Pixel merger lane state is inconsistent");
    std::array<uint32_t, 36> constants{};
    constants[32] = s.rasterDepthBits.value_or(0);
    constants[33] = UINT32_MAX;
    constants[34] = !lane.at("shader_active").get<bool>();
    std::map<uint32_t, const Json *> colors;
    bool hasDepth = false, hasCoverage = false;
    for (const auto &output : lane.at("outputs")) {
        const auto kind = output.at("kind").get<std::string>();
        const auto index = output.at("index").get<uint32_t>();
        if (kind == "color") {
            if (index >= 8 || !colors.emplace(index, &output).second)
                throw std::invalid_argument("Invalid pixel merger color identity");
        } else {
            const bool coverage = kind == "coverage";
            if ((!coverage && kind != "depth" && kind != "depth_greater_equal" &&
                 kind != "depth_less_equal") ||
                index || output.at("declared_mask") != 1 || output.at("valid_mask") != 1 ||
                output.at("numeric") != (coverage ? "uint32" : "float32") ||
                (coverage ? hasCoverage : hasDepth))
                throw std::invalid_argument("Unknown or invalid pixel merger depth/coverage output");
            (coverage ? hasCoverage : hasDepth) = true;
            constants[coverage ? 33 : 32] = output.at("raw_bits").at(0).get<uint32_t>();
        }
    }
    if (s.depthFormat && !hasDepth && !s.rasterDepthBits)
        throw std::invalid_argument("Private pixel merger requires observed raster or shader depth");
    std::set<uint32_t> needed;
    if (s.colorFormat)
        needed.insert(s.colorSlot);
    const auto &blend = s.blend.RenderTarget[s.blend.IndependentBlendEnable ? s.colorSlot : 0];
    if (s.blend.AlphaToCoverageEnable)
        needed.insert(0);
    if (s.colorFormat && blend.BlendEnable &&
        (dual(blend.SrcBlend) || dual(blend.DestBlend) || dual(blend.SrcBlendAlpha) ||
         dual(blend.DestBlendAlpha))) {
        if (s.colorSlot != 0)
            throw std::invalid_argument("Dual-source blending requires target zero");
        needed.insert(1);
    }
    std::string fields, assignments;
    for (auto slot : needed) {
        const auto it = colors.find(slot);
        if (it == colors.end() || it->second->at("declared_mask") != 15 || it->second->at("valid_mask") != 15)
            throw std::runtime_error("Private pixel merger requires known complete color outputs");
        const auto &field = *it->second;
        const auto numeric = field.at("numeric").get<std::string>();
        const auto type = numeric == "float32"  ? "float4"
                          : numeric == "uint32" ? "uint4"
                          : numeric == "int32"  ? "int4"
                                                : nullptr;
        if (!type)
            throw std::invalid_argument("Invalid pixel merger numeric type");
        const auto raw = field.at("raw_bits").get<std::array<uint32_t, 4>>();
        std::copy(raw.begin(), raw.end(), constants.begin() + slot * 4);
        const auto n = std::to_string(slot);
        fields += std::string(type) + " c" + n + ":SV_Target" + n + ";";
        assignments += "o.c" + n + "=" +
                       (numeric == "float32" ? "asfloat"
                        : numeric == "int32" ? "asint"
                                             : "uint4") +
                       "(values[" + n + "]);";
    }
    const auto ps =
        compileHlsl("cbuffer C:register(b0){uint4 values[8];uint4 control;}struct O{" + fields +
                        "float depth:SV_Depth;" + (hasCoverage ? "uint coverage:SV_Coverage;" : "") +
                        "};O main(){if(control.z)discard;O o;" + assignments + "o.depth=asfloat(control.x);" +
                        (hasCoverage ? "o.coverage=control.y;" : "") + "return o;}",
                    "ps_5_0")
            .bytecode;
    const auto vs =
        compileHlsl("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}",
                    "vs_5_0")
            .bytecode;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    const auto level = reference->GetFeatureLevel();
    if (level < D3D_FEATURE_LEVEL_11_0)
        throw std::runtime_error("Private pixel merger requires feature level 11.0");
    const bool debug = (reference->GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG) != 0;
    check(D3D11CreateDevice(warp ? nullptr : originalAdapter.Get(),
                            warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            debug ? D3D11_CREATE_DEVICE_DEBUG : 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, &context),
          "Create private merger device");
    Com<IDXGIAdapter> actualAdapter;
    const auto actual = adapter(device.Get(), actualAdapter);
    if (luid(actual) != luid(expected))
        throw std::runtime_error("Private pixel merger adapter changed");
    std::array<Com<ID3D11Texture2D>, 2> textures, staging;
    std::array<Resource, 2> layouts;
    for (unsigned i = 0; i < 2; ++i) {
        const auto format = i ? s.depthFormat : s.colorFormat;
        const auto &bytes = i ? s.depth : s.color;
        if (!format) {
            if (!bytes.empty() || (i && s.depthViewFlags))
                throw std::invalid_argument("Unbound merger target contains storage or flags");
            continue;
        }
        UINT support = 0;
        check(device->CheckFormatSupport(DXGI_FORMAT(format), &support), "Check merger format");
        if (!(support & (i ? D3D11_FORMAT_SUPPORT_DEPTH_STENCIL : D3D11_FORMAT_SUPPORT_RENDER_TARGET)) ||
            bytes.empty() || bytes.size() != uint64_t(pitches(1, 1, format).first) * s.samples.Count)
            throw std::invalid_argument("Pixel merger storage format or length mismatch");
        layouts[i] = cellResource(format, s.samples, i != 0, s.width, s.height);
        if (s.samples.Count > 1) {
            const auto encoding = msaaEditEncoding(layouts[i], format);
            auto storage = layouts[i];
            storage.desc[4] = encoding.storage;
            auto object = createCell(device.Get(), storage, std::nullopt);
            check(object.As(&textures[i]), "Get private multisample cell");
            const auto size = pitches(1, 1, format).first;
            for (uint32_t sample = 0; sample < s.samples.Count; ++sample) {
                checkCancellation(cancelled);
                TexturePatch patch;
                patch.sample = sample;
                patch.typedFormat = format;
                patch.bytes.resize(uint64_t(s.width) * s.height * size);
                std::copy_n(bytes.begin() + size * sample, size,
                            patch.bytes.begin() + (uint64_t(s.y) * s.width + s.x) * size);
                writeNativeMsaaSample(device.Get(), context.Get(), textures[i].Get(), layouts[i], patch,
                                      [&](const Resource &r, std::optional<Bytes> data) {
                                          return createCell(device.Get(), r, data);
                                      });
            }
            if (readSamples(device.Get(), context.Get(), textures[i].Get(), layouts[i], s.x, s.y, cancelled,
                            s.timeoutMs) != bytes)
                throw std::runtime_error("Private merger sample initialization did not preserve storage");
            continue;
        }
        D3D11_TEXTURE2D_DESC desc{s.width,
                                  s.height,
                                  1,
                                  1,
                                  DXGI_FORMAT(format),
                                  {1, 0},
                                  D3D11_USAGE_DEFAULT,
                                  UINT(i ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET),
                                  0,
                                  0};
        const auto pitch = uint64_t(s.width) * bytes.size();
        std::vector<uint8_t> initial(pitch * s.height);
        std::copy(bytes.begin(), bytes.end(), initial.begin() + uint64_t(s.y) * pitch + s.x * bytes.size());
        const D3D11_SUBRESOURCE_DATA data{initial.data(), UINT(pitch), UINT(initial.size())};
        check(device->CreateTexture2D(&desc, &data, &textures[i]), "Create private merger storage");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &staging[i]), "Create private merger staging");
    }
    Com<ID3D11RenderTargetView> target;
    Com<ID3D11DepthStencilView> depth;
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
    targetDesc.Format = DXGI_FORMAT(s.colorFormat);
    targetDesc.ViewDimension =
        s.samples.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    if (textures[0])
        check(device->CreateRenderTargetView(textures[0].Get(), &targetDesc, &target),
              "Create private merger RTV");
    if (textures[1]) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT(s.depthFormat);
        desc.ViewDimension =
            s.samples.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        desc.Flags = s.depthViewFlags;
        check(device->CreateDepthStencilView(textures[1].Get(), &desc, &depth), "Create private merger DSV");
    }
    Com<ID3D11Device1> device1;
    Com<ID3D11BlendState1> blendState;
    Com<ID3D11DepthStencilState> depthState;
    check(device.As(&device1), "Query private merger device1");
    check(device1->CreateBlendState1(&s.blend, &blendState), "Create private merger blend state");
    check(device->CreateDepthStencilState(&s.depthStencil, &depthState), "Create private merger depth state");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable = s.samples.Count > 1;
    raster.FrontCounterClockwise = !s.frontFace;
    Com<ID3D11RasterizerState> rasterState;
    check(device->CreateRasterizerState(&raster, &rasterState), "Create private merger raster state");
    Com<ID3D11VertexShader> vertex;
    Com<ID3D11PixelShader> pixel;
    Com<ID3D11Buffer> cb;
    check(device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex), "Create private merger VS");
    check(device->CreatePixelShader(ps.data(), ps.size(), nullptr, &pixel), "Create private merger PS");
    const D3D11_BUFFER_DESC buffer{
        sizeof(constants), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA data{constants.data(), 0, 0};
    check(device->CreateBuffer(&buffer, &data, &cb), "Create private merger inputs");
    std::array<ID3D11RenderTargetView *, 8> targets{};
    targets[s.colorSlot] = target.Get();
    context->OMSetRenderTargets(target ? s.colorSlot + 1 : 0, targets.data(), depth.Get());
    context->OMSetBlendState(blendState.Get(), s.blendFactor.data(), s.sampleMask & s.coverageMask);
    context->OMSetDepthStencilState(depthState.Get(), s.stencilRef);
    context->RSSetState(rasterState.Get());
    const D3D11_VIEWPORT viewport{float(s.x), float(s.y), 1, 1, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    auto *raw = cb.Get();
    context->PSSetConstantBuffers(0, 1, &raw);
    Com<ID3D11Query> query;
    const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION, 0};
    check(device->CreateQuery(&queryDesc, &query), "Create private merger query");
    Json testObservations;
    if (s.measureTests) {
        // All probes read the initial storage. No color, depth or stencil writes
        // are permitted, including stencil fail/depth-fail operations.
        auto diagnosticBlend = s.blend;
        for (auto &rt : diagnosticBlend.RenderTarget)
            rt.RenderTargetWriteMask = 0;
        Com<ID3D11BlendState1> combinedBlend, independentBlend;
        check(device1->CreateBlendState1(&diagnosticBlend, &combinedBlend), "Create merger test blend");
        diagnosticBlend.AlphaToCoverageEnable = FALSE;
        check(device1->CreateBlendState1(&diagnosticBlend, &independentBlend),
              "Create independent merger test blend");
        auto diagnosticDepth = s.depthStencil;
        diagnosticDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        diagnosticDepth.StencilWriteMask = 0;
        std::array<Com<ID3D11DepthStencilState>, 4> testStates;
        for (unsigned mode = 0; mode < testStates.size(); ++mode) {
            auto desc = diagnosticDepth;
            if (mode == 0 || mode == 2)
                desc.StencilEnable = FALSE;
            if (mode == 1 || mode == 2)
                desc.DepthEnable = FALSE;
            check(device->CreateDepthStencilState(&desc, &testStates[mode]), "Create merger test depth");
        }
        // Ignore shader discard and SV_Coverage for independent fixed-function
        // tests. Keep the original output alpha and supplied depth unchanged.
        const auto diagnosticCode =
            compileHlsl("cbuffer C:register(b0){uint4 values[8];uint4 control;}struct O{" + fields +
                            "float depth:SV_Depth;};O main(){O o;" + assignments +
                            "o.depth=asfloat(control.x);return o;}",
                        "ps_5_0")
                .bytecode;
        Com<ID3D11PixelShader> diagnosticPixel;
        check(device->CreatePixelShader(diagnosticCode.data(), diagnosticCode.size(), nullptr,
                                        &diagnosticPixel),
              "Create independent merger test shader");
        std::vector<std::array<Com<ID3D11Query>, 4>> testQueries(s.samples.Count);
        for (uint32_t sample = 0; sample < s.samples.Count; ++sample)
            for (unsigned mode = 0; mode < 4; ++mode) {
                checkCancellation(cancelled);
                auto &q = testQueries[sample][mode];
                check(device->CreateQuery(&queryDesc, &q), "Create sample merger test query");
                context->PSSetShader(mode == 3 ? pixel.Get() : diagnosticPixel.Get(), nullptr, 0);
                context->OMSetDepthStencilState(testStates[mode].Get(), s.stencilRef);
                context->OMSetBlendState(
                    mode < 2 ? independentBlend.Get() : combinedBlend.Get(), s.blendFactor.data(),
                    (1u << sample) & (mode == 3 ? s.sampleMask & s.coverageMask : UINT32_MAX));
                context->Begin(q.Get());
                context->Draw(3, 0);
                context->End(q.Get());
            }
        dxbc_exec::GpuCompletion tests(device.Get(), context.Get(), cancelled, s.timeoutMs);
        tests.submit();
        testObservations = {{"schema", 1},
                            {"scope", "private_late_tests_at_initial_storage"},
                            {"depth_enabled", bool(depth && s.depthStencil.DepthEnable)},
                            {"stencil_enabled", bool(depth && s.depthStencil.StencilEnable)},
                            {"alpha_to_coverage_enabled", bool(s.blend.AlphaToCoverageEnable)},
                            {"shader_active", lane.at("shader_active")},
                            {"shader_discarded", lane.at("discarded")},
                            {"initially_covered", lane.at("initially_covered")},
                            {"shader_coverage_mask", constants[33]},
                            {"target_writes", false},
                            {"draws", s.samples.Count * 4},
                            {"samples", Json::array()}};
        const std::array<const char *, 4> names{"depth", "stencil", "alpha_to_coverage", "combined"};
        for (uint32_t sample = 0; sample < s.samples.Count; ++sample) {
            Json row{{"sample", sample},
                     {"raster_covered", bool(s.coverageMask & (1u << sample))},
                     {"sample_enabled", bool(s.sampleMask & (1u << sample))},
                     {"shader_covered", bool(constants[33] & (1u << sample))}};
            for (unsigned mode = 0; mode < 4; ++mode) {
                uint64_t count = 0;
                tests.query(testQueries[sample][mode].Get(), &count, sizeof count);
                if (count > 1)
                    throw std::runtime_error("Private merger test exceeded one storage sample");
                row[names[mode]] = {{"passing_samples", count}, {"passed", count == 1}};
            }
            testObservations["samples"].push_back(std::move(row));
        }
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->OMSetDepthStencilState(depthState.Get(), s.stencilRef);
        context->OMSetBlendState(blendState.Get(), s.blendFactor.data(), s.sampleMask & s.coverageMask);
    }
    checkCancellation(cancelled);
    context->Begin(query.Get());
    context->Draw(3, 0);
    context->End(query.Get());
    context->ClearState();
    for (unsigned i = 0; i < 2; ++i)
        if (staging[i])
            context->CopyResource(staging[i].Get(), textures[i].Get());
    dxbc_exec::GpuCompletion completion(device.Get(), context.Get(), cancelled, s.timeoutMs);
    completion.submit();
    uint64_t passing = 0;
    completion.query(query.Get(), &passing, sizeof(passing));
    if (s.measureTests) {
        uint64_t sum = 0;
        for (const auto &sample : testObservations.at("samples"))
            sum += sample.at("combined").at("passing_samples").get<uint64_t>();
        if (sum != passing)
            throw std::runtime_error("Independent sample tests disagree with the combined merger query");
    }
    std::array<std::vector<uint8_t>, 2> after;
    for (unsigned i = 0; i < 2; ++i)
        if (textures[i]) {
            if (s.samples.Count > 1) {
                after[i] = readSamples(device.Get(), context.Get(), textures[i].Get(), layouts[i], s.x, s.y,
                                       cancelled, s.timeoutMs);
                continue;
            }
            const auto mapped = completion.map(staging[i].Get());
            const auto *bytes = static_cast<const uint8_t *>(mapped.pData) + uint64_t(s.y) * mapped.RowPitch +
                                uint64_t(s.x) * (i ? s.depth.size() : s.color.size());
            after[i].assign(bytes, bytes + (i ? s.depth.size() : s.color.size()));
            context->Unmap(staging[i].Get(), 0);
            if (i && s.depthFormat == 20)
                std::fill(after[i].begin() + 5, after[i].end(), 0);
        }
    if (debug) {
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Get private merger validation");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Size merger validation");
            std::vector<uint8_t> storage(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(queue->GetMessage(i, message, &size), "Read merger validation");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
    }
    const auto source = outputs.dump();
    return {{"schema", 1},
            {"kind", "native_pixel_merger"},
            {"backend", "native-pixel-merger/4"},
            {"scope", s.samples.Count > 1 ? "private_multisample_output_merger"
                                          : "private_single_sample_output_merger"},
            {"sample_count", s.samples.Count},
            {"sample_quality", s.samples.Quality},
            {"storage_layout", "sample_major_native_texels"},
            {"color_invalid_byte_offsets", s.samples.Count > 1 && (s.colorFormat == 88 || s.colorFormat == 93)
                                               ? Json::array({3})
                                               : Json::array()},
            {"depth_invalid_byte_offsets", s.depthFormat == 20 ? Json::array({5, 6, 7}) : Json::array()},
            {"coverage_mask", s.coverageMask},
            {"effective_sample_mask", s.sampleMask & s.coverageMask},
            {"private_pixel_location", Json::array({s.x, s.y})},
            {"private_target_extent", Json::array({s.width, s.height})},
            {"logical_payload_bound", payload},
            {"payload_limit", s.maxBytes},
            {"original_fragment_eligibility", nullptr},
            {"original_early_depth_reconstructed", false},
            {"lane", laneIndex},
            {"source_sha256",
             sha256(Bytes(reinterpret_cast<const uint8_t *>(source.data()), source.size()), cancelled)},
            {"adapter", outputs.at("adapter")},
            {"color_slot", s.colorSlot},
            {"color_format", s.colorFormat},
            {"depth_format", s.depthFormat},
            {"color_before", s.color},
            {"color_after", after[0]},
            {"depth_before", s.depth},
            {"depth_after", after[1]},
            {"front_face", s.frontFace},
            {"blend_descriptor", words(s.blend)},
            {"depth_stencil_descriptor", words(s.depthStencil)},
            {"blend_factor_bits", words(s.blendFactor)},
            {"sample_mask", s.sampleMask},
            {"stencil_ref", s.stencilRef},
            {"depth_view_flags", s.depthViewFlags},
            {"raster_depth_bits", s.rasterDepthBits ? Json(*s.rasterDepthBits) : Json(nullptr)},
            {"passing_samples", passing},
            {"test_observations", testObservations},
            {"count_semantics", "d3d11_occlusion_query"},
            {"debug_validation", debug ? "no_errors" : "not_requested"}};
}
} // namespace flora
