#include "NativePixelDrawHistory.h"
#include "FrameOutput.h"
#include "NativePixel.h"
#include "NativePixelCandidates.h"
#include "NativePixelGeometryReplay.h"
#include "NativePixelIdentity.h"
#include "NativePixelInvocation.h"
#include "NativePixelMergerSnapshot.h"
#include "NativePixelOutputs.h"
#include <algorithm>
#include <chrono>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
struct Target {
    Resource resource;
    Json view;
    Id viewId = 0;
    dxbc_exec::PixelRasterState raster;
};
Target selectedTarget(Replay &r, const NativeHistorySelection &s, const dxbc_exec::PixelRasterState &raster) {
    Target result;
    result.resource = r.observeTextureMetadata(s.resource).resource;
    const auto info = textureInfo(result.resource);
    if (!s.maxReadbackBytes || s.mip >= info.mips || s.mip >= 32 || s.layer >= info.layers ||
        s.slice >= std::max(1u, info.depth >> s.mip) || s.x >= std::max(1u, info.width >> s.mip) ||
        s.y >= std::max(1u, info.height >> s.mip) || s.sample >= info.samples)
        throw std::invalid_argument("Private pixel history selection exceeds resource bounds");
    const auto storage = textureSubresources(result.resource);
    const auto bytes = storage.back().offset + storage.back().size;
    if (bytes > s.maxReadbackBytes / info.samples)
        throw std::runtime_error("Private pixel history readback payload budget exceeded");
    result.raster = raster;
    r.inspectNativeState([&](auto *context, const auto &objects) {
        Com<ID3D11Resource> selected;
        check(objects.at(s.resource).As(&selected), "Get selected pixel history resource");
        auto consider = [&](ID3D11View *view, Bytes descriptor, bool depth, uint32_t slot) {
            if (!view)
                return;
            Com<ID3D11Resource> owner;
            view->GetResource(&owner);
            if (owner != selected)
                return;
            const auto sub = outputSubresource(result.resource, descriptor, {}, depth);
            const auto layer = info.dimension == 4 ? s.slice : s.layer;
            if (sub.at("mip") != s.mip || layer < sub.at("first_layer").get<uint32_t>() ||
                uint64_t(layer) - sub.at("first_layer").get<uint32_t>() >=
                    sub.at("layer_count").get<uint32_t>())
                return;
            if (!result.view.is_null())
                throw std::runtime_error("Ambiguous private pixel history output binding");
            result.view = outputSubresource(result.resource, descriptor, layer, depth);
            result.view["slot"] = depth ? Json(nullptr) : Json(slot);
            const auto format = result.view.at("format").get<uint32_t>();
            if (!nativePixelFormatsCompatible(info.format, format) ||
                (s.typedFormat && *s.typedFormat != format))
                throw std::runtime_error("Private pixel history interpretation differs from its bound view");
            for (const auto &[id, object] : objects) {
                Com<ID3D11View> candidate;
                if (SUCCEEDED(object.As(&candidate)) && candidate.Get() == view) {
                    result.viewId = id;
                    break;
                }
            }
            if (!result.viewId)
                throw std::runtime_error("Private pixel history output has no GPA view identity");
            auto &target = result.raster.target;
            selected->GetType(&target.dimension);
            target.width = result.view.at("width").get<uint32_t>();
            target.height = result.view.at("height").get<uint32_t>();
            target.layers = result.view.at("layer_count").get<uint32_t>();
            target.format = DXGI_FORMAT(format);
            target.depth = depth;
            target.samples = {info.samples, result.resource.type == 0x85 ? result.resource.desc.at(6) : 0};
        };
        std::array<ID3D11RenderTargetView *, 8> raw{};
        std::array<Com<ID3D11RenderTargetView>, 8> views;
        Com<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(8, raw.data(), &depth);
        for (size_t i = 0; i < raw.size(); ++i)
            views[i].Attach(raw[i]);
        for (uint32_t i = 0; i < views.size(); ++i)
            if (views[i]) {
                D3D11_RENDER_TARGET_VIEW_DESC d{};
                views[i]->GetDesc(&d);
                consider(views[i].Get(), Bytes(reinterpret_cast<const uint8_t *>(&d), sizeof d), false, i);
            }
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC d{};
            depth->GetDesc(&d);
            consider(depth.Get(), Bytes(reinterpret_cast<const uint8_t *>(&d), sizeof d), true, 0);
        }
    });
    if (result.view.is_null())
        throw std::runtime_error("Selected pixel is not in a bound RTV or DSV");
    return result;
}
Json readPixel(Replay &r, NativePixelDecoder &decoder, const Target &target, const NativeHistorySelection &s,
               const CancelCheck &cancelled, uint64_t &readBytes) {
    checkCancellation(cancelled);
    const auto info = textureInfo(target.resource);
    const auto originalInfo = textureInfo(r.frame().resource(s.resource));
    const bool sameExtent = info.width == originalInfo.width && info.height == originalInfo.height &&
                            info.depth == originalInfo.depth && info.layers == originalInfo.layers &&
                            info.mips == originalInfo.mips && info.samples == originalInfo.samples;
    const auto format = target.view.at("format").get<uint32_t>();
    const bool capturedFamily = nativePixelFormatsCompatible(originalInfo.format, format);
    auto resource = target.resource;
    std::vector<uint8_t> bytes;
    bool point = false, canonicalX = false, exactBits = true;
    std::string transfer = "native_storage_copy";
    if (info.samples > 1) {
        if (!sameExtent || !capturedFamily)
            throw std::runtime_error(
                "MSAA history storage differs from the captured extent or format family");
        auto data = r.readMsaa(s.resource, s.sample, format);
        resource = std::move(data.resource);
        bytes = std::move(data.bytes);
        canonicalX = data.canonicalX;
        exactBits = data.integerBits || data.depthStencil;
        transfer = data.mode;
    } else if (auto narrow = sameExtent && capturedFamily
                                 ? r.readTexturePoint(s.resource, s.x, s.y, s.slice, s.mip, s.layer)
                                 : std::nullopt) {
        bytes = std::move(*narrow);
        point = true;
    } else {
        auto data = r.observeTextureStorage(s.resource, s.maxReadbackBytes, cancelled);
        resource = std::move(data.resource);
        bytes = std::move(data.bytes);
    }
    checkCancellation(cancelled);
    if (bytes.size() > s.maxReadbackBytes || bytes.size() > UINT64_MAX - readBytes)
        throw std::runtime_error("Private pixel history readback limit exceeded");
    readBytes += bytes.size();
    const auto sub = textureSubresources(resource).at(s.mip + s.layer * info.mips);
    const auto [size, rows] = pitches(1, 1, format);
    if (rows != 1 || !size)
        throw std::runtime_error("Private pixel history requires native texel storage");
    const auto offset = point ? 0
                              : sub.offset + uint64_t(s.slice) * sub.slicePitch +
                                    uint64_t(s.y) * sub.rowPitch + uint64_t(s.x) * size;
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("Private pixel history texel bounds");
    std::vector<uint8_t> value(bytes.begin() + ptrdiff_t(offset), bytes.begin() + ptrdiff_t(offset + size));
    std::vector<bool> valid(size, true);
    if (format == 20)
        for (size_t i = 5; i < 8; ++i)
            valid[i] = false;
    if (format == 88 || format == 93)
        valid[3] = false;
    return {{"bytes", value},
            {"valid_bytes", valid},
            {"defined", nullptr},
            {"typed_format", format},
            {"canonical_unused_x", canonicalX},
            {"exact_storage_bits", exactBits},
            {"transfer", transfer},
            {"readback_scope", point ? "single_texel" : "complete_storage"},
            {"decoded", decoder.decode(value, format, s.x, s.y)}};
}
bool equal(const Json &a, const Json &b) {
    if (a.at("typed_format") != b.at("typed_format") || a.at("valid_bytes") != b.at("valid_bytes") ||
        a.at("bytes").size() != b.at("bytes").size())
        throw std::runtime_error("Private pixel storage shape changed");
    for (size_t i = 0; i < a.at("bytes").size(); ++i)
        if (a.at("valid_bytes")[i] == true && a.at("bytes")[i] != b.at("bytes")[i])
            return false;
    return true;
}
} // namespace
nlohmann::json collectNativePixelDrawHistory(Replay &r, Id event, const NativeHistorySelection &s,
                                             const NativePixelDrawHistoryLimits &limits,
                                             const CancelCheck &cancelled,
                                             const NativePixelInvocationObserver &observeInvocation) {
    if (!s.maxRecords || !limits.maxCandidates || limits.maxCandidates > 2048 || !limits.timeoutMs ||
        limits.timeoutMs > 60000 || !limits.maxBytes ||
        (limits.evaluateShaderOutputs && (!limits.maxShaderBytes || !limits.maxShaderSteps)) ||
        (limits.evaluateOutputMerger && !limits.evaluateShaderOutputs) ||
        (limits.observePrimitiveTests && !limits.observePrimitivePresence) ||
        (observeInvocation && !limits.evaluateShaderOutputs))
        throw std::invalid_argument("Invalid private pixel history limits");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeoutMs);
    const CancelCheck bounded = [&] {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Private pixel history deadline");
        return false;
    };
    Target target;
    NativePixelDecoder decoder(r);
    uint64_t readBytes = 0, reads = 0, completed = 0, total = 0;
    Json before, previous, after, candidateReports = Json::array(), records = Json::array(),
                                  unattributed = Json::array();
    std::map<uint64_t, Json> candidates;
    struct Invocation {
        dxbc_exec::PixelTraceOptions inputs;
        std::vector<uint32_t> samples;
        uint64_t bytes = 0;
    };
    std::map<uint64_t, std::vector<Invocation>> shaderInputs;
    std::vector<uint8_t> pixelCode;
    Id pixelShader = 0;
    uint64_t shaderRetained = 0, shaderSteps = 0, shaderInvocations = 0;
    uint64_t mergerInvocations = 0, mergerReadbackBytes = 0;
    auto retainShader = [&](uint64_t bytes) {
        if (bytes > limits.maxShaderBytes - shaderRetained)
            throw std::runtime_error("Private pixel shader retention budget exceeded");
        shaderRetained += bytes;
    };
    auto snapshot = [&](Replay &replay) {
        ++reads;
        return readPixel(replay, decoder, target, s, bounded, readBytes);
    };
    const NativePixelCheckpointPlanner plan = [&](const PostTransformGeometry &geometry,
                                                  const dxbc_exec::PixelRasterState &raster) {
        checkCancellation(bounded);
        target = selectedTarget(r, s, raster);
        before = snapshot(r);
        total = geometry.report.at("primitives").get<uint64_t>();
        std::set<uint64_t> checkpoints;
        if (limits.evaluateShaderOutputs && total) {
            bool present = false;
            r.inspectNativeState([&](auto *context, const auto &) {
                Com<ID3D11PixelShader> shader;
                context->PSGetShader(&shader, nullptr, nullptr);
                present = shader != nullptr;
            });
            if (present) {
                auto inputs = dxbc_exec::snapshotBoundResourceInputs(r, 4, limits.maxShaderBytes, bounded);
                pixelCode = std::move(inputs.bytecode);
                pixelShader = inputs.shader;
                retainShader(pixelCode.size());
            }
        }
        if (total) {
            auto discovery = target.raster;
            // Discover geometry before the application sample mask; rejected
            // storage samples must not disappear merely because no value changed.
            discovery.sampleMask = UINT32_MAX;
            const auto forced = discovery.rasterizer.ForcedSampleCount;
            const uint32_t passes = forced ? forced : 1;
            uint32_t observations = 0;
            for (uint32_t pass = 0; pass < passes; ++pass) {
                NativePixelCandidateSelection select;
                select.x = s.x;
                select.y = s.y;
                select.layer = target.view.at("relative_layer").get<uint32_t>();
                select.sample = forced ? pass : s.sample;
                select.primitiveCount = total;
                select.maxRecords = std::max(1u, limits.maxCandidates - observations);
                select.timeoutMs = limits.timeoutMs;
                const auto sourceBytes =
                    geometry.bytes.size() + geometry.primitiveIds.size() * sizeof(uint32_t);
                if (sourceBytes >= limits.maxBytes)
                    throw std::runtime_error("Private pixel candidate payload budget exceeded");
                select.maxBytes = limits.maxBytes - sourceBytes;
                auto found = captureNativePixelCandidates(r.nativeDevice(), r.options().warp, discovery,
                                                          geometry, select, bounded);
                observations += uint32_t(found.report.at("observations").size());
                if (observations > limits.maxCandidates)
                    throw std::runtime_error("Private pixel history candidate record limit exceeded");
                for (const auto &row : found.report.at("observations")) {
                    const auto ordinal = row.at("primitive_ordinal").get<uint64_t>();
                    auto &entry = candidates[ordinal];
                    if (entry.is_null())
                        entry = {{"primitive_ordinal", ordinal},
                                 {"original_primitive_id", row.at("original_primitive_id")},
                                 {"geometry_instance", row.at("geometry_instance")},
                                 {"observations", Json::array()}};
                    if (candidates.size() > s.maxRecords)
                        throw std::runtime_error("Private pixel history primitive record limit exceeded");
                    entry["observations"].push_back({{"pass", pass},
                                                     {"observation_index", row.at("observation_index")},
                                                     {"raster_sample", select.sample},
                                                     {"selected_lane", row.at("selected_lane")}});
                    if (ordinal)
                        checkpoints.insert(ordinal);
                    checkpoints.insert(ordinal + 1);
                }
                candidateReports.push_back(std::move(found.report));
            }
        }
        if (limits.evaluateShaderOutputs) {
            auto discovery = target.raster;
            discovery.sampleMask = UINT32_MAX;
            std::optional<dxbc_exec::PixelRasterState> depthRaster;
            r.inspectNativeState([&](auto *context, const auto &) {
                auto state = dxbc_exec::pixelRasterState(context, {}, true);
                if (state.target.depth) {
                    state.sampleMask = UINT32_MAX;
                    depthRaster = std::move(state);
                }
            });
            // The bound depth format also determines depth bias for the
            // original PS inputs, independently of the selected color view.
            if (depthRaster)
                discovery.target = depthRaster->target;
            for (auto &[ordinal, candidate] : candidates) {
                candidate["private_shader_outputs"] = Json::array();
                candidate["pixel_shader_bound"] = pixelShader != 0;
                if (limits.evaluateOutputMerger)
                    candidate["private_output_mergers"] = Json::array();
                if (!pixelShader)
                    continue;
                if (limits.evaluateOutputMerger && depthRaster) {
                    auto observed = captureNativePixelSampleDepth(
                        r.nativeDevice(), r.options().warp, *depthRaster, geometry, ordinal,
                        {s.x, s.y, s.sample, target.view.at("relative_layer").get<uint32_t>()},
                        {limits.maxBytes, limits.timeoutMs}, bounded);
                    retainShader(observed.dump().size());
                    candidate["sample_depth_observation"] = std::move(observed);
                }
                std::set<uint32_t> samples;
                for (const auto &observation : candidate.at("observations"))
                    samples.insert(observation.at("raster_sample").get<uint32_t>());
                for (auto sample : samples) {
                    auto inputs = captureNativePixelInvocationInputs(
                        r.nativeDevice(), r.options().warp, discovery, geometry, pixelCode, ordinal,
                        {s.x, s.y, sample, target.view.at("relative_layer").get<uint32_t>()},
                        {limits.maxBytes, limits.timeoutMs}, bounded);
                    auto &invocations = shaderInputs[ordinal];
                    const bool pixelFrequency = inputs.inputProvenance.at("sample_frequency") == false;
                    auto duplicate =
                        std::find_if(invocations.begin(), invocations.end(), [&](const auto &old) {
                            return pixelFrequency && old.inputs.activeMask == inputs.activeMask &&
                                   old.inputs.selectedLane == inputs.selectedLane &&
                                   old.inputs.registers == inputs.registers;
                        });
                    if (duplicate != invocations.end()) {
                        const auto bytes = inputs.inputProvenance.dump().size();
                        retainShader(bytes);
                        duplicate->bytes += bytes;
                        duplicate->inputs.inputProvenance["equivalent_sample_observations"].push_back(
                            std::move(inputs.inputProvenance));
                        duplicate->samples.push_back(sample);
                        continue;
                    }
                    const uint64_t bytes =
                        inputs.inputProvenance.dump().size() +
                        uint64_t(4 * 33) * (sizeof(dxbc_exec::Register) + sizeof(dxbc_exec::Value));
                    retainShader(bytes);
                    invocations.push_back({std::move(inputs), {sample}, bytes});
                }
            }
        }
        return std::vector<uint64_t>(checkpoints.begin(), checkpoints.end());
    };
    const auto replay = replayNativePixelGeometryCheckpoints(
        r, event, plan,
        [&](Replay &privateReplay, uint64_t count) {
            auto current = snapshot(privateReplay);
            if (!count) {
                if (!equal(before, current))
                    throw std::runtime_error("Private pixel initial storage differs");
            } else {
                const auto found = candidates.find(completed);
                if (count == completed + 1 && found != candidates.end()) {
                    auto row = std::move(found->second);
                    row["event"] = event;
                    row["record_kind"] = "private_primitive_storage";
                    row["before"] = previous;
                    row["after"] = current;
                    row["observed_value_changed"] = !equal(previous, current);
                    row["storage_changed"] =
                        previous.at("exact_storage_bits") == true && current.at("exact_storage_bits") == true
                            ? row["observed_value_changed"]
                            : Json(nullptr);
                    row["instance"] = nullptr;
                    row["original_fragment_eligibility"] = nullptr;
                    row["write_executed"] = nullptr;
                    row["shader_output"] = nullptr;
                    row["failure_reasons"] = nullptr;
                    if (limits.evaluateOutputMerger)
                        for (auto &merger : row.at("private_output_mergers")) {
                            auto predicted = current;
                            predicted["bytes"] = merger.at("selected_after");
                            merger["observed_value_matches_checkpoint"] = equal(predicted, current);
                            merger["exact_storage_matches_checkpoint"] =
                                merger.at("selected_exact_storage_bits") == true &&
                                        current.at("exact_storage_bits") == true
                                    ? merger.at("observed_value_matches_checkpoint")
                                    : Json(nullptr);
                        }
                    records.push_back(std::move(row));
                } else if (!equal(previous, current))
                    unattributed.push_back({{"first_primitive", completed},
                                            {"primitive_count", count - completed},
                                            {"before", previous},
                                            {"after", current}});
            }
            completed = count;
            previous = std::move(current);
            after = previous;
            if (auto pending = shaderInputs.find(count); pending != shaderInputs.end()) {
                auto resources = dxbc_exec::snapshotBoundResourceInputs(
                    privateReplay, 4, limits.maxShaderBytes - shaderRetained, bounded);
                if (resources.shader != pixelShader || resources.bytecode != pixelCode ||
                    resources.resourceObservation != "current_native_bindings")
                    throw std::runtime_error("Private pixel shader resource context changed");
                uint64_t sourceBytes = resources.bytecode.size();
                auto account = [&](uint64_t bytes) {
                    if (bytes > limits.maxShaderBytes - shaderRetained - sourceBytes)
                        throw std::runtime_error("Private pixel shader input-copy budget exceeded");
                    sourceBytes += bytes;
                };
                if (sourceBytes > limits.maxShaderBytes - shaderRetained)
                    throw std::runtime_error("Private pixel shader bytecode budget exceeded");
                for (const auto &[slot, values] : resources.inputs.constantBuffers)
                    account(values.size() * sizeof(dxbc_exec::Value));
                if (resources.inputs.memory)
                    for (const auto &[id, buffer] : resources.inputs.memory->buffers())
                        account(buffer.bytes.size() + buffer.valid.size());
                if (resources.inputs.textures)
                    for (const auto &[id, texture] : resources.inputs.textures->resources)
                        account(texture.storage.bytes.size() + texture.storage.valid.size());
                std::optional<NativePixelMergerSnapshot> mergerSnapshot;
                if (limits.evaluateOutputMerger) {
                    NativePixelMergerSelection selection;
                    if (!target.raster.target.depth)
                        selection.colorSlot = target.view.at("slot").get<uint32_t>();
                    selection.x = s.x;
                    selection.y = s.y;
                    selection.layer = target.view.at("relative_layer").get<uint32_t>();
                    selection.maxReadbackBytes = s.maxReadbackBytes;
                    selection.timeoutMs = limits.timeoutMs;
                    mergerSnapshot = snapshotNativePixelMergerState(privateReplay, selection, bounded);
                    mergerReadbackBytes += mergerSnapshot->identity.at("readback_bytes").get<uint64_t>();
                    mergerSnapshot->state.maxBytes = limits.maxBytes;
                }
                for (auto &invocation : pending->second) {
                    checkCancellation(bounded);
                    if (shaderSteps >= limits.maxShaderSteps)
                        throw std::runtime_error("Private pixel shader global step budget exceeded");
                    invocation.inputs.maxSteps = limits.maxShaderSteps - shaderSteps;
                    invocation.inputs.maxBytes = limits.maxShaderBytes - shaderRetained - sourceBytes;
                    if (!invocation.inputs.maxBytes)
                        throw std::runtime_error("Private pixel shader execution storage budget exceeded");
                    auto output = evaluateNativePixelOutputs(resources, invocation.inputs, bounded);
                    shaderSteps += output.at("global_steps").get<uint64_t>();
                    ++shaderInvocations;
                    output["resource_boundary"] = "before_private_primitive";
                    output["completed_primitives"] = count;
                    output["candidate_raster_samples"] = invocation.samples;
                    if (mergerSnapshot) {
                        const auto &provenance = output.at("input_provenance");
                        const auto &raster = provenance.at("selected_raster");
                        const auto lane = output.at("selected_lane").get<uint32_t>();
                        if (raster.at("lane") != lane || provenance.at("primitive_ordinal") != count ||
                            output.at("frame_sha256") != privateReplay.frame().sha256(bounded))
                            throw std::runtime_error("Private merger input provenance differs");
                        auto state = mergerSnapshot->state;
                        state.rasterDepthBits = raster.at("depth_bits").get<uint32_t>();
                        if (state.depthFormat) {
                            const auto &depth = candidates.at(count).at("sample_depth_observation");
                            if (depth.at("sample") != s.sample || depth.at("primitive_ordinal") != count ||
                                depth.at("front_face") != raster.at("front_face"))
                                throw std::runtime_error("Private merger sample-depth identity differs");
                            state.rasterDepthBits = depth.at("depth_bits").get<uint32_t>();
                        }
                        state.frontFace = raster.at("front_face").get<bool>();
                        state.coverageMask = raster.at("coverage_mask").get<uint32_t>();
                        // Only the selected storage sample is predicted. Other
                        // samples can belong to distinct shader invocations.
                        state.sampleMask &= 1u << s.sample;
                        state.measureTests = true;
                        auto merged =
                            mergeNativePixelOutput(privateReplay.nativeDevice(), privateReplay.options().warp,
                                                   state, output, lane, bounded);
                        const auto &targetSnapshot = mergerSnapshot->identity.at("targets").at(
                            target.raster.target.depth && state.colorFormat ? 1 : 0);
                        const auto &subresource = targetSnapshot.at("subresource");
                        if (targetSnapshot.at("resource") != s.resource || subresource.at("mip") != s.mip ||
                            subresource.at("array_layer") != s.layer || subresource.at("slice") != s.slice)
                            throw std::runtime_error("Private merger target identity differs");
                        const auto key = target.raster.target.depth ? "depth_after" : "color_after";
                        const auto bytes = merged.at(key).get<std::vector<uint8_t>>();
                        const auto texel = pitches(1, 1, target.view.at("format").get<uint32_t>()).first;
                        const auto offset = size_t(s.sample) * texel;
                        if (offset > bytes.size() || texel > bytes.size() - offset)
                            throw std::runtime_error("Private merger selected sample bounds");
                        Json joined = {
                            {"scope", "one_private_shader_lane_on_selected_storage_sample"},
                            {"shader_output_index", candidates.at(count).at("private_shader_outputs").size()},
                            {"selected_sample", s.sample},
                            {"application_sample_mask", mergerSnapshot->state.sampleMask},
                            {"selected_after",
                             std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + offset + texel)},
                            {"selected_exact_storage_bits", targetSnapshot.at("exact_storage_bits")},
                            {"binding_snapshot", mergerSnapshot->identity},
                            {"raster_inputs", raster},
                            {"raster_depth_source",
                             state.depthFormat ? "sample_position_observation" : "unused"},
                            {"original_fragment_eligibility", nullptr},
                            {"observed_value_matches_checkpoint", false},
                            {"exact_storage_matches_checkpoint", false},
                            {"merger", std::move(merged)}};
                        retainShader(joined.dump().size());
                        candidates.at(count)["private_output_mergers"].push_back(std::move(joined));
                        ++mergerInvocations;
                    }
                    if (observeInvocation) {
                        observeInvocation(count, resources, invocation.inputs, invocation.samples);
                        checkCancellation(bounded);
                    }
                    shaderRetained -= invocation.bytes;
                    retainShader(output.dump().size());
                    candidates.at(count)["private_shader_outputs"].push_back(std::move(output));
                }
                shaderInputs.erase(pending);
            }
        },
        limits.maxBytes, limits.timeoutMs, bounded);
    if (completed != total || records.size() != candidates.size() || !shaderInputs.empty())
        throw std::runtime_error("Incomplete private pixel checkpoint join");
    Json originalBefore, originalAfter;
    auto original = replayNativeOriginalDraw(
        r, event,
        [&](Replay &privateReplay, bool done) {
            (done ? originalAfter : originalBefore) = snapshot(privateReplay);
        },
        limits.maxBytes, limits.timeoutMs, bounded);
    if (!equal(before, originalBefore))
        throw std::runtime_error("Original draw observation started from different pixel storage");
    uint64_t bridgePassing = 0;
    for (const auto &batch : replay.at("batches")) {
        const auto passing = batch.at("passing_samples").get<uint64_t>();
        if (passing > UINT64_MAX - bridgePassing)
            throw std::runtime_error("Geometry replay sample count overflow");
        bridgePassing += passing;
    }
    original["before"] = originalBefore;
    original["after"] = originalAfter;
    original["geometry_observed_value_matches"] = equal(after, originalAfter);
    original["geometry_exact_storage_matches"] =
        after.at("exact_storage_bits") == true && originalAfter.at("exact_storage_bits") == true
            ? original.at("geometry_observed_value_matches")
            : Json(nullptr);
    original["geometry_passing_samples"] = bridgePassing;
    original["geometry_passing_samples_match"] = original.at("passing_samples") == bridgePassing;
    Json returnCount{{"requested", limits.observeReturnCount},
                     {"status", "not_requested"},
                     {"count", nullptr},
                     {"original_fragment_eligibility", nullptr}};
    if (limits.observeReturnCount) {
        const bool depthTarget = target.view.at("slot").is_null();
        bool colorBound = false;
        if (depthTarget)
            r.inspectNativeState([&](auto *context, const auto &) {
                std::array<ID3D11RenderTargetView *, 8> views{};
                context->OMGetRenderTargets(UINT(views.size()), views.data(), nullptr);
                for (auto *view : views)
                    if (view) {
                        colorBound = true;
                        view->Release();
                    }
            });
        if (depthTarget && colorBound) {
            returnCount["status"] = "not_applicable";
            returnCount["reason"] = "selected_depth_target";
        } else if (!depthTarget && original.at("shaders")[4].at("resource") == 0) {
            returnCount["status"] = "not_applicable";
            returnCount["reason"] = "no_pixel_shader";
        } else {
            const auto observation =
                depthTarget
                    ? replayNativeDepthReturnCount(
                          r, event, {s.x, s.y, target.view.at("relative_layer").get<uint32_t>(), s.sample},
                          {}, limits.maxBytes, limits.timeoutMs, bounded)
                    : replayNativePixelReturnCount(r, event,
                                                   {target.view.at("slot").get<uint32_t>(), s.x, s.y,
                                                    target.view.at("relative_layer").get<uint32_t>(),
                                                    s.sample},
                                                   {}, limits.maxBytes, limits.timeoutMs, bounded);
            if (observation.at("frame_sha256") != original.at("frame_sha256") ||
                observation.at("adapter") != original.at("adapter") ||
                observation.at("shaders") != original.at("shaders"))
                throw std::runtime_error("Private return count context mismatch");
            const auto size = observation.dump().size();
            if (size > limits.maxBytes)
                throw std::runtime_error("Private return count retained payload budget exceeded");
            returnCount["retained_payload_bytes"] = size;
            returnCount["status"] =
                observation.at("return_count").at("valid") == true ? "observed" : "unknown";
            returnCount["count"] = observation.at("return_count").at("count");
            returnCount["observation"] = observation;
        }
    }
    Json identity{{"requested", limits.observePrimitiveIdentity}, {"status", "not_requested"}};
    Json association{{"scope", "system_id_equality_only"},
                     {"status", "not_observed"},
                     {"matching_primitive_ordinals", Json::array()},
                     {"unknown_identity_ordinals", Json::array()},
                     {"original_fragment_eligibility", nullptr}};
    if (limits.observePrimitiveIdentity) {
        if (target.view.at("slot").is_null()) {
            identity["status"] = "not_applicable";
            identity["reason"] = "selected_depth_target";
        } else if (original.at("shaders")[4].at("resource") == 0) {
            identity["status"] = "not_applicable";
            identity["reason"] = "no_pixel_shader";
        } else {
            identity = observeNativePixelIdentity(
                r, event, target.view.at("slot").get<uint32_t>(),
                [&](Replay &privateReplay) {
                    const auto pixel = snapshot(privateReplay);
                    return NativePixelPredicateStorage{pixel.at("typed_format").get<uint32_t>(),
                                                       pixel.at("bytes").get<std::vector<uint8_t>>()};
                },
                limits.maxBytes, limits.timeoutMs, bounded);
            identity["requested"] = true;
            if (identity.at("status") == "verified_conditional_replay") {
                const auto observed = identity.at("system_primitive_id");
                for (auto &row : records) {
                    const auto id = row.at("original_primitive_id");
                    row["system_id_matches_verified_predicate"] =
                        id.is_null() ? Json(nullptr) : Json(id == observed);
                    if (id.is_null())
                        association["unknown_identity_ordinals"].push_back(row.at("primitive_ordinal"));
                    else if (id == observed)
                        association["matching_primitive_ordinals"].push_back(row.at("primitive_ordinal"));
                }
                const auto matches = association.at("matching_primitive_ordinals").size();
                association["status"] = matches == 0   ? "no_matching_candidate"
                                        : matches == 1 ? "one_matching_candidate"
                                                       : "ambiguous_candidates";
            }
        }
    }
    Json presence{{"requested", limits.observePrimitivePresence},
                  {"test_variants_requested", limits.observePrimitiveTests},
                  {"status", "not_requested"},
                  {"scope", "system_id_color_completion_in_repeated_private_draws"},
                  {"original_fragment_eligibility", nullptr},
                  {"observations", Json::array()}};
    if (limits.observePrimitivePresence) {
        if (target.view.at("slot").is_null() || original.at("shaders")[4].at("resource") == 0) {
            presence["status"] = "not_applicable";
            presence["reason"] =
                target.view.at("slot").is_null() ? "selected_depth_target" : "no_pixel_shader";
        } else {
            std::map<uint32_t, size_t> observed;
            uint64_t retained = 0;
            for (auto &row : records) {
                row["system_id_presence_observation"] = nullptr;
                if (row.at("original_primitive_id").is_null())
                    continue;
                const auto id = row.at("original_primitive_id").get<uint32_t>();
                if (!observed.contains(id)) {
                    auto pair = [&](NativePixelTestOverride mode) -> Json {
                        Json passes = Json::array();
                        for (unsigned repeat = 0; repeat < 2; ++repeat) {
                            Json initial, final;
                            const auto observation = replayNativePixelPrimitivePresence(
                                r, event, target.view.at("slot").get<uint32_t>(), {UINT32_MAX, id},
                                [&](Replay &privateReplay, bool after) {
                                    const auto pixel = snapshot(privateReplay);
                                    (after ? final : initial) = decodeNativePixelPredicate(
                                        {pixel.at("typed_format").get<uint32_t>(),
                                         pixel.at("bytes").get<std::vector<uint8_t>>()});
                                },
                                limits.maxBytes, limits.timeoutMs, bounded, mode);
                            if (observation.at("frame_sha256") != original.at("frame_sha256") ||
                                observation.at("adapter") != original.at("adapter") ||
                                observation.at("shaders") != original.at("shaders") ||
                                initial.at("typed_format") != target.view.at("format") ||
                                final.at("typed_format") != target.view.at("format") ||
                                initial.at("rgb_predicate") != false)
                                throw std::runtime_error(
                                    "Private primitive presence context or clear mismatch");
                            Json pass{{"before", initial}, {"after", final}, {"observation", observation}};
                            const auto bytes = pass.dump().size();
                            if (bytes > limits.maxBytes - retained)
                                throw std::runtime_error(
                                    "Private primitive presence retained payload budget exceeded");
                            retained += bytes;
                            passes.push_back(std::move(pass));
                        }
                        const auto first = passes[0].at("after").at("rgb_predicate");
                        const auto second = passes[1].at("after").at("rgb_predicate");
                        const bool verified = !first.is_null() && first == second;
                        return {{"status", verified ? "repeat_agreement" : "unverified"},
                                {"color_completion_observed", verified ? first : Json(nullptr)},
                                {"passes", std::move(passes)}};
                    };
                    auto entry = pair(NativePixelTestOverride::Preserve);
                    entry["system_primitive_id"] = id;
                    entry["matching_primitive_ordinals"] = Json::array();
                    entry["test_variants"] = Json::array();
                    if (limits.observePrimitiveTests) {
                        for (auto mode :
                             {NativePixelTestOverride::DisableDepth, NativePixelTestOverride::DisableStencil,
                              NativePixelTestOverride::DisableDepthStencil,
                              NativePixelTestOverride::DisableAlphaToCoverage,
                              NativePixelTestOverride::AllSamples}) {
                            auto variant = pair(mode);
                            variant["mode"] = nativePixelTestOverrideName(mode);
                            variant["failure_reason"] = nullptr;
                            const auto base = entry.at("color_completion_observed");
                            const auto value = variant.at("color_completion_observed");
                            variant["differs_from_preserved_tests"] =
                                base.is_null() || value.is_null() ? Json(nullptr) : Json(base != value);
                            entry["test_variants"].push_back(std::move(variant));
                        }
                    }
                    observed[id] = presence.at("observations").size();
                    presence["observations"].push_back(std::move(entry));
                }
                const auto index = observed.at(id);
                row["system_id_presence_observation"] = index;
                presence["observations"][index]["matching_primitive_ordinals"].push_back(
                    row.at("primitive_ordinal"));
            }
            presence["status"] = "observed";
            presence["retained_payload_bytes"] = retained;
            presence["pass_count"] = observed.size() * (limits.observePrimitiveTests ? 12 : 2);
        }
    }
    const auto restored = snapshot(r);
    if (!equal(before, restored))
        throw std::runtime_error("Private pixel history changed original storage");
    return {
        {"schema", 1},
        {"kind", "private_pixel_draw_history"},
        {"backend", "native-pixel-draw-history/3"},
        {"event", event},
        {"frame_sha256", replay.at("frame_sha256")},
        {"adapter", replay.at("adapter")},
        {"selection",
         {{"resource", s.resource},
          {"view", target.viewId},
          {"x", s.x},
          {"y", s.y},
          {"mip", s.mip},
          {"layer", s.layer},
          {"slice", s.slice},
          {"sample", s.sample},
          {"subresource", target.view}}},
        {"before", before},
        {"after", after},
        {"restored", restored},
        {"records", records},
        {"unattributed_changes", unattributed},
        {"candidate_passes", candidateReports},
        {"geometry_replay", replay},
        {"original_draw_observation", original},
        {"return_count_observation", returnCount},
        {"primitive_identity_observation", identity},
        {"primitive_identity_association", association},
        {"primitive_presence_observation", presence},
        {"shader_evaluation",
         {{"requested", limits.evaluateShaderOutputs},
          {"invocations", shaderInvocations},
          {"global_steps", shaderSteps},
          {"retained_payload_bytes", shaderRetained},
          {"resource_boundary", "before_private_primitive"},
          {"original_fragment_eligibility", nullptr}}},
        {"merger_evaluation",
         {{"requested", limits.evaluateOutputMerger},
          {"invocations", mergerInvocations},
          {"readback_bytes", mergerReadbackBytes},
          {"resource_boundary", "before_private_primitive"},
          {"original_fragment_eligibility", nullptr}}},
        {"readback_count", reads},
        {"readback_bytes", readBytes},
        {"order", "captured_geometry_storage"},
        {"sample_selection_domain", "target_storage"},
        {"observed_storage_restored", true},
        {"original_storage_restored", before.at("exact_storage_bits") == true ? Json(true) : Json(nullptr)},
        {"original_fragment_eligibility", nullptr},
        {"fragment_history_available", false},
        {"original_gpu_schedule", false},
        {"pre_raster_side_effects_replayed", false}};
}
} // namespace flora
