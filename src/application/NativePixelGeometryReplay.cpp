#include "NativePixelGeometryReplay.h"
#include "DrawParameters.h"
#include "DxbcGpuCompletion.h"
#include "DxbcPixelCompletion.h"
#include "DxbcPixelGeometry.h"
#include "DxbcPixelRaster.h"
#include "HlslCompilation.h"
#include "core/ClassLinkage.h"
#include "core/StreamOutput.h"
#include "replay/BlendState.h"
#include "replay/NativeMsaaTransfer.h"
#include "replay/Unpredicated.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <d3d11sdklayers.h>
#include <set>
namespace flora {
using Json = nlohmann::json;
Json decodeNativePixelReturnCount(std::array<uint32_t, 4> bits) {
    const float value = std::bit_cast<float>(bits[0]);
    const bool consistent = bits[0] == bits[1] && bits[0] == bits[2];
    const bool integral = std::isfinite(value) && value >= 0 && std::floor(value) == value;
    const bool exact = consistent && integral && value < 16777216.0f;
    return {{"raw_bits", bits},
            {"valid", exact},
            {"count", exact ? Json(uint32_t(value)) : Json(nullptr)},
            {"reason", exact         ? "exact_float32_integer"
                       : !consistent ? "rgb_disagreement"
                       : !integral   ? "invalid_accumulation"
                                     : "float32_integer_limit"},
            {"exclusive_integer_limit", 16777216},
            {"original_fragment_eligibility", nullptr}};
}
const char *nativePixelTestOverrideName(NativePixelTestOverride mode) {
    switch (mode) {
    case NativePixelTestOverride::Preserve:
        return "preserve";
    case NativePixelTestOverride::DisableDepth:
        return "disable_depth";
    case NativePixelTestOverride::DisableStencil:
        return "disable_stencil";
    case NativePixelTestOverride::DisableDepthStencil:
        return "disable_depth_stencil";
    case NativePixelTestOverride::DisableAlphaToCoverage:
        return "disable_alpha_to_coverage";
    case NativePixelTestOverride::AllSamples:
        return "all_samples";
    }
    throw std::invalid_argument("Unknown private pixel test override");
}
namespace {
Json depthIdentity(const D3D11_DEPTH_STENCIL_DESC &d) {
    auto face = [](const D3D11_DEPTH_STENCILOP_DESC &f) -> Json {
        return {{"fail", uint32_t(f.StencilFailOp)},
                {"depth_fail", uint32_t(f.StencilDepthFailOp)},
                {"pass", uint32_t(f.StencilPassOp)},
                {"function", uint32_t(f.StencilFunc)}};
    };
    return {{"depth_enabled", d.DepthEnable != FALSE},
            {"depth_write_mask", uint32_t(d.DepthWriteMask)},
            {"depth_function", uint32_t(d.DepthFunc)},
            {"stencil_enabled", d.StencilEnable != FALSE},
            {"stencil_read_mask", d.StencilReadMask},
            {"stencil_write_mask", d.StencilWriteMask},
            {"front", face(d.FrontFace)},
            {"back", face(d.BackFace)}};
}
} // namespace
class NativePixelGeometryReplay {
    Replay &r;
    Id id;
    NativePixelGeometryRange request;
    const NativePixelCheckpointPlanner *planner;
    const std::function<void(Replay &, uint64_t)> &observer;
    CancelCheck cancelled;
    std::chrono::steady_clock::time_point deadline;
    void checkDeadline() const {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native pixel geometry replay deadline");
    }
    Bytes shader(Id resource) const {
        if (auto it = r.options_.shaders.find(resource); it != r.options_.shaders.end())
            return it->second;
        return r.frame_.shader(r.frame_.resource(resource).data);
    }
    void validate() const {
        if (!r.options_.debug)
            return;
        Com<ID3D11InfoQueue> queue;
        check(r.device_.As(&queue), "Get private pixel replay validation");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Size pixel replay validation");
            std::vector<uint8_t> bytes(size);
            auto *m = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, m, &size), "Read pixel replay validation");
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(m->pDescription);
        }
    }

  public:
    NativePixelGeometryReplay(Replay &replay, Id event, const NativePixelGeometryRange &selection,
                              const std::function<void(Replay &, uint64_t)> &observe,
                              const CancelCheck &cancel, const NativePixelCheckpointPlanner *plan = nullptr)
        : r(replay), id(event), request(selection), planner(plan), observer(observe), cancelled(cancel),
          deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(selection.timeoutMs)) {}
    Json original(const std::function<void(Replay &, bool)> &observe,
                  std::optional<uint32_t> completionTarget = {},
                  std::optional<PixelPrimitiveMask> primitiveMask = {},
                  std::array<uint32_t, 3> marker = {0x3f800000, 0x40000000, 0x40400000},
                  bool accumulatePredicate = false,
                  NativePixelTestOverride testOverride = NativePixelTestOverride::Preserve,
                  std::optional<NativeDepthCompletionSelection> depthSelection = {},
                  std::optional<NativePixelOrdinalSelection> ordinalSelection = {},
                  std::optional<NativePixelReturnCountSelection> countSelection = {},
                  bool countDepthReturns = false) {
        const bool countReturns = countSelection.has_value() || countDepthReturns;
        if (countDepthReturns && (!depthSelection || countSelection || primitiveMask || accumulatePredicate ||
                                  ordinalSelection || testOverride != NativePixelTestOverride::Preserve))
            throw std::invalid_argument("Invalid private depth return count request");
        checkDeadline();
        const auto overrideName = nativePixelTestOverrideName(testOverride);
        if (testOverride != NativePixelTestOverride::Preserve && !accumulatePredicate)
            throw std::invalid_argument("Pixel test overrides require accumulated primitive presence");
        if (!r.replayComplete_ || !r.options_.before || r.options_.until != id)
            throw std::runtime_error("Private original draw requires a completed before-event replay");
        const auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d || !request.maxBytes || !request.timeoutMs ||
            request.timeoutMs > 60000)
            throw std::invalid_argument("Invalid private original draw request");
        if (completionTarget && *completionTarget >= 8)
            throw std::invalid_argument("Invalid private pixel completion target");
        if (accumulatePredicate && (!completionTarget || !primitiveMask))
            throw std::invalid_argument("Pixel presence requires a target and primitive predicate");
        if (ordinalSelection &&
            (ordinalSelection->target >= 8 || ordinalSelection->ordinal >= 255 || depthSelection ||
             primitiveMask || accumulatePredicate || testOverride != NativePixelTestOverride::Preserve))
            throw std::invalid_argument("Invalid private pixel ordinal selection; supported range is 0..254");
        if (countSelection && (countSelection->target >= 8 || completionTarget != countSelection->target ||
                               primitiveMask || accumulatePredicate || depthSelection || ordinalSelection ||
                               testOverride != NativePixelTestOverride::Preserve))
            throw std::invalid_argument("Invalid private pixel return count selection");
        const auto state = r.prepareState(event);
        const CancelCheck scoped = [&] {
            checkDeadline();
            return false;
        };
        Json report;
        r.inspectEventInputs(id, [&] {
            r.bind(state, false);
            r.applyGraphicsEdits(event, state);
            const auto cloneBytes = r.privateOutputPayloadBytes(state, true, scoped);
            if (cloneBytes > request.maxBytes)
                throw std::runtime_error("Private original draw output payload budget exceeded");
            const auto parameters = resolveDrawParameters(r, event);
            const auto raster = dxbc_exec::pixelRasterState(r.context_.Get());
            if ((ordinalSelection || countReturns) && raster.rasterizer.ForcedSampleCount)
                throw std::runtime_error(
                    "Private pixel diagnostic transport is incompatible with forced sampling");
            Json shaders = Json::array();
            for (unsigned stage = 0; stage < 5; ++stage) {
                const auto resource = state.stages[stage].shader;
                shaders.push_back(
                    {{"stage", stage},
                     {"resource", resource},
                     {"sha256", resource ? Json(sha256(shader(resource), scoped)) : Json(nullptr)}});
            }
            const bool enabled = !r.options_.suppressDraws && !r.options_.disabled.contains(id);
            report = {{"schema", 1},
                      {"kind", "native_original_draw_observation"},
                      {"backend", "native-original-draw/1"},
                      {"event", id},
                      {"scope", "original_draw_on_private_outputs"},
                      {"frame_sha256", r.frame_.sha256(scoped)},
                      {"enabled", enabled},
                      {"command_type", event.type},
                      {"parameters", parameters.parameters},
                      {"indirect", parameters.indirect},
                      {"shaders", shaders},
                      {"raster_state", dxbc_exec::pixelRasterIdentity(raster)},
                      {"private_output_payload_bytes", cloneBytes},
                      {"payload_limit", request.maxBytes},
                      {"original_fragment_eligibility", nullptr},
                      {"original_gpu_schedule", false},
                      {"diagnostic_shader_replacements", false},
                      {"extra_uav_slots", 0},
                      {"count_domain", "entire_bound_output"}};
            r.withPrivateOutputs(
                event, state,
                [&] {
                    Com<ID3D11PixelShader> completionShader;
                    std::vector<uint8_t> ordinalInput;
                    Com<ID3D11BlendState1> completionBlend;
                    Com<ID3D11DepthStencilState> testDepthState;
                    Com<ID3D11Texture2D> diagnosticTransport;
                    Resource transportResource;
                    uint64_t workingBytes = cloneBytes;
                    bool auxiliaryCount = false;
                    if (depthSelection) {
                        auto *context = r.context_.Get();
                        std::array<ID3D11RenderTargetView *, 8> raw{};
                        Com<ID3D11DepthStencilView> depth;
                        context->OMGetRenderTargets(8, raw.data(), &depth);
                        bool colorBound = false;
                        for (auto *view : raw)
                            if (view) {
                                colorBound = true;
                                view->Release();
                            }
                        Com<ID3D11UnorderedAccessView> slotZero;
                        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1,
                                                                           &slotZero);
                        if (!depth || colorBound || slotZero || completionTarget != 0)
                            throw std::runtime_error(
                                "Depth completion requires a DSV, no RTVs and an unused UAV slot zero");
                        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                        depth->GetDesc(&view);
                        Com<ID3D11Resource> source;
                        depth->GetResource(&source);
                        Com<ID3D11Texture2D> texture;
                        check(source.As(&texture), "Get 2D depth completion source");
                        D3D11_TEXTURE2D_DESC desc{};
                        texture->GetDesc(&desc);
                        uint32_t mip = 0, firstLayer = 0, layers = 1;
                        switch (view.ViewDimension) {
                        case D3D11_DSV_DIMENSION_TEXTURE2D:
                            mip = view.Texture2D.MipSlice;
                            break;
                        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
                            mip = view.Texture2DArray.MipSlice;
                            firstLayer = view.Texture2DArray.FirstArraySlice;
                            layers = view.Texture2DArray.ArraySize;
                            break;
                        case D3D11_DSV_DIMENSION_TEXTURE2DMS:
                            break;
                        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
                            firstLayer = view.Texture2DMSArray.FirstArraySlice;
                            layers = view.Texture2DMSArray.ArraySize;
                            break;
                        default:
                            throw std::runtime_error("Unsupported depth completion view dimension");
                        }
                        desc.Width = std::max(1u, desc.Width >> mip);
                        desc.Height = std::max(1u, desc.Height >> mip);
                        if (depthSelection->x >= desc.Width || depthSelection->y >= desc.Height ||
                            depthSelection->layer >= layers ||
                            depthSelection->sample >= desc.SampleDesc.Count)
                            throw std::invalid_argument("Depth completion selection exceeds its DSV");
                        const uint64_t plane = uint64_t(desc.Width) * desc.Height * 16;
                        if (!layers || !desc.SampleDesc.Count ||
                            plane > (request.maxBytes - workingBytes) / 3 / layers / desc.SampleDesc.Count)
                            throw std::runtime_error("Depth completion transport payload budget exceeded");
                        workingBytes += plane * layers * desc.SampleDesc.Count * 3;
                        desc.MipLevels = 1;
                        desc.ArraySize = layers;
                        desc.Format = desc.SampleDesc.Count > 1 ? DXGI_FORMAT_R32G32B32A32_TYPELESS
                                                                : DXGI_FORMAT_R32G32B32A32_FLOAT;
                        desc.Usage = D3D11_USAGE_DEFAULT;
                        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        desc.CPUAccessFlags = desc.MiscFlags = 0;
                        check(r.device_->CreateTexture2D(&desc, nullptr, &diagnosticTransport),
                              "Create depth completion transport");
                        Com<ID3D11RenderTargetView> target;
                        D3D11_RENDER_TARGET_VIEW_DESC targetView{};
                        targetView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                        if (desc.SampleDesc.Count > 1) {
                            targetView.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                            targetView.Texture2DMSArray = {0, layers};
                        } else {
                            targetView.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                            targetView.Texture2DArray = {0, 0, layers};
                        }
                        check(r.device_->CreateRenderTargetView(diagnosticTransport.Get(), &targetView,
                                                                &target),
                              "Create depth completion RTV");
                        auto *pointer = target.Get();
                        context->OMSetRenderTargetsAndUnorderedAccessViews(
                            1, &pointer, depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                        transportResource.type = 0x85;
                        transportResource.desc = {desc.Width,
                                                  desc.Height,
                                                  1,
                                                  layers,
                                                  uint32_t(desc.Format),
                                                  desc.SampleDesc.Count,
                                                  desc.SampleDesc.Quality,
                                                  0,
                                                  desc.BindFlags,
                                                  0,
                                                  0};
                        report["depth_transport"] = {{"source_format", uint32_t(view.Format)},
                                                     {"source_mip", mip},
                                                     {"source_first_layer", firstLayer},
                                                     {"width", desc.Width},
                                                     {"height", desc.Height},
                                                     {"layers", layers},
                                                     {"samples", desc.SampleDesc.Count},
                                                     {"format", uint32_t(desc.Format)},
                                                     {"view_format", uint32_t(targetView.Format)},
                                                     {"x", depthSelection->x},
                                                     {"y", depthSelection->y},
                                                     {"layer", depthSelection->layer},
                                                     {"sample", depthSelection->sample},
                                                     {"layer_domain", "bound_dsv_view"},
                                                     {"budgeted_bytes", workingBytes - cloneBytes}};
                    }
                    if (countSelection) {
                        auto *context = r.context_.Get();
                        std::array<ID3D11RenderTargetView *, 8> raw{};
                        std::array<Com<ID3D11RenderTargetView>, 8> views;
                        Com<ID3D11DepthStencilView> depth;
                        context->OMGetRenderTargets(8, raw.data(), &depth);
                        UINT count = 0;
                        for (UINT i = 0; i < 8; ++i) {
                            views[i].Attach(raw[i]);
                            if (raw[i])
                                count = i + 1;
                        }
                        const auto sourceTarget = countSelection->target;
                        auto target = sourceTarget;
                        if (!views[sourceTarget])
                            throw std::runtime_error("Private pixel count target is not bound");
                        if (target == 0 && state.stages[4].shader) {
                            Com<ID3D11BlendState> originalBlend;
                            context->OMGetBlendState(&originalBlend, nullptr, nullptr);
                            D3D11_BLEND_DESC desc{};
                            if (originalBlend)
                                originalBlend->GetDesc(&desc);
                            if (desc.AlphaToCoverageEnable) {
                                const auto output =
                                    instrumentPixelReturnCount(shader(state.stages[4].shader), 0,
                                                               request.maxBytes - workingBytes, scoped);
                                if (output.identity.at("component_type") != 3 &&
                                    !output.identity.at("coverage_output_declared").get<bool>()) {
                                    Com<ID3D11UnorderedAccessView> occupied;
                                    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1,
                                                                                       1, &occupied);
                                    if (occupied)
                                        throw std::runtime_error(
                                            "Auxiliary pixel count target overlaps UAV slot 1");
                                    target = 1;
                                    completionTarget = target;
                                    auxiliaryCount = true;
                                    count = std::max(count, target + 1);
                                }
                            }
                        }
                        D3D11_RENDER_TARGET_VIEW_DESC view{};
                        views[sourceTarget]->GetDesc(&view);
                        Com<ID3D11Resource> resource;
                        views[sourceTarget]->GetResource(&resource);
                        Com<ID3D11Texture2D> texture;
                        check(resource.As(&texture), "Get 2D pixel count source");
                        D3D11_TEXTURE2D_DESC desc{};
                        texture->GetDesc(&desc);
                        uint32_t mip = 0, firstLayer = 0, layers = 1;
                        switch (view.ViewDimension) {
                        case D3D11_RTV_DIMENSION_TEXTURE2D:
                            mip = view.Texture2D.MipSlice;
                            break;
                        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                            mip = view.Texture2DArray.MipSlice;
                            firstLayer = view.Texture2DArray.FirstArraySlice;
                            layers = view.Texture2DArray.ArraySize;
                            break;
                        case D3D11_RTV_DIMENSION_TEXTURE2DMS:
                            break;
                        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                            firstLayer = view.Texture2DMSArray.FirstArraySlice;
                            layers = view.Texture2DMSArray.ArraySize;
                            break;
                        default:
                            throw std::runtime_error("Unsupported pixel count RTV dimension");
                        }
                        desc.Width = std::max(1u, desc.Width >> mip);
                        desc.Height = std::max(1u, desc.Height >> mip);
                        if (countSelection->x >= desc.Width || countSelection->y >= desc.Height ||
                            countSelection->layer >= layers ||
                            countSelection->sample >= desc.SampleDesc.Count)
                            throw std::invalid_argument("Pixel count selection exceeds its RTV");
                        const uint64_t plane = uint64_t(desc.Width) * desc.Height * 16;
                        if (!layers || !desc.SampleDesc.Count ||
                            plane > (request.maxBytes - workingBytes) / 3 / layers / desc.SampleDesc.Count)
                            throw std::runtime_error("Pixel count transport payload budget exceeded");
                        workingBytes += plane * layers * desc.SampleDesc.Count * 3;
                        desc.MipLevels = 1;
                        desc.ArraySize = layers;
                        desc.Format = desc.SampleDesc.Count > 1 ? DXGI_FORMAT_R32G32B32A32_TYPELESS
                                                                : DXGI_FORMAT_R32G32B32A32_FLOAT;
                        desc.Usage = D3D11_USAGE_DEFAULT;
                        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        desc.CPUAccessFlags = desc.MiscFlags = 0;
                        UINT support = 0;
                        check(r.device_->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT, &support),
                              "Check pixel count float blending");
                        if (!(support & D3D11_FORMAT_SUPPORT_BLENDABLE))
                            throw std::runtime_error("Pixel count requires RGBA32F blending");
                        check(r.device_->CreateTexture2D(&desc, nullptr, &diagnosticTransport),
                              "Create pixel count transport");
                        D3D11_RENDER_TARGET_VIEW_DESC replacement{};
                        replacement.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                        if (desc.SampleDesc.Count > 1) {
                            replacement.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                            replacement.Texture2DMSArray = {0, layers};
                        } else {
                            replacement.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                            replacement.Texture2DArray = {0, 0, layers};
                        }
                        views[target].Reset();
                        check(r.device_->CreateRenderTargetView(diagnosticTransport.Get(), &replacement,
                                                                &views[target]),
                              "Create pixel count RTV");
                        raw[target] = views[target].Get();
                        context->OMSetRenderTargetsAndUnorderedAccessViews(count, raw.data(), depth.Get(), 0,
                                                                           D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                                           nullptr, nullptr);
                        transportResource.type = 0x85;
                        transportResource.desc = {desc.Width,
                                                  desc.Height,
                                                  1,
                                                  layers,
                                                  uint32_t(desc.Format),
                                                  desc.SampleDesc.Count,
                                                  desc.SampleDesc.Quality,
                                                  0,
                                                  desc.BindFlags,
                                                  0,
                                                  0};
                        report["count_transport"] = {{"slot", target},
                                                     {"source_slot", sourceTarget},
                                                     {"auxiliary_target", auxiliaryCount},
                                                     {"source_format", uint32_t(view.Format)},
                                                     {"source_mip", mip},
                                                     {"source_first_layer", firstLayer},
                                                     {"width", desc.Width},
                                                     {"height", desc.Height},
                                                     {"layers", layers},
                                                     {"samples", desc.SampleDesc.Count},
                                                     {"sample_quality", desc.SampleDesc.Quality},
                                                     {"view_format", uint32_t(replacement.Format)},
                                                     {"x", countSelection->x},
                                                     {"y", countSelection->y},
                                                     {"layer", countSelection->layer},
                                                     {"sample", countSelection->sample},
                                                     {"layer_domain", "bound_rtv_view"},
                                                     {"budgeted_bytes", workingBytes - cloneBytes}};
                    }
                    if (completionTarget) {
                        const auto target = *completionTarget;
                        if (!state.stages[4].shader && !depthSelection)
                            throw std::runtime_error("Private pixel completion requires a pixel shader");
                        std::vector<uint8_t> ownedCode;
                        if (!state.stages[4].shader)
                            ownedCode =
                                compileHlsl("void main(){}", "ps_5_0", "main", "native-null-depth-ps.hlsl")
                                    .bytecode;
                        Bytes completionCode =
                            state.stages[4].shader ? shader(state.stages[4].shader) : Bytes(ownedCode);
                        if (depthSelection) {
                            auto added = addPixelCompletionTarget(completionCode, target,
                                                                  request.maxBytes - workingBytes, scoped);
                            ownedCode = std::move(added.bytecode);
                            completionCode = ownedCode;
                            report["additional_target_shader"] = std::move(added.identity);
                            report["original_null_pixel_shader"] = !state.stages[4].shader;
                        }
                        std::optional<PixelPrimitiveInput> producer;
                        if (primitiveMask && state.stages[3].shader) {
                            uint32_t stream = 0;
                            if (const auto so = shaderStreamOutput(r.frame_, state.stages[3].shader))
                                stream = readStreamOutputDeclaration(r.frame_, so).rasterizedStream;
                            const auto layout = postTransformLayout(shader(state.stages[3].shader), stream);
                            for (const auto &attribute : layout.at("attributes")) {
                                if (attribute.at("system_value") != 7)
                                    continue;
                                if (producer || attribute.at("component_type") != 1 ||
                                    attribute.at("register_components").size() != 1 ||
                                    attribute.value("minimum_precision", 0u))
                                    throw std::runtime_error("Bound GS primitive identity is ambiguous");
                                producer = PixelPrimitiveInput{
                                    attribute.at("register").get<uint32_t>(),
                                    attribute.at("register_components")[0].get<uint32_t>()};
                            }
                            if (!producer)
                                throw std::runtime_error("Bound GS does not provide a primitive identity");
                        }
                        auto marked =
                            auxiliaryCount
                                ? instrumentAuxiliaryPixelReturnCount(completionCode, target,
                                                                      request.maxBytes - workingBytes, scoped)
                            : countReturns
                                ? instrumentPixelReturnCount(completionCode, target,
                                                             request.maxBytes - workingBytes, scoped)
                            : accumulatePredicate
                                ? instrumentPixelPrimitivePresence(completionCode, target, *primitiveMask,
                                                                   request.maxBytes - workingBytes, scoped,
                                                                   producer)
                            : primitiveMask
                                ? instrumentPixelPrimitiveMask(completionCode, target, *primitiveMask,
                                                               request.maxBytes - workingBytes, scoped,
                                                               producer)
                                : instrumentPixelCompletion(completionCode, target, marker,
                                                            request.maxBytes - workingBytes, scoped);
                        if (marked.bytecode.size() > request.maxBytes - workingBytes)
                            throw std::runtime_error("Private pixel completion shader budget exceeded");
                        workingBytes += marked.bytecode.size();
                        auto *context = r.context_.Get();
                        std::array<ID3D11RenderTargetView *, 8> rawViews{};
                        std::array<Com<ID3D11RenderTargetView>, 8> views;
                        context->OMGetRenderTargets(8, rawViews.data(), nullptr);
                        for (size_t i = 0; i < views.size(); ++i)
                            views[i].Attach(rawViews[i]);
                        if (!views[target])
                            throw std::runtime_error("Private pixel completion target is not bound");
                        auto blend = defaultBlend();
                        std::array<float, 4> factor{};
                        UINT mask = 0;
                        Com<ID3D11BlendState> currentBlend;
                        context->OMGetBlendState(&currentBlend, factor.data(), &mask);
                        if (currentBlend) {
                            Com<ID3D11BlendState1> extended;
                            if (SUCCEEDED(currentBlend.As(&extended)))
                                extended->GetDesc1(&blend);
                            else {
                                D3D11_BLEND_DESC base{};
                                currentBlend->GetDesc(&base);
                                blend = decodeBlend(
                                    Bytes(reinterpret_cast<const uint8_t *>(&base), sizeof base), false);
                            }
                        }
                        if (depthSelection && blend.AlphaToCoverageEnable)
                            throw std::runtime_error(
                                "Depth completion has no original alpha-to-coverage value");
                        if (countSelection && target == 0 && blend.AlphaToCoverageEnable &&
                            marked.identity.at("component_type") != 3 &&
                            !marked.identity.at("coverage_output_declared").get<bool>())
                            throw std::runtime_error(
                                "Integer RT0 alpha-to-coverage is not preserved by the float count target");
                        if (accumulatePredicate) {
                            Com<ID3D11DepthStencilState> currentDepth;
                            UINT reference = 0;
                            context->OMGetDepthStencilState(&currentDepth, &reference);
                            D3D11_DEPTH_STENCIL_DESC depth{};
                            depth.DepthEnable = TRUE;
                            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                            depth.DepthFunc = D3D11_COMPARISON_LESS;
                            depth.StencilReadMask = depth.StencilWriteMask = 255;
                            depth.FrontFace =
                                depth.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                                  D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
                            if (currentDepth)
                                currentDepth->GetDesc(&depth);
                            auto &test = report["pipeline_test_state"];
                            test = {{"mode", overrideName},
                                    {"scope", "complete_private_draw_counterfactual"},
                                    {"failure_reason", nullptr},
                                    {"depth_stencil_before", depthIdentity(depth)},
                                    {"stencil_reference", reference},
                                    {"sample_mask_before", mask},
                                    {"alpha_to_coverage_before", blend.AlphaToCoverageEnable != FALSE}};
                            Com<ID3D11DepthStencilView> depthView;
                            context->OMGetRenderTargets(0, nullptr, &depthView);
                            test["depth_view_bound"] = bool(depthView);
                            if (testOverride == NativePixelTestOverride::DisableDepth ||
                                testOverride == NativePixelTestOverride::DisableDepthStencil)
                                depth.DepthEnable = FALSE;
                            if (testOverride == NativePixelTestOverride::DisableStencil ||
                                testOverride == NativePixelTestOverride::DisableDepthStencil)
                                depth.StencilEnable = FALSE;
                            if (testOverride == NativePixelTestOverride::DisableDepth ||
                                testOverride == NativePixelTestOverride::DisableStencil ||
                                testOverride == NativePixelTestOverride::DisableDepthStencil) {
                                check(r.device_->CreateDepthStencilState(&depth, &testDepthState),
                                      "Create private counterfactual depth state");
                                context->OMSetDepthStencilState(testDepthState.Get(), reference);
                            }
                            if (testOverride == NativePixelTestOverride::DisableAlphaToCoverage)
                                blend.AlphaToCoverageEnable = FALSE;
                            if (testOverride == NativePixelTestOverride::AllSamples)
                                mask = UINT32_MAX;
                            test["depth_stencil_after"] = depthIdentity(depth);
                            test["sample_mask_after"] = mask;
                            test["alpha_to_coverage_after"] = blend.AlphaToCoverageEnable != FALSE;
                        }
                        if (!blend.IndependentBlendEnable) {
                            const auto shared = blend.RenderTarget[0];
                            for (size_t i = 0; i < views.size(); ++i)
                                blend.RenderTarget[i] = views[i] ? shared : defaultBlend().RenderTarget[i];
                        }
                        blend.IndependentBlendEnable = TRUE;
                        blend.RenderTarget[target] = defaultBlend().RenderTarget[target];
                        if (accumulatePredicate) {
                            D3D11_RENDER_TARGET_VIEW_DESC desc{};
                            views[target]->GetDesc(&desc);
                            UINT support = 0;
                            check(r.device_->CheckFormatSupport(desc.Format, &support),
                                  "Check private predicate blend format");
                            if (!(support & D3D11_FORMAT_SUPPORT_BLENDABLE))
                                throw std::runtime_error("Private pixel presence requires a blendable RTV");
                            auto &selected = blend.RenderTarget[target];
                            selected.BlendEnable = TRUE;
                            selected.SrcBlend = selected.DestBlend = D3D11_BLEND_ONE;
                            selected.BlendOp = D3D11_BLEND_OP_MAX;
                            selected.SrcBlendAlpha = D3D11_BLEND_ONE;
                            selected.DestBlendAlpha = D3D11_BLEND_ZERO;
                            selected.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                        }
                        if (countReturns) {
                            auto &selected = blend.RenderTarget[target];
                            selected.BlendEnable = TRUE;
                            selected.SrcBlend = selected.DestBlend = D3D11_BLEND_ONE;
                            selected.BlendOp = D3D11_BLEND_OP_ADD;
                            selected.SrcBlendAlpha = D3D11_BLEND_ONE;
                            selected.DestBlendAlpha = D3D11_BLEND_ZERO;
                            selected.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                        }
                        Com<ID3D11Device1> extendedDevice;
                        check(r.device_.As(&extendedDevice), "Get private completion device");
                        check(extendedDevice->CreateBlendState1(&blend, &completionBlend),
                              "Create private completion blend state");
                        auto linkage = state.stages[4].shader ? r.get<ID3D11ClassLinkage>(shaderClassLinkage(
                                                                    r.frame_, state.stages[4].shader))
                                                              : nullptr;
                        check(r.device_->CreatePixelShader(marked.bytecode.data(), marked.bytecode.size(),
                                                           linkage, &completionShader),
                              "Create private completion shader");
                        std::array<ID3D11ClassInstance *, 256> rawClasses{};
                        std::array<Com<ID3D11ClassInstance>, 256> classes;
                        UINT classCount = UINT(rawClasses.size());
                        Com<ID3D11PixelShader> originalShader;
                        context->PSGetShader(&originalShader, rawClasses.data(), &classCount);
                        for (UINT i = 0; i < classCount; ++i)
                            classes[i].Attach(rawClasses[i]);
                        context->PSSetShader(completionShader.Get(), rawClasses.data(), classCount);
                        context->OMSetBlendState(completionBlend.Get(), factor.data(), mask);
                        {
                            Unpredicated clear(context);
                            const float zero[4]{};
                            context->ClearRenderTargetView(views[target].Get(), zero);
                        }
                        D3D11_RENDER_TARGET_VIEW_DESC desc{};
                        views[target]->GetDesc(&desc);
                        report["kind"] = "native_pixel_completion_observation";
                        report["backend"] = "native-pixel-completion-observation/1";
                        report["scope"] = "original_ps_body_on_private_completion_target";
                        report["diagnostic_shader_replacements"] = true;
                        report["completion_shader"] = marked.identity;
                        if (ordinalSelection)
                            ordinalInput = std::move(marked.bytecode);
                        if (primitiveMask) {
                            report["kind"] = "native_pixel_primitive_mask_observation";
                            report["backend"] = "native-pixel-primitive-mask-observation/1";
                            if (primitiveMask->notEqual)
                                report["backend"] = "native-pixel-primitive-mask-observation/2";
                            report["scope"] = "original_ps_body_with_private_primitive_predicate";
                            report["primitive_id_source"] =
                                state.stages[3].shader ? "bound_geometry_shader" : "rasterizer";
                            report["primitive_ordinal"] = nullptr;
                            report["instance"] = nullptr;
                        }
                        report["completion_target"] = {
                            {"slot", target},
                            {"format", uint32_t(desc.Format)},
                            {"cleared_to_zero", true},
                            {"blend_and_logic_disabled", !accumulatePredicate && !countReturns},
                            {"write_mask", 15},
                            {"alpha_to_coverage", blend.AlphaToCoverageEnable != FALSE},
                            {"sample_mask", mask}};
                        if (countReturns) {
                            report["kind"] = "native_pixel_return_count_observation";
                            report["backend"] = countDepthReturns ? "native-depth-return-count/1"
                                                                  : "native-pixel-return-count/3";
                            if (countDepthReturns)
                                report["kind"] = "native_depth_return_count_observation";
                            report["scope"] = "returning_samples_on_private_additive_target";
                            report["completion_target"]["rgb_reduction"] = "add";
                            report["completion_target"]["alpha_reduction"] = "replace";
                            report["completion_target"]["logic_disabled"] = true;
                            report["diagnostic_working_payload_bytes"] = workingBytes;
                        }
                        if (accumulatePredicate) {
                            report["kind"] = "native_pixel_primitive_presence_observation";
                            report["backend"] = "native-pixel-primitive-presence/1";
                            if (primitiveMask->notEqual)
                                report["backend"] = "native-pixel-primitive-presence/2";
                            report["scope"] = "original_ps_body_with_private_primitive_presence";
                            if (testOverride != NativePixelTestOverride::Preserve)
                                report["scope"] = "original_ps_body_with_counterfactual_primitive_presence";
                            report["completion_target"]["rgb_reduction"] = "maximum";
                            report["completion_target"]["alpha_reduction"] = "replace";
                            report["completion_target"]["logic_disabled"] = true;
                        }
                    }
                    if (ordinalSelection) {
                        auto *context = r.context_.Get();
                        std::array<ID3D11RenderTargetView *, 8> raw{};
                        std::array<Com<ID3D11RenderTargetView>, 8> views;
                        Com<ID3D11DepthStencilView> originalDepth;
                        context->OMGetRenderTargets(8, raw.data(), &originalDepth);
                        UINT count = 0;
                        for (UINT i = 0; i < 8; ++i) {
                            views[i].Attach(raw[i]);
                            if (raw[i])
                                count = i + 1;
                        }
                        const auto target = ordinalSelection->target;
                        if (!views[target])
                            throw std::runtime_error("Private pixel ordinal target is not bound");
                        D3D11_RENDER_TARGET_VIEW_DESC view{};
                        views[target]->GetDesc(&view);
                        UINT mip = 0, layer = 0, layers = 1;
                        bool array = false, multisampled = false;
                        switch (view.ViewDimension) {
                        case D3D11_RTV_DIMENSION_TEXTURE2D:
                            mip = view.Texture2D.MipSlice;
                            break;
                        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                            mip = view.Texture2DArray.MipSlice;
                            layer = view.Texture2DArray.FirstArraySlice;
                            layers = view.Texture2DArray.ArraySize;
                            array = true;
                            break;
                        case D3D11_RTV_DIMENSION_TEXTURE2DMS:
                            multisampled = true;
                            break;
                        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                            layer = view.Texture2DMSArray.FirstArraySlice;
                            layers = view.Texture2DMSArray.ArraySize;
                            array = multisampled = true;
                            break;
                        default:
                            throw std::runtime_error("Private pixel ordinal requires a 2D target view");
                        }
                        Com<ID3D11Resource> resource;
                        Com<ID3D11Texture2D> color;
                        views[target]->GetResource(&resource);
                        check(resource.As(&color), "Get private ordinal target texture");
                        D3D11_TEXTURE2D_DESC desc{};
                        color->GetDesc(&desc);
                        if (mip >= desc.MipLevels || mip >= 32 || !layers || layer >= desc.ArraySize ||
                            layers > desc.ArraySize - layer)
                            throw std::runtime_error("Private pixel ordinal view bounds");
                        desc.Width = std::max(1u, desc.Width >> mip);
                        desc.Height = std::max(1u, desc.Height >> mip);
                        desc.MipLevels = 1;
                        desc.ArraySize = layers;
                        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                        desc.Usage = D3D11_USAGE_DEFAULT;
                        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                        desc.CPUAccessFlags = desc.MiscFlags = 0;
                        const uint64_t bytes =
                            uint64_t(desc.Width) * desc.Height * layers * desc.SampleDesc.Count * 4;
                        if (bytes > request.maxBytes - workingBytes)
                            throw std::runtime_error("Private pixel ordinal stencil payload budget exceeded");
                        UINT levels = 0;
                        check(r.device_->CheckMultisampleQualityLevels(desc.Format, desc.SampleDesc.Count,
                                                                       &levels),
                              "Check private ordinal stencil sampling");
                        if (!levels || (desc.SampleDesc.Quality >= levels &&
                                        desc.SampleDesc.Quality != D3D11_STANDARD_MULTISAMPLE_PATTERN &&
                                        desc.SampleDesc.Quality != D3D11_CENTER_MULTISAMPLE_PATTERN))
                            throw std::runtime_error(
                                "Private pixel ordinal stencil sample quality is unavailable");
                        Com<ID3D11Texture2D> stencil;
                        Com<ID3D11DepthStencilView> selector;
                        check(r.device_->CreateTexture2D(&desc, nullptr, &stencil),
                              "Create private ordinal stencil");
                        D3D11_DEPTH_STENCIL_VIEW_DESC dsView{};
                        dsView.Format = desc.Format;
                        if (array) {
                            dsView.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY
                                                                : D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                            if (multisampled)
                                dsView.Texture2DMSArray.ArraySize = layers;
                            else
                                dsView.Texture2DArray.ArraySize = layers;
                        } else {
                            dsView.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                                                : D3D11_DSV_DIMENSION_TEXTURE2D;
                        }
                        check(r.device_->CreateDepthStencilView(stencil.Get(), &dsView, &selector),
                              "Create private ordinal stencil view");
                        D3D11_DEPTH_STENCIL_DESC previous{};
                        previous.DepthEnable = TRUE;
                        previous.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                        previous.DepthFunc = D3D11_COMPARISON_LESS;
                        previous.StencilReadMask = previous.StencilWriteMask = 255;
                        previous.FrontFace =
                            previous.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                                 D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
                        Com<ID3D11DepthStencilState> oldState;
                        UINT oldReference = 0;
                        context->OMGetDepthStencilState(&oldState, &oldReference);
                        if (oldState)
                            oldState->GetDesc(&previous);
                        D3D11_DEPTH_STENCIL_DESC selected{};
                        selected.DepthFunc = D3D11_COMPARISON_ALWAYS;
                        selected.StencilEnable = TRUE;
                        selected.StencilReadMask = selected.StencilWriteMask = 255;
                        selected.FrontFace =
                            selected.BackFace = {D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_INCR_SAT,
                                                 D3D11_STENCIL_OP_INCR_SAT, D3D11_COMPARISON_EQUAL};
                        check(r.device_->CreateDepthStencilState(&selected, &testDepthState),
                              "Create private ordinal selector state");
                        bool early = false, coverage = false;
                        uint64_t inspectedShaderBytes = 0, normalizedShaderBytes = 0;
                        report["coverage_normalization"] = nullptr;
                        if (state.stages[4].shader) {
                            auto code = shader(state.stages[4].shader);
                            if (code.size() > request.maxBytes - workingBytes - bytes)
                                throw std::runtime_error(
                                    "Private pixel ordinal shader inspection budget exceeded");
                            inspectedShaderBytes = code.size();
                            for (const auto &[tag, part] : readDxbcParts(code)) {
                                if (tag != 0x52444853 && tag != 0x58454853)
                                    continue;
                                for (const auto &instruction : readDxbcProgram(part).instructions) {
                                    checkDeadline();
                                    if ((instruction[0] & 0x7ff) == 106)
                                        early |= (instruction[0] & 0x2000) != 0;
                                    if ((instruction[0] & 0x7ff) == 101 && instruction.size() >= 2)
                                        coverage |= ((instruction[1] >> 12) & 255) == 15;
                                }
                            }
                            if (coverage && desc.SampleDesc.Count == 1) {
                                auto normalized = normalizeSingleSamplePixelCoverage(
                                    ordinalInput.empty() ? code : Bytes(ordinalInput),
                                    request.maxBytes - workingBytes - bytes - inspectedShaderBytes, scoped);
                                normalizedShaderBytes = normalized.bytecode.size();
                                Com<ID3D11PixelShader> selectedShader;
                                auto linkage = r.get<ID3D11ClassLinkage>(
                                    shaderClassLinkage(r.frame_, state.stages[4].shader));
                                check(r.device_->CreatePixelShader(normalized.bytecode.data(),
                                                                   normalized.bytecode.size(), linkage,
                                                                   &selectedShader),
                                      "Create private ordinal coverage shader");
                                std::array<ID3D11ClassInstance *, 256> rawClasses{};
                                std::array<Com<ID3D11ClassInstance>, 256> classes;
                                UINT classCount = UINT(rawClasses.size());
                                Com<ID3D11PixelShader> previousShader;
                                context->PSGetShader(&previousShader, rawClasses.data(), &classCount);
                                for (UINT i = 0; i < classCount; ++i)
                                    classes[i].Attach(rawClasses[i]);
                                context->PSSetShader(selectedShader.Get(), rawClasses.data(), classCount);
                                report["coverage_normalization"] = std::move(normalized.identity);
                                report["diagnostic_shader_replacements"] = true;
                            }
                        }
                        {
                            Unpredicated clear(context);
                            context->ClearDepthStencilView(selector.Get(),
                                                           D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
                        }
                        context->OMSetRenderTargetsAndUnorderedAccessViews(
                            count, raw.data(), selector.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                            nullptr);
                        context->OMSetDepthStencilState(testDepthState.Get(), ordinalSelection->ordinal);
                        report["kind"] = "native_pixel_ordinal_observation";
                        report["backend"] = "native-pixel-ordinal/2";
                        report["scope"] = "original_ps_body_with_counterfactual_stencil_reference";
                        report["ordinal_selection"] = {{"target", target},
                                                       {"requested_ordinal", ordinalSelection->ordinal},
                                                       {"verified_fragment_ordinal", nullptr},
                                                       {"maximum_supported_ordinal", 254},
                                                       {"mark_returns", ordinalSelection->markReturns},
                                                       {"population", !state.stages[4].shader
                                                                          ? "no_pixel_shader_stencil_updates"
                                                                      : early ? "early_test_stencil_updates"
                                                                              : "late_test_survivor_updates"},
                                                       {"pixel_shader_bound", bool(state.stages[4].shader)},
                                                       {"force_early_depth_stencil", early},
                                                       {"stencil_initial_value", 0},
                                                       {"original_depth_view_bound", bool(originalDepth)},
                                                       {"original_stencil_reference", oldReference},
                                                       {"depth_stencil_before", depthIdentity(previous)},
                                                       {"depth_stencil_after", depthIdentity(selected)},
                                                       {"mip", mip},
                                                       {"first_layer", layer},
                                                       {"layer_count", layers},
                                                       {"width", desc.Width},
                                                       {"height", desc.Height},
                                                       {"samples", desc.SampleDesc.Count},
                                                       {"sample_quality", desc.SampleDesc.Quality},
                                                       {"stencil_format", uint32_t(desc.Format)},
                                                       {"stencil_payload_bytes", bytes},
                                                       {"application_uav_bindings_retained", true},
                                                       {"original_depth_stencil_eligibility", nullptr}};
                        report["diagnostic_working_payload_bytes"] =
                            workingBytes + bytes + inspectedShaderBytes + normalizedShaderBytes;
                    }
                    if (observe)
                        observe(r, false);
                    checkDeadline();
                    auto *context = r.context_.Get();
                    Com<ID3D11Query> occlusion, statistics;
                    const D3D11_QUERY_DESC oq{D3D11_QUERY_OCCLUSION, 0},
                        sq{D3D11_QUERY_PIPELINE_STATISTICS, 0};
                    check(r.device_->CreateQuery(&oq, &occlusion), "Create original draw occlusion query");
                    check(r.device_->CreateQuery(&sq, &statistics), "Create original draw statistics query");
                    context->Begin(occlusion.Get());
                    context->Begin(statistics.Get());
                    if (enabled) {
                        const auto &a = event.args;
                        switch (event.type) {
                        case 0x37:
                            context->Draw(a[0], a[1]);
                            break;
                        case 0x38:
                            context->Draw(parameters.direct.args[0], 0);
                            break;
                        case 0x39:
                            context->DrawIndexed(a[0], a[1], int32_t(a[2]));
                            break;
                        case 0x3a:
                            context->DrawIndexedInstanced(a[0], a[1], a[2], int32_t(a[3]), a[4]);
                            break;
                        case 0x3b:
                            context->DrawIndexedInstancedIndirect(r.get<ID3D11Buffer>(event.argumentBuffer),
                                                                  a[0]);
                            break;
                        case 0x3c:
                            context->DrawInstanced(a[0], a[1], a[2], a[3]);
                            break;
                        case 0x3d:
                            context->DrawInstancedIndirect(r.get<ID3D11Buffer>(event.argumentBuffer), a[0]);
                            break;
                        }
                    }
                    context->End(statistics.Get());
                    context->End(occlusion.Get());
                    dxbc_exec::GpuCompletion completion(r.device_.Get(), context, scoped, request.timeoutMs);
                    completion.submit();
                    uint64_t passing = 0;
                    D3D11_QUERY_DATA_PIPELINE_STATISTICS counts{};
                    completion.query(occlusion.Get(), &passing, sizeof passing);
                    completion.query(statistics.Get(), &counts, sizeof counts);
                    report["passing_samples"] = passing;
                    report["pipeline_statistics"] = {
                        {"ia_vertices", counts.IAVertices},       {"ia_primitives", counts.IAPrimitives},
                        {"vs_invocations", counts.VSInvocations}, {"hs_invocations", counts.HSInvocations},
                        {"ds_invocations", counts.DSInvocations}, {"gs_invocations", counts.GSInvocations},
                        {"gs_primitives", counts.GSPrimitives},   {"clip_invocations", counts.CInvocations},
                        {"clip_primitives", counts.CPrimitives},  {"ps_invocations", counts.PSInvocations},
                        {"cs_invocations", counts.CSInvocations}};
                    if (observe)
                        observe(r, true);
                    if (diagnosticTransport) {
                        const auto position = depthSelection
                                                  ? *depthSelection
                                                  : NativeDepthCompletionSelection{
                                                        countSelection->x, countSelection->y,
                                                        countSelection->layer, countSelection->sample};
                        // The transport is no longer an output after the draw.
                        // Unbind private color outputs before inspecting diagnostic storage.
                        Com<ID3D11DepthStencilView> depth;
                        context->OMGetRenderTargets(0, nullptr, &depth);
                        context->OMSetRenderTargetsAndUnorderedAccessViews(
                            0, nullptr, depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                        Unpredicated unpredicated(context);
                        Replay::PredicateIsolation isolation(r);
                        const auto storage =
                            transportResource.desc[5] == 1
                                ? r.readTextureStorage(diagnosticTransport.Get(), transportResource, scoped)
                                : readNativeMsaaStorage(
                                      r.device_.Get(), context, diagnosticTransport.Get(), transportResource,
                                      position.sample, uint32_t(DXGI_FORMAT_R32G32B32A32_FLOAT),
                                      [&](ID3D11Resource *resource, const Resource &layout) {
                                          return r.readTextureStorage(resource, layout, scoped);
                                      },
                                      scoped)
                                      .bytes;
                        const auto width = transportResource.desc[0], height = transportResource.desc[1];
                        const auto offset =
                            ((uint64_t(position.layer) * height + position.y) * width + position.x) * 16;
                        if (offset > storage.size() || storage.size() - offset < 16)
                            throw std::runtime_error("Diagnostic color transport readback bounds");
                        if (countReturns) {
                            std::array<uint32_t, 4> bits{};
                            std::memcpy(bits.data(), storage.data() + offset, 16);
                            report["return_count"] = decodeNativePixelReturnCount(bits);
                        } else {
                            std::array<uint32_t, 3> rgb{};
                            std::memcpy(rgb.data(), storage.data() + offset, 12);
                            report["depth_transport"]["rgb_bits"] = rgb;
                            report["depth_transport"]["marker_present"] = rgb == marker;
                            report["kind"] = "native_depth_completion_observation";
                            report["scope"] = "original_depth_draw_with_private_color_transport";
                            report["backend"] = "native-depth-completion/1";
                        }
                    }
                    checkDeadline();
                    validate();
                },
                true, scoped);
            checkDeadline();
            const auto adapter = r.adapterDescription();
            report["adapter"] = {{"vendor", adapter.VendorId},
                                 {"device", adapter.DeviceId},
                                 {"luid_low", adapter.AdapterLuid.LowPart},
                                 {"luid_high", adapter.AdapterLuid.HighPart},
                                 {"warp", r.options_.warp}};
            report["debug_validation"] = r.options_.debug ? "no_errors" : "not_requested";
        });
        return report;
    }
    Json depthCount(const NativeDepthCompletionSelection &selection,
                    const std::function<void(Replay &, bool)> &observe) {
        checkDeadline();
        if (!r.replayComplete_ || !r.options_.before || r.options_.until != id)
            throw std::runtime_error("Depth count requires a completed before-event replay");
        const auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d || !request.maxBytes || !request.timeoutMs ||
            request.timeoutMs > 60000)
            throw std::invalid_argument("Invalid depth count request");
        const auto state = r.prepareState(event);
        std::optional<uint32_t> colorSlot;
        Json depthView;
        r.inspectEventInputs(id, [&] {
            r.bind(state, false);
            r.applyGraphicsEdits(event, state);
            std::array<ID3D11RenderTargetView *, 8> raw{};
            std::array<Com<ID3D11RenderTargetView>, 8> views;
            Com<ID3D11DepthStencilView> depth;
            r.context_->OMGetRenderTargets(8, raw.data(), &depth);
            bool anyColor = false;
            for (uint32_t i = 0; i < 8; ++i) {
                views[i].Attach(raw[i]);
                anyColor |= bool(views[i]);
            }
            if (!anyColor)
                return;
            if (!depth || !state.stages[4].shader)
                throw std::runtime_error("Mixed depth counting requires a DSV and a pixel shader");
            const auto code = shader(state.stages[4].shader);
            if (code.size() > request.maxBytes)
                throw std::runtime_error("Mixed depth count shader inspection budget exceeded");
            std::set<uint32_t> outputs;
            for (const auto &[tag, bytes] : readDxbcParts(code))
                if (tag == 0x52444853 || tag == 0x58454853)
                    for (const auto &row : readDxbcProgram(bytes).instructions) {
                        checkDeadline();
                        if ((row[0] & 2047) == 101 && row.size() >= 3 && ((row[1] >> 12) & 255) == 2 &&
                            row.back() < 8)
                            outputs.insert(row.back());
                    }
            for (auto slot : outputs)
                if (views[slot]) {
                    colorSlot = slot;
                    break;
                }
            if (!colorSlot)
                throw std::runtime_error("Mixed depth counting requires a bound shader color output");
            auto extent = [&](ID3D11View *view, uint32_t mip, uint32_t first, uint32_t layers,
                              DXGI_FORMAT format) {
                Com<ID3D11Resource> resource;
                Com<ID3D11Texture2D> texture;
                view->GetResource(&resource);
                check(resource.As(&texture), "Get mixed count 2D view resource");
                D3D11_TEXTURE2D_DESC d{};
                texture->GetDesc(&d);
                if (mip >= d.MipLevels || mip >= 32 || !layers || first >= d.ArraySize ||
                    layers > d.ArraySize - first)
                    throw std::runtime_error("Mixed count view bounds");
                return Json{{"source_format", uint32_t(format)},
                            {"source_mip", mip},
                            {"source_first_layer", first},
                            {"width", std::max(1u, d.Width >> mip)},
                            {"height", std::max(1u, d.Height >> mip)},
                            {"layers", layers},
                            {"samples", d.SampleDesc.Count},
                            {"sample_quality", d.SampleDesc.Quality}};
            };
            D3D11_DEPTH_STENCIL_VIEW_DESC ds{};
            depth->GetDesc(&ds);
            uint32_t mip = 0, first = 0, layers = 1;
            switch (ds.ViewDimension) {
            case D3D11_DSV_DIMENSION_TEXTURE2D:
                mip = ds.Texture2D.MipSlice;
                break;
            case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
                mip = ds.Texture2DArray.MipSlice;
                first = ds.Texture2DArray.FirstArraySlice;
                layers = ds.Texture2DArray.ArraySize;
                break;
            case D3D11_DSV_DIMENSION_TEXTURE2DMS:
                break;
            case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
                first = ds.Texture2DMSArray.FirstArraySlice;
                layers = ds.Texture2DMSArray.ArraySize;
                break;
            default:
                throw std::runtime_error("Mixed depth count requires a 2D DSV");
            }
            depthView = extent(depth.Get(), mip, first, layers, ds.Format);
            D3D11_RENDER_TARGET_VIEW_DESC rt{};
            views[*colorSlot]->GetDesc(&rt);
            mip = first = 0;
            layers = 1;
            switch (rt.ViewDimension) {
            case D3D11_RTV_DIMENSION_TEXTURE2D:
                mip = rt.Texture2D.MipSlice;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                mip = rt.Texture2DArray.MipSlice;
                first = rt.Texture2DArray.FirstArraySlice;
                layers = rt.Texture2DArray.ArraySize;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMS:
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                first = rt.Texture2DMSArray.FirstArraySlice;
                layers = rt.Texture2DMSArray.ArraySize;
                break;
            default:
                throw std::runtime_error("Mixed depth count requires a 2D RTV");
            }
            const auto colorView = extent(views[*colorSlot].Get(), mip, first, layers, rt.Format);
            for (const auto *key : {"width", "height", "layers", "samples", "sample_quality"})
                if (colorView.at(key) != depthView.at(key))
                    throw std::runtime_error("Mixed count color and depth view extents differ");
            if (selection.x >= depthView.at("width").get<uint32_t>() ||
                selection.y >= depthView.at("height").get<uint32_t>() ||
                selection.layer >= depthView.at("layers").get<uint32_t>() ||
                selection.sample >= depthView.at("samples").get<uint32_t>())
                throw std::invalid_argument("Mixed count selection exceeds the DSV");
            depthView.update({{"x", selection.x},
                              {"y", selection.y},
                              {"layer", selection.layer},
                              {"sample", selection.sample},
                              {"layer_domain", "bound_dsv_view"},
                              {"route", "bound_color_output"},
                              {"source_color_slot", *colorSlot}});
        });
        if (!colorSlot)
            return original(observe, 0, {}, {0x3f800000, 0x3f800000, 0x3f800000}, false,
                            NativePixelTestOverride::Preserve, selection, {}, {}, true);
        auto result = original(observe, *colorSlot, {}, {0x3f800000, 0x3f800000, 0x3f800000}, false,
                               NativePixelTestOverride::Preserve, {}, {},
                               NativePixelReturnCountSelection{*colorSlot, selection.x, selection.y,
                                                               selection.layer, selection.sample});
        result["kind"] = "native_depth_return_count_observation";
        result["backend"] = "native-depth-return-count/2";
        result["depth_transport"] = std::move(depthView);
        result["original_null_pixel_shader"] = false;
        return result;
    }
    Json run() {
        checkDeadline();
        if (!r.replayComplete_ || !r.options_.before || r.options_.until != id)
            throw std::runtime_error("Native pixel geometry replay requires a completed before-event replay");
        const auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d || !request.maxBytes || !request.timeoutMs ||
            request.timeoutMs > 60000 || request.checkpoints.size() > 4096)
            throw std::invalid_argument("Invalid native pixel geometry replay request");
        const auto state = r.prepareState(event);
        Json report;
        r.inspectEventInputs(id, [&] {
            r.bind(state, false);
            r.applyGraphicsEdits(event, state);
            checkDeadline();
            uint32_t stream = 0;
            if (state.stages[3].shader)
                if (const auto so = shaderStreamOutput(r.frame_, state.stages[3].shader))
                    stream = readStreamOutputDeclaration(r.frame_, so).rasterizedStream;
            if (stream == UINT32_MAX)
                throw std::runtime_error("Selected draw has no rasterized stream");
            const auto cloneBytes = r.privateOutputPayloadBytes(state, true, cancelled);
            if (cloneBytes >= request.maxBytes)
                throw std::runtime_error("Native pixel private-output payload budget exceeded");
            const auto geometry =
                captureBoundPixelGeometry(r, event, state, stream, request.maxBytes - cloneBytes, [&] {
                    checkDeadline();
                    return false;
                });
            checkDeadline();
            const auto count = geometry.report.at("primitives").get<uint64_t>();
            const auto raster = dxbc_exec::pixelRasterState(r.context_.Get());
            if (planner) {
                request.firstPrimitive = 0;
                request.primitiveCount = count;
                request.checkpoints = (*planner)(geometry, raster);
                checkDeadline();
                if (request.checkpoints.size() > 4096)
                    throw std::invalid_argument("Native pixel checkpoint plan exceeds its limit");
            }
            if (request.firstPrimitive > count || request.primitiveCount > count - request.firstPrimitive ||
                (!request.primitiveCount && count))
                throw std::invalid_argument("Native pixel geometry range is outside captured primitives");
            auto checkpoints = request.checkpoints;
            if (!std::is_sorted(checkpoints.begin(), checkpoints.end()) ||
                std::adjacent_find(checkpoints.begin(), checkpoints.end()) != checkpoints.end() ||
                (!checkpoints.empty() &&
                 (!checkpoints.front() || checkpoints.back() > request.primitiveCount)))
                throw std::invalid_argument("Native pixel checkpoints must be increasing completed counts");
            if (request.primitiveCount &&
                (checkpoints.empty() || checkpoints.back() != request.primitiveCount))
                checkpoints.push_back(request.primitiveCount);
            const auto ps = state.stages[4].shader ? shader(state.stages[4].shader) : Bytes{};
            dxbc_exec::PixelGeometryBridge bridge;
            Com<ID3D11VertexShader> vs;
            Com<ID3D11GeometryShader> gs;
            Com<ID3D11InputLayout> layout;
            Com<ID3D11Buffer> vertices;
            const uint64_t sourceBytes =
                geometry.bytes.size() + geometry.primitiveIds.size() * sizeof(uint32_t);
            if (sourceBytes > request.maxBytes || cloneBytes > request.maxBytes - sourceBytes)
                throw std::runtime_error("Native pixel private-output payload budget exceeded");
            if (request.primitiveCount) {
                const auto available = request.maxBytes - sourceBytes - cloneBytes;
                bridge = dxbc_exec::pixelShaderGeometryRangeBridge(geometry, ps, request.firstPrimitive,
                                                                   request.primitiveCount, available / 2);
                if (bridge.vertices.size() > UINT32_MAX)
                    throw std::runtime_error("Native pixel bridge exceeds D3D11 buffer bounds");
                check(r.device_->CreateVertexShader(bridge.vertexShader.data(), bridge.vertexShader.size(),
                                                    nullptr, &vs),
                      "Create original-PS bridge VS");
                check(r.device_->CreateGeometryShader(bridge.geometryShader.data(),
                                                      bridge.geometryShader.size(), nullptr, &gs),
                      "Create original-PS bridge GS");
                std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
                for (UINT i = 0; i < bridge.stride / 16; ++i)
                    elements.push_back({"FLORA_GEOMETRY", i, DXGI_FORMAT_R32G32B32A32_UINT, 0, i * 16,
                                        D3D11_INPUT_PER_VERTEX_DATA, 0});
                check(r.device_->CreateInputLayout(elements.data(), UINT(elements.size()),
                                                   bridge.vertexShader.data(), bridge.vertexShader.size(),
                                                   &layout),
                      "Create original-PS bridge layout");
                const D3D11_BUFFER_DESC desc{
                    UINT(bridge.vertices.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
                const D3D11_SUBRESOURCE_DATA initial{bridge.vertices.data(), 0, 0};
                check(r.device_->CreateBuffer(&desc, &initial, &vertices),
                      "Create original-PS bridge vertices");
            }
            report = {{"schema", 1},
                      {"kind", "native_pixel_geometry_replay"},
                      {"backend", "native-pixel-geometry-replay/1"},
                      {"scope", "private_original_ps_geometry_range"},
                      {"event", id},
                      {"frame_sha256", r.frame_.sha256(cancelled)},
                      {"first_primitive", request.firstPrimitive},
                      {"primitive_count", request.primitiveCount},
                      {"geometry", geometry.report},
                      {"bridge", bridge.identity},
                      {"pixel_shader_resource", state.stages[4].shader},
                      {"pixel_shader_sha256", ps.empty() ? Json(nullptr) : Json(sha256(ps))},
                      {"raster_state", dxbc_exec::pixelRasterIdentity(raster)},
                      {"original_pixel_shader_preserved", true},
                      {"original_raster_and_om_state_preserved", true},
                      {"original_resources_written", false},
                      {"original_fragment_eligibility", nullptr},
                      {"original_gpu_schedule", false},
                      {"pre_raster_side_effects_replayed", false},
                      {"extra_uav_slots", 0},
                      {"private_output_payload_bytes", cloneBytes},
                      {"source_geometry_bytes", sourceBytes},
                      {"bridge_payload_bytes", bridge.vertices.size() * 2},
                      {"batches", Json::array()}};
            checkDeadline();
            const CancelCheck scoped = [&] {
                checkDeadline();
                return false;
            };
            r.withPrivateOutputs(
                event, state,
                [&] {
                    // No PS, rasterizer or output-merger replacement is performed.
                    // Every original writeable binding points at its private clone.
                    if (observer)
                        observer(r, 0);
                    checkDeadline();
                    if (request.primitiveCount) {
                        r.unbindStreamOutput();
                        auto *context = r.context_.Get();
                        context->HSSetShader(nullptr, nullptr, 0);
                        context->DSSetShader(nullptr, nullptr, 0);
                        context->VSSetShader(vs.Get(), nullptr, 0);
                        context->GSSetShader(gs.Get(), nullptr, 0);
                        context->IASetInputLayout(layout.Get());
                        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
                        const UINT offset = 0;
                        context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &bridge.stride, &offset);
                        context->IASetPrimitiveTopology(
                            bridge.verticesPerPrimitive == 1   ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                            : bridge.verticesPerPrimitive == 2 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                               : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        dxbc_exec::GpuCompletion completion(r.device_.Get(), context, scoped,
                                                            request.timeoutMs);
                        const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
                        Com<ID3D11Query> query;
                        check(r.device_->CreateQuery(&desc, &query), "Create private geometry replay query");
                        uint64_t completed = 0;
                        for (auto boundary : checkpoints) {
                            checkDeadline();
                            context->Begin(query.Get());
                            context->Draw(UINT((boundary - completed) * bridge.verticesPerPrimitive),
                                          UINT(completed * bridge.verticesPerPrimitive));
                            context->End(query.Get());
                            completion.submit();
                            uint64_t passing = 0;
                            completion.query(query.Get(), &passing, sizeof passing);
                            report["batches"].push_back(
                                {{"first_primitive", request.firstPrimitive + completed},
                                 {"primitive_count", boundary - completed},
                                 {"passing_samples", passing},
                                 {"count_domain", "entire_bound_output"}});
                            completed = boundary;
                            if (observer)
                                observer(r, completed);
                            checkDeadline();
                        }
                    }
                    validate();
                },
                true, scoped);
            checkDeadline();
            const auto adapter = r.adapterDescription();
            report["adapter"] = {{"vendor", adapter.VendorId},
                                 {"device", adapter.DeviceId},
                                 {"luid_low", adapter.AdapterLuid.LowPart},
                                 {"luid_high", adapter.AdapterLuid.HighPart},
                                 {"warp", r.options_.warp}};
            report["debug_validation"] = r.options_.debug ? "no_errors" : "not_requested";
        });
        return report;
    }
};
Json replayNativeOriginalDraw(Replay &r, Id event, const std::function<void(Replay &, bool)> &observe,
                              uint64_t maxBytes, uint32_t timeoutMs, const CancelCheck &cancelled) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .original(observe);
}
Json replayNativePixelOrdinal(Replay &r, Id event, const NativePixelOrdinalSelection &selection,
                              const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                              uint32_t timeoutMs, const CancelCheck &cancelled) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .original(observe, selection.markReturns ? std::optional<uint32_t>(selection.target) : std::nullopt,
                  {}, {0x3f800000, 0x40000000, 0x40400000}, false, NativePixelTestOverride::Preserve, {},
                  selection);
}
Json replayNativePixelCompletion(Replay &r, Id event, uint32_t target,
                                 const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                                 uint32_t timeoutMs, const CancelCheck &cancelled,
                                 std::array<uint32_t, 3> marker) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .original(observe, target, {}, marker);
}
Json replayNativeDepthCompletion(Replay &r, Id event, const NativeDepthCompletionSelection &selection,
                                 const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                                 uint32_t timeoutMs, const CancelCheck &cancelled) {
    const std::function<void(Replay &, uint64_t)> unused;
    NativePixelGeometryReplay replay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled);
    // Adding a color output can change early-test occlusion accounting on the
    // selected driver. Measure the unmodified draw separately on fresh clones.
    const auto reference = replay.original({});
    auto result = replay.original(observe, 0, {}, {0x3f800000, 0x40000000, 0x40400000}, false,
                                  NativePixelTestOverride::Preserve, selection);
    result["diagnostic_passing_samples"] = result.at("passing_samples");
    result["diagnostic_pipeline_statistics"] = result.at("pipeline_statistics");
    result["passing_samples"] = reference.at("passing_samples");
    result["pipeline_statistics"] = reference.at("pipeline_statistics");
    result["count_domain"] = "unmodified_draw_on_fresh_private_outputs";
    result["diagnostic_count_domain"] = "draw_with_added_color_transport";
    result["diagnostic_counts_match"] =
        result.at("diagnostic_passing_samples") == result.at("passing_samples");
    result["reference_draw"] = reference;
    result["replay_passes"] = 2;
    return result;
}
Json replayNativeDepthReturnCount(Replay &r, Id event, const NativeDepthCompletionSelection &selection,
                                  const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                                  uint32_t timeoutMs, const CancelCheck &cancelled) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .depthCount(selection, observe);
}
Json replayNativePixelReturnCount(Replay &r, Id event, const NativePixelReturnCountSelection &selection,
                                  const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                                  uint32_t timeoutMs, const CancelCheck &cancelled) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .original(observe, selection.target, {}, {0x3f800000, 0x3f800000, 0x3f800000}, false,
                  NativePixelTestOverride::Preserve, {}, {}, selection);
}
Json replayNativePixelPrimitiveMask(Replay &r, Id event, uint32_t target, PixelPrimitiveMask selection,
                                    const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                                    uint32_t timeoutMs, const CancelCheck &cancelled) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .original(observe, target, selection);
}
Json replayNativePixelPrimitivePresence(Replay &r, Id event, uint32_t target, PixelPrimitiveMask selection,
                                        const std::function<void(Replay &, bool)> &observe, uint64_t maxBytes,
                                        uint32_t timeoutMs, const CancelCheck &cancelled,
                                        NativePixelTestOverride testOverride) {
    const std::function<void(Replay &, uint64_t)> unused;
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, unused, cancelled)
        .original(observe, target, selection, {0x3f800000, 0x40000000, 0x40400000}, true, testOverride);
}
Json replayNativePixelGeometry(Replay &r, Id event, const NativePixelGeometryRange &range,
                               const std::function<void(Replay &, uint64_t)> &observe,
                               const CancelCheck &cancelled) {
    return NativePixelGeometryReplay(r, event, range, observe, cancelled).run();
}
Json replayNativePixelGeometryCheckpoints(Replay &r, Id event, const NativePixelCheckpointPlanner &planner,
                                          const std::function<void(Replay &, uint64_t)> &observe,
                                          uint64_t maxBytes, uint32_t timeoutMs,
                                          const CancelCheck &cancelled) {
    if (!planner)
        throw std::invalid_argument("Native pixel checkpoint planner is missing");
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, observe, cancelled, &planner)
        .run();
}
} // namespace flora
