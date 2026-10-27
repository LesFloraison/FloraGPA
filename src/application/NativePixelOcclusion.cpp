#include "NativePixelOcclusion.h"
#include "DxbcGpuCompletion.h"
#include "DxbcPixelRaster.h"
#include <algorithm>
#include <d3d11sdklayers.h>

namespace flora {
class NativePixelOcclusionCapture {
    Replay replay;
    NativePixelOcclusionSelection selection;
    CancelCheck cancelled;
    nlohmann::json result;
    bool began = false, ended = false;
    Com<ID3D11Query> query;
    Com<ID3D11RasterizerState> originalRaster, diagnosticRaster;
    Com<ID3D11BlendState> originalBlend;
    std::array<float, 4> factors{};
    UINT originalMask = 0;
    std::vector<D3D11_RECT> originalScissors;
    std::unique_ptr<dxbc_exec::GpuCompletion> completion;
    void before(ID3D11DeviceContext *context, const std::map<Id, Com<IUnknown>> &objects) {
        checkCancellation(cancelled);
        if (began)
            throw std::runtime_error("Duplicate native pixel occlusion boundary");
        const auto state = dxbc_exec::pixelRasterState(context, selection.colorSlot);
        if (state.target.layers != 1)
            throw std::runtime_error("Pixel occlusion requires a single-layer bound target view");
        if (selection.x >= state.target.width || selection.y >= state.target.height)
            throw std::runtime_error("Pixel occlusion coordinate exceeds the bound target");
        // SampleMask selects output storage samples, even with forced raster samples.
        const auto samples = state.target.samples.Count;
        if (!samples || samples > 32 || selection.sample >= samples)
            throw std::runtime_error("Pixel occlusion sample exceeds target sample count");
        std::array<ID3D11RenderTargetView *, 8> raw{};
        context->OMGetRenderTargets(8, raw.data(), nullptr);
        std::array<Com<ID3D11RenderTargetView>, 8> targets;
        for (unsigned i = 0; i < 8; ++i)
            targets[i].Attach(raw[i]);
        auto target = targets[selection.colorSlot];
        Id viewId = 0;
        for (const auto &[id, object] : objects) {
            Com<ID3D11RenderTargetView> view;
            if (SUCCEEDED(object.As(&view)) && view == target) {
                viewId = id;
                break;
            }
        }
        if (!viewId)
            throw std::runtime_error("Cannot identify the native pixel target view");
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
        target->GetDesc(&viewDesc);
        std::array<uint32_t, sizeof(viewDesc) / 4> viewWords{};
        std::memcpy(viewWords.data(), &viewDesc, sizeof(viewDesc));
        context->RSGetState(&originalRaster);
        context->OMGetBlendState(&originalBlend, factors.data(), &originalMask);
        originalScissors = state.scissors;
        auto raster = state.rasterizer;
        raster.ScissorEnable = TRUE;
        diagnosticRaster = replay.createRasterizer(raster);
        const D3D11_RECT selected{LONG(selection.x), LONG(selection.y), LONG(selection.x + 1),
                                  LONG(selection.y + 1)};
        std::vector<D3D11_RECT> scissors(state.viewports.size(), selected);
        for (size_t i = 0; state.rasterizer.ScissorEnable && i < scissors.size(); ++i) {
            auto &rect = scissors[i];
            if (i >= state.scissors.size())
                rect = {0, 0, 0, 0};
            else {
                const auto &original = state.scissors[i];
                rect = {std::max(rect.left, original.left), std::max(rect.top, original.top),
                        std::min(rect.right, original.right), std::min(rect.bottom, original.bottom)};
                rect.right = std::max(rect.left, rect.right);
                rect.bottom = std::max(rect.top, rect.bottom);
            }
        }
        // A single-sample target already selects the only output sample. Keep
        // all original bits, including those affecting forced-sample queries.
        const auto mask = samples == 1 ? originalMask : originalMask & (1u << selection.sample);
        context->RSSetState(diagnosticRaster.Get());
        context->RSSetScissorRects(UINT(scissors.size()), scissors.data());
        context->OMSetBlendState(originalBlend.Get(), factors.data(), mask);
        const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
        check(replay.nativeDevice()->CreateQuery(&desc, &query), "Create native pixel occlusion query");
        completion = std::make_unique<dxbc_exec::GpuCompletion>(replay.nativeDevice(), context, cancelled,
                                                                selection.timeoutMs);
        result["raster_state"] = dxbc_exec::pixelRasterIdentity(state);
        result["count_domain"] =
            state.rasterizer.ForcedSampleCount ? "forced_raster_samples" : "output_samples";
        result["forced_sample_mask_implementation_freedom"] = state.rasterizer.ForcedSampleCount != 0;
        result["view"] = viewId;
        result["view_descriptor"] = viewWords;
        result["original_sample_mask"] = originalMask;
        result["effective_sample_mask"] = mask;
        result["effective_scissors"] = nlohmann::json::array();
        for (const auto &rect : scissors)
            result["effective_scissors"].push_back({rect.left, rect.top, rect.right, rect.bottom});
        context->Begin(query.Get());
        began = true;
    }
    void after(ID3D11DeviceContext *context) {
        if (!began || ended)
            throw std::runtime_error("Unpaired native pixel occlusion boundary");
        context->End(query.Get());
        ended = true;
        context->RSSetState(originalRaster.Get());
        context->RSSetScissorRects(UINT(originalScissors.size()), originalScissors.data());
        context->OMSetBlendState(originalBlend.Get(), factors.data(), originalMask);
        completion->submit();
        uint64_t count = 0;
        completion->query(query.Get(), &count, sizeof(count));
        result["passing_samples"] = count;
        result["completion_wait"] = completion->usesFence() ? "fence-event/1" : "bounded-query-poll/1";
    }

  public:
    NativePixelOcclusionCapture(const Frame &frame, ReplayOptions options,
                                const NativePixelOcclusionSelection &s, const CancelCheck &c)
        : replay(frame, std::move(options)), selection(s), cancelled(c) {}
    nlohmann::json run() {
        const auto &options = replay.options();
        result = {{"schema", 1},
                  {"count_semantics", "d3d11_occlusion_query"},
                  {"sample_selection_domain", "output_storage"},
                  {"kind", "native_pixel_occlusion"},
                  {"backend", "native-pixel-occlusion/1"},
                  {"scope", "selected_region_replay"},
                  {"isolation", "independent_replay_instance"},
                  {"event", selection.event},
                  {"color_slot", selection.colorSlot},
                  {"x", selection.x},
                  {"y", selection.y},
                  {"sample", selection.sample},
                  {"enabled", !options.suppressDraws && !options.disabled.contains(selection.event)},
                  {"original_fragment_eligibility", nullptr},
                  {"fragment_records", nullptr},
                  {"replay_count", 1},
                  {"original_shaders_preserved", true},
                  {"original_depth_stencil_preserved", true},
                  {"additional_uav_slots", 0}};
        replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); },
                   [&](Id event, bool afterDraw, auto *context, const auto &objects) {
                       if (event == selection.event) {
                           if (afterDraw)
                               after(context);
                           else
                               before(context, objects);
                       }
                   });
        if (!ended)
            throw std::runtime_error("Native pixel occlusion draw was not reached");
        checkCancellation(cancelled);
        if (options.debug) {
            Com<ID3D11InfoQueue> queue;
            check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Query occlusion validation");
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                check(queue->GetMessage(i, nullptr, &size), "Size occlusion validation");
                std::vector<uint8_t> storage(size);
                auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
                check(queue->GetMessage(i, message, &size), "Read occlusion validation");
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
        }
        result["debug_validation"] = options.debug ? "no_errors" : "not_requested";
        const auto adapter = replay.adapterDescription();
        result["adapter"] = {{"vendor", adapter.VendorId},
                             {"device", adapter.DeviceId},
                             {"luid_low", adapter.AdapterLuid.LowPart},
                             {"luid_high", adapter.AdapterLuid.HighPart},
                             {"warp", options.warp}};
        result["frame_sha256"] = replay.frame().sha256(cancelled);
        return result;
    }
};
nlohmann::json measureNativePixelOcclusion(const Frame &frame, ReplayOptions options,
                                           const NativePixelOcclusionSelection &selection,
                                           const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const auto &entry = frame.entry(selection.event);
    const auto type = entry.type;
    if (entry.category != 7 || type < 0x37 || type > 0x3d || selection.colorSlot >= 8 ||
        selection.x >= INT32_MAX || selection.y >= INT32_MAX || !selection.timeoutMs ||
        selection.timeoutMs > 60000)
        throw std::invalid_argument("Invalid native pixel occlusion selection");
    if (!options.renderdocLibrary.empty() || !options.renderdocOutput.empty())
        throw std::invalid_argument("Native pixel occlusion cannot create an external recapture");
    options.until = selection.event;
    options.before = false;
    options.timings = false;
    options.measurement.reset();
    return NativePixelOcclusionCapture(frame, std::move(options), selection, cancelled).run();
}
} // namespace flora
