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
#include <chrono>
#include <d3d11sdklayers.h>
namespace flora {
using Json = nlohmann::json;
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
                  std::optional<NativeDepthCompletionSelection> depthSelection = {}) {
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
                    Com<ID3D11BlendState1> completionBlend;
                    Com<ID3D11DepthStencilState> testDepthState;
                    Com<ID3D11Texture2D> depthTransport;
                    Resource transportResource;
                    uint64_t workingBytes = cloneBytes;
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
                        uint32_t mip = 0, layers = 1;
                        switch (view.ViewDimension) {
                        case D3D11_DSV_DIMENSION_TEXTURE2D:
                            mip = view.Texture2D.MipSlice;
                            break;
                        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
                            mip = view.Texture2DArray.MipSlice;
                            layers = view.Texture2DArray.ArraySize;
                            break;
                        case D3D11_DSV_DIMENSION_TEXTURE2DMS:
                            break;
                        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
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
                        check(r.device_->CreateTexture2D(&desc, nullptr, &depthTransport),
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
                        check(r.device_->CreateRenderTargetView(depthTransport.Get(), &targetView, &target),
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
                        report["depth_transport"] = {{"width", desc.Width},
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
                        const auto marked =
                            accumulatePredicate
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
                        if (primitiveMask) {
                            report["kind"] = "native_pixel_primitive_mask_observation";
                            report["backend"] = "native-pixel-primitive-mask-observation/1";
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
                            {"blend_and_logic_disabled", !accumulatePredicate},
                            {"write_mask", 15},
                            {"alpha_to_coverage", blend.AlphaToCoverageEnable != FALSE},
                            {"sample_mask", mask}};
                        if (accumulatePredicate) {
                            report["kind"] = "native_pixel_primitive_presence_observation";
                            report["backend"] = "native-pixel-primitive-presence/1";
                            report["scope"] = "original_ps_body_with_private_primitive_presence";
                            if (testOverride != NativePixelTestOverride::Preserve)
                                report["scope"] = "original_ps_body_with_counterfactual_primitive_presence";
                            report["completion_target"]["rgb_reduction"] = "maximum";
                            report["completion_target"]["alpha_reduction"] = "replace";
                            report["completion_target"]["logic_disabled"] = true;
                        }
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
                    if (depthTransport) {
                        // The transport is no longer an output after the draw.
                        // Restore the depth-only OM layout before inspection.
                        Com<ID3D11DepthStencilView> depth;
                        context->OMGetRenderTargets(0, nullptr, &depth);
                        context->OMSetRenderTargetsAndUnorderedAccessViews(
                            0, nullptr, depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                        Unpredicated unpredicated(context);
                        Replay::PredicateIsolation isolation(r);
                        const auto storage =
                            transportResource.desc[5] == 1
                                ? r.readTextureStorage(depthTransport.Get(), transportResource, scoped)
                                : readNativeMsaaStorage(
                                      r.device_.Get(), context, depthTransport.Get(), transportResource,
                                      depthSelection->sample, uint32_t(DXGI_FORMAT_R32G32B32A32_FLOAT),
                                      [&](ID3D11Resource *resource, const Resource &layout) {
                                          return r.readTextureStorage(resource, layout, scoped);
                                      },
                                      scoped)
                                      .bytes;
                        const auto width = transportResource.desc[0], height = transportResource.desc[1];
                        const auto offset =
                            ((uint64_t(depthSelection->layer) * height + depthSelection->y) * width +
                             depthSelection->x) *
                            16;
                        if (offset > storage.size() || storage.size() - offset < 16)
                            throw std::runtime_error("Depth completion readback bounds");
                        std::array<uint32_t, 3> rgb{};
                        std::memcpy(rgb.data(), storage.data() + offset, 12);
                        report["depth_transport"]["rgb_bits"] = rgb;
                        report["depth_transport"]["marker_present"] = rgb == marker;
                        report["kind"] = "native_depth_completion_observation";
                        report["scope"] = "original_depth_draw_with_private_color_transport";
                        report["backend"] = "native-depth-completion/1";
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
                captureBoundPixelGeometry(r, event, state, stream, request.maxBytes - cloneBytes);
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
