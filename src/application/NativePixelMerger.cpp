#include "NativePixelMerger.h"
#include "DxbcGpuCompletion.h"
#include "HlslCompilation.h"
#include "replay/BlendState.h"
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
    if (!reference || s.colorSlot >= 8 || laneIndex >= 4 || !s.timeoutMs || s.timeoutMs > 60000 ||
        (s.depthViewFlags & ~(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL)))
        throw std::invalid_argument("Invalid private pixel merger request");
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
    std::set<uint32_t> needed{s.colorSlot};
    const auto &blend = s.blend.RenderTarget[s.blend.IndependentBlendEnable ? s.colorSlot : 0];
    if (s.blend.AlphaToCoverageEnable)
        needed.insert(0);
    if (blend.BlendEnable && (dual(blend.SrcBlend) || dual(blend.DestBlend) || dual(blend.SrcBlendAlpha) ||
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
    for (unsigned i = 0; i < 2; ++i) {
        const auto format = i ? s.depthFormat : s.colorFormat;
        const auto &bytes = i ? s.depth : s.color;
        if (i && !format) {
            if (!bytes.empty() || s.depthViewFlags)
                throw std::invalid_argument("Unbound merger depth contains storage or flags");
            continue;
        }
        UINT support = 0;
        check(device->CheckFormatSupport(DXGI_FORMAT(format), &support), "Check merger format");
        if (!(support & (i ? D3D11_FORMAT_SUPPORT_DEPTH_STENCIL : D3D11_FORMAT_SUPPORT_RENDER_TARGET)) ||
            bytes.empty() || bytes.size() != pitches(1, 1, format).first)
            throw std::invalid_argument("Pixel merger storage format or length mismatch");
        D3D11_TEXTURE2D_DESC desc{1,
                                  1,
                                  1,
                                  1,
                                  DXGI_FORMAT(format),
                                  {1, 0},
                                  D3D11_USAGE_DEFAULT,
                                  UINT(i ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET),
                                  0,
                                  0};
        const D3D11_SUBRESOURCE_DATA data{bytes.data(), UINT(bytes.size()), UINT(bytes.size())};
        check(device->CreateTexture2D(&desc, &data, &textures[i]), "Create private merger storage");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &staging[i]), "Create private merger staging");
    }
    Com<ID3D11RenderTargetView> target;
    Com<ID3D11DepthStencilView> depth;
    check(device->CreateRenderTargetView(textures[0].Get(), nullptr, &target), "Create private merger RTV");
    if (textures[1]) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT(s.depthFormat);
        desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
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
    context->OMSetRenderTargets(s.colorSlot + 1, targets.data(), depth.Get());
    context->OMSetBlendState(blendState.Get(), s.blendFactor.data(), s.sampleMask);
    context->OMSetDepthStencilState(depthState.Get(), s.stencilRef);
    context->RSSetState(rasterState.Get());
    const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    auto *raw = cb.Get();
    context->PSSetConstantBuffers(0, 1, &raw);
    Com<ID3D11Query> query;
    const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION, 0};
    check(device->CreateQuery(&queryDesc, &query), "Create private merger query");
    checkCancellation(cancelled);
    context->Begin(query.Get());
    context->Draw(3, 0);
    context->End(query.Get());
    context->ClearState();
    for (unsigned i = 0; i < 2; ++i)
        if (textures[i])
            context->CopyResource(staging[i].Get(), textures[i].Get());
    dxbc_exec::GpuCompletion completion(device.Get(), context.Get(), cancelled, s.timeoutMs);
    completion.submit();
    uint64_t passing = 0;
    completion.query(query.Get(), &passing, sizeof(passing));
    std::array<std::vector<uint8_t>, 2> after;
    for (unsigned i = 0; i < 2; ++i)
        if (textures[i]) {
            const auto mapped = completion.map(staging[i].Get());
            const auto *bytes = static_cast<const uint8_t *>(mapped.pData);
            after[i].assign(bytes, bytes + (i ? s.depth.size() : s.color.size()));
            context->Unmap(staging[i].Get(), 0);
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
            {"backend", "native-pixel-merger/1"},
            {"scope", "private_single_sample_output_merger"},
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
            {"count_semantics", "d3d11_occlusion_query"},
            {"debug_validation", debug ? "no_errors" : "not_requested"}};
}
} // namespace flora
