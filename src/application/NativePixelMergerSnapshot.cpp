#include "NativePixelMergerSnapshot.h"
#include "FrameOutput.h"
#include "NativePixel.h"
#include "replay/BlendState.h"
#include <chrono>

namespace flora {
NativePixelMergerSnapshot snapshotNativePixelMergerState(Replay &replay,
                                                         const NativePixelMergerSelection &selection,
                                                         const CancelCheck &cancelled) {
    if ((selection.colorSlot && *selection.colorSlot >= 8) || !selection.maxReadbackBytes ||
        !selection.timeoutMs || selection.timeoutMs > 60000)
        throw std::invalid_argument("Invalid pixel merger snapshot selection or limits");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(selection.timeoutMs);
    const CancelCheck bounded = [&] {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Pixel merger snapshot deadline");
        return false;
    };
    checkCancellation(bounded);
    NativePixelMergerSnapshot result;
    auto &state = result.state;
    state.x = selection.x;
    state.y = selection.y;
    state.colorSlot = selection.colorSlot.value_or(0);
    state.timeoutMs = selection.timeoutMs;
    result.identity = {{"schema", 1},
                       {"kind", "native_pixel_merger_snapshot"},
                       {"backend", "native-pixel-merger-snapshot/1"},
                       {"resource_observation", "current_native_bindings"},
                       {"raster_inputs_supplied", false},
                       {"content_defined", nullptr},
                       {"original_fragment_eligibility", nullptr},
                       {"targets", nlohmann::json::array()}};
    struct Target {
        Id resource = 0, view = 0;
        Resource metadata;
        nlohmann::json selection;
        bool depth = false;
    };
    std::vector<Target> targets;
    replay.inspectNativeState([&](auto *context, const auto &objects) {
        Com<ID3D11BlendState> blend;
        context->OMGetBlendState(&blend, state.blendFactor.data(), &state.sampleMask);
        if (blend) {
            Com<ID3D11BlendState1> extended;
            if (SUCCEEDED(blend.As(&extended)))
                extended->GetDesc1(&state.blend);
            else {
                D3D11_BLEND_DESC desc{};
                blend->GetDesc(&desc);
                state.blend =
                    decodeBlend(Bytes(reinterpret_cast<const uint8_t *>(&desc), sizeof desc), false);
            }
        }
        Com<ID3D11DepthStencilState> depthState;
        context->OMGetDepthStencilState(&depthState, &state.stencilRef);
        if (depthState)
            depthState->GetDesc(&state.depthStencil);
        std::array<ID3D11RenderTargetView *, 8> raw{};
        std::array<Com<ID3D11RenderTargetView>, 8> views;
        Com<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(8, raw.data(), &depth);
        for (size_t i = 0; i < raw.size(); ++i)
            views[i].Attach(raw[i]);
        auto add = [&](ID3D11View *view, Bytes descriptor, bool isDepth) {
            if (!view)
                return;
            Target target;
            target.depth = isDepth;
            Com<ID3D11Resource> resource;
            view->GetResource(&resource);
            for (const auto &[id, object] : objects) {
                Com<ID3D11Resource> candidate;
                if (SUCCEEDED(object.As(&candidate)) && candidate == resource)
                    target.resource = id;
                Com<ID3D11View> candidateView;
                if (SUCCEEDED(object.As(&candidateView)) && candidateView.Get() == view)
                    target.view = id;
            }
            if (!target.resource || !target.view)
                throw std::runtime_error("Pixel merger target has no GPA resource/view identity");
            target.metadata = replay.observeTextureMetadata(target.resource).resource;
            const auto info = textureInfo(target.metadata);
            const auto range = outputSubresource(target.metadata, descriptor, {}, isDepth);
            if (selection.layer >= range.at("layer_count").get<uint32_t>())
                throw std::invalid_argument("Pixel merger layer exceeds bound view");
            const auto layer = range.at("first_layer").get<uint32_t>() + selection.layer;
            target.selection = outputSubresource(target.metadata, descriptor, layer, isDepth);
            const auto format = range.at("format").get<uint32_t>();
            const auto width = range.at("width").get<uint32_t>(), height = range.at("height").get<uint32_t>();
            if (!nativePixelFormatsCompatible(info.format, format) || selection.x >= width ||
                selection.y >= height)
                throw std::invalid_argument("Pixel merger texel or view format exceeds target");
            const DXGI_SAMPLE_DESC samples{info.samples,
                                           target.metadata.type == 0x85 ? target.metadata.desc.at(6) : 0};
            if (!targets.empty() &&
                (state.width != width || state.height != height || state.samples.Count != samples.Count ||
                 state.samples.Quality != samples.Quality))
                throw std::runtime_error("Pixel merger target extents or samples differ");
            state.width = width;
            state.height = height;
            state.samples = samples;
            if (isDepth) {
                state.depthFormat = format;
                state.depthViewFlags = range.at("dsv_flags").get<uint32_t>();
            } else
                state.colorFormat = format;
            targets.push_back(std::move(target));
        };
        if (selection.colorSlot) {
            const auto &view = views[*selection.colorSlot];
            if (!view)
                throw std::invalid_argument("Selected pixel merger color slot is unbound");
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            view->GetDesc(&desc);
            add(view.Get(), Bytes(reinterpret_cast<const uint8_t *>(&desc), sizeof desc), false);
        }
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            depth->GetDesc(&desc);
            add(depth.Get(), Bytes(reinterpret_cast<const uint8_t *>(&desc), sizeof desc), true);
        }
    });
    if (targets.empty())
        throw std::invalid_argument("Pixel merger snapshot has no bound target");
    uint64_t readBytes = 0;
    uint64_t plannedReadback = 0;
    for (const auto &target : targets) {
        const auto layout = textureSubresources(target.metadata);
        const auto bytes = layout.back().offset + layout.back().size;
        if (bytes > (selection.maxReadbackBytes - plannedReadback) / state.samples.Count)
            throw std::runtime_error("Pixel merger snapshot readback budget exceeded");
        plannedReadback += bytes * state.samples.Count;
    }
    for (const auto &target : targets) {
        const auto format = target.selection.at("format").get<uint32_t>();
        const auto [texel, rows] = pitches(1, 1, format);
        if (!texel || rows != 1)
            throw std::runtime_error("Pixel merger snapshot requires ordinary texel storage");
        auto &retained = target.depth ? state.depth : state.color;
        bool exactBits = true;
        for (uint32_t sample = 0; sample < state.samples.Count; ++sample) {
            checkCancellation(bounded);
            if (readBytes >= selection.maxReadbackBytes)
                throw std::runtime_error("Pixel merger snapshot readback budget exceeded");
            const auto budget = selection.maxReadbackBytes - readBytes;
            Resource layout;
            std::vector<uint8_t> bytes;
            if (state.samples.Count > 1) {
                auto storage =
                    replay.observeMsaaStorage(target.resource, sample, format, selection.maxReadbackBytes,
                                              bounded, selection.timeoutMs);
                exactBits &= storage.integerBits || storage.depthStencil;
                if (storage.mode != "selected_sample")
                    throw std::runtime_error("Pixel merger snapshot attempted a resolve");
                layout = std::move(storage.resource);
                bytes = std::move(storage.bytes);
            } else {
                auto storage = replay.observeTextureStorage(target.resource, budget, bounded);
                layout = std::move(storage.resource);
                bytes = std::move(storage.bytes);
            }
            readBytes += bytes.size();
            if (readBytes > plannedReadback)
                throw std::runtime_error("Pixel merger snapshot readback layout changed");
            const auto info = textureInfo(layout);
            const auto sub = textureSubresources(layout).at(
                target.selection.at("mip").get<uint32_t>() +
                target.selection.at("array_layer").get<uint32_t>() * info.mips);
            const auto offset = sub.offset + target.selection.at("slice").get<uint32_t>() * sub.slicePitch +
                                uint64_t(selection.y) * sub.rowPitch + uint64_t(selection.x) * texel;
            if (offset > bytes.size() || texel > bytes.size() - offset)
                throw std::runtime_error("Pixel merger snapshot storage bounds");
            const auto begin = retained.size();
            retained.insert(retained.end(), bytes.begin() + size_t(offset),
                            bytes.begin() + size_t(offset + texel));
            if (format == 20)
                std::fill(retained.begin() + begin + 5, retained.end(), 0);
        }
        result.identity["targets"].push_back(
            {{"resource", target.resource},
             {"view", target.view},
             {"depth", target.depth},
             {"subresource", target.selection},
             {"exact_storage_bits", exactBits},
             {"transfer", state.samples.Count > 1 ? "selected_sample" : "native_storage_copy"},
             {"invalid_byte_offsets",
              format == 20 ? nlohmann::json::array({5, 6, 7}) : nlohmann::json::array()}});
    }
    checkCancellation(bounded);
    result.identity["readback_bytes"] = readBytes;
    result.identity["readback_limit"] = selection.maxReadbackBytes;
    result.identity["storage_layout"] = "sample_major_native_texels";
    return result;
}
} // namespace flora
