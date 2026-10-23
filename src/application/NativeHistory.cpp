#include "NativeHistory.h"
#include "NativePixel.h"
#include "ViewEdits.h"
#include "core/ClearView.h"
#include "core/Commands.h"
#include "core/CopyCommands.h"
#include "core/TextureStorage.h"
#include <algorithm>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
struct Cell {
    uint64_t offset{};
    uint32_t bytes{};
    std::optional<std::pair<uint64_t, uint32_t>> chroma;
    bool exactPixel = true;
    std::string encoding = "texel";
};
Cell cell(const Resource &resource, const NativeHistorySelection &s) {
    const auto info = textureInfo(resource);
    const auto subs = textureSubresources(resource);
    const auto &sub = subs.at(s.mip + s.layer * info.mips);
    const auto f = info.format;
    Cell c;
    c.offset = sub.offset + uint64_t(s.slice) * sub.slicePitch;
    if ((f >= 70 && f <= 84) || (f >= 94 && f <= 99)) {
        c.bytes = pitches(1, 1, f).first;
        c.offset += uint64_t(s.y / 4) * sub.rowPitch + uint64_t(s.x / 4) * c.bytes;
        c.exactPixel = false;
        c.encoding = "compressed_4x4_block";
    } else if (f == 68 || f == 69 || (f >= 107 && f <= 109)) {
        c.bytes = pitches(2, 1, f).first;
        c.offset += uint64_t(s.y) * sub.rowPitch + uint64_t(s.x / 2) * c.bytes;
        c.exactPixel = false;
        c.encoding = "packed_two_pixel_group";
    } else if (f == 66) {
        c.bytes = 1;
        c.offset += uint64_t(s.y) * sub.rowPitch + s.x / 8;
        c.exactPixel = false;
        c.encoding = "packed_eight_pixel_group";
    } else if (f >= 103 && f <= 105) {
        c.bytes = f == 103 ? 1u : 2u;
        c.offset += uint64_t(s.y) * sub.rowPitch + uint64_t(s.x) * c.bytes;
        c.chroma = {{sub.offset + uint64_t(sub.rowPitch) * sub.height + uint64_t(s.y / 2) * sub.rowPitch +
                         uint64_t(s.x / 2) * c.bytes * 2,
                     c.bytes * 2}};
        c.exactPixel = false;
        c.encoding = "planar_luma_then_shared_chroma";
    } else {
        c.bytes = pitches(1, 1, f).first;
        c.offset += uint64_t(s.y) * sub.rowPitch + uint64_t(s.x) * c.bytes;
    }
    if (!c.bytes || c.offset < sub.offset || c.offset + c.bytes > sub.offset + sub.size)
        throw std::runtime_error("Native history cell exceeds its selected subresource");
    return c;
}
std::string hex(Bytes bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        result += digits[b >> 4];
        result += digits[b & 15];
    }
    return result;
}
bool inRange(uint32_t value, uint32_t first, uint32_t count) {
    return value >= first && uint64_t(value) - first < count;
}
bool rectangles(const std::vector<std::array<int32_t, 4>> &rects, const NativeHistorySelection &s) {
    return std::any_of(rects.begin(), rects.end(), [&](const auto &r) {
        return int64_t(s.x) >= r[0] && int64_t(s.y) >= r[1] && int64_t(s.x) < r[2] && int64_t(s.y) < r[3];
    });
}
bool contains(const Json &d, const std::string &kind, const NativeHistorySelection &s,
              bool generated = false) {
    if (kind == "srv") {
        const auto mip = d.value("most_detailed_mip", 0u);
        if (!inRange(s.mip, mip, d.value("mip_levels", 1u)) || (generated && s.mip == mip))
            return false;
        const auto dimension = d.at("dimension").get<uint32_t>();
        if (dimension == 9)
            return s.layer < 6;
        if (dimension == 10)
            return s.layer >= d.at("first_2d_array_face").get<uint32_t>() &&
                   uint64_t(s.layer) - d.at("first_2d_array_face").get<uint32_t>() <
                       uint64_t(d.at("num_cubes").get<uint32_t>()) * 6;
    } else if (s.mip != d.value("mip_slice", 0u))
        return false;
    if (!inRange(s.layer, d.value("first_array_slice", 0u), d.value("array_size", 1u)))
        return false;
    return !d.contains("first_w_slice") ||
           inRange(s.slice, d.at("first_w_slice").get<uint32_t>(), d.at("w_size").get<uint32_t>());
}
Json viewFootprint(const Frame &frame, Id view, const NativeHistorySelection &s, bool generated = false) {
    const auto info = describeView(frame, view);
    if (info.at("resource") != s.resource || !contains(info.at("descriptor"), info.at("kind"), s, generated))
        return nullptr;
    return {{"view", view}, {"kind", info.at("kind")}, {"descriptor", info.at("descriptor")}};
}
Json commandFootprint(const Replay &replay, Id event, const NativeHistorySelection &s, const Cell &selected,
                      const MapRecordAudit &maps) {
    const auto &frame = replay.frame();
    const auto type = frame.entry(event).type;
    Bytes payload = frame.payload(event);
    if (const auto found = replay.options().commandPayloads.find(event);
        found != replay.options().commandPayloads.end())
        payload = found->second;
    const auto info = textureInfo(frame.resource(s.resource));
    const uint32_t subresource = s.mip + s.layer * info.mips;
    const auto whole = [&](const char *operation) {
        return Json{{"operation", operation}, {"coverage", "addressed"}};
    };
    if (isCopyCommand(type)) {
        const auto copy = readCopyCommand(type, payload);
        if (copy.destination != s.resource || type == 0x3f)
            return nullptr;
        if (type != 0x3e && copy.destinationSubresource != subresource)
            return nullptr;
        if (type == 0x40 || type == 0x256) {
            const auto source = textureInfo(frame.resource(copy.source));
            const auto mip = copy.sourceSubresource % source.mips;
            const auto box = copy.box.value_or(std::array<uint32_t, 6>{
                0, 0, 0, std::max(1u, source.width >> mip), std::max(1u, source.height >> mip),
                std::max(1u, source.depth >> mip)});
            for (size_t axis = 0; axis < 3; ++axis)
                if (box[axis] >= box[axis + 3])
                    return nullptr;
            if (!inRange(s.x, copy.x, box[3] - box[0]) || !inRange(s.y, copy.y, box[4] - box[1]) ||
                !inRange(s.slice, copy.z, box[5] - box[2]))
                return nullptr;
        }
        auto out = whole(type == 0x42 ? "resolve" : "copy");
        out["source"] = copy.source;
        return out;
    }
    if (type == 0x247 || type == 0x255) {
        const auto update = updateSourceLayout(frame, event);
        if (update.destination != s.resource || update.subresource != subresource || update.empty)
            return nullptr;
        if (update.hasBox && (!inRange(s.x, update.box[0], update.box[3] - update.box[0]) ||
                              !inRange(s.y, update.box[1], update.box[4] - update.box[1]) ||
                              !inRange(s.slice, update.box[2], update.box[5] - update.box[2])))
            return nullptr;
        return whole("cpu_update");
    }
    if (type == 0x246) {
        const auto &map = requireMapRecord(maps, event);
        if (map.resource != s.resource || map.subresource != subresource || map.result < 0 || map.kind == 1)
            return nullptr;
        const auto layout = mappedWriteLayout(frame, map);
        bool addressed = layout.full;
        if (!addressed) {
            const auto subs = textureSubresources(frame.resource(s.resource));
            const auto offset = selected.offset - subs.at(subresource).offset;
            for (const auto &[start, bytes] : layout.updates)
                addressed |= start < offset + selected.bytes && offset < start + bytes.size();
        }
        if (!addressed && map.kind != D3D11_MAP_WRITE_DISCARD)
            return nullptr;
        auto out = whole("cpu_map");
        out["map_type"] = map.kind;
        out["captured_bytes_overlap"] = addressed;
        out["coverage"] = addressed ? "addressed" : "discarded";
        return out;
    }
    if (isClearCommand(type) || type == 0x245 || type == 0x257) {
        Reader reader(payload);
        reader.skip(16);
        const auto view = reader.read<Id>();
        auto footprint = viewFootprint(frame, view, s, type == 0x245);
        if (footprint.is_null())
            return nullptr;
        if (type == 0x31) {
            const auto flags = reader.read<uint32_t>();
            if (!flags)
                return nullptr;
            footprint["clear_flags"] = flags;
        }
        if (type == 0x257) {
            const auto clear = readClearView(payload);
            if (clear.hasRectangles && clear.count && !rectangles(clear.rectangles, s))
                return nullptr;
        }
        footprint["operation"] = type == 0x245 ? "generate_mips" : "clear";
        footprint["coverage"] = "addressed";
        return footprint;
    }
    if (isDiscardRecord(type)) {
        const auto discard = readDiscardRecord(type, payload);
        if (type == 0x3553) {
            if (discard.target != s.resource)
                return nullptr;
        } else {
            if (viewFootprint(frame, discard.target, s).is_null())
                return nullptr;
            if (discard.hasRectangles && discard.count && !rectangles(discard.rectangles, s))
                return nullptr;
        }
        return {{"operation", "discard"}, {"coverage", "discarded"}};
    }
    return nullptr;
}
Json boundOutputs(ID3D11DeviceContext *context, ID3D11Resource *selected, const NativeHistorySelection &s,
                  bool compute, D3D_FEATURE_LEVEL level) {
    Json result = Json::array();
    auto add = [&](ID3D11View *view, const std::string &kind, uint32_t slot, const auto &descriptor) {
        if (!view)
            return;
        Com<ID3D11Resource> resource;
        view->GetResource(&resource);
        if (resource.Get() != selected)
            return;
        const auto d =
            unpackView(kind, Bytes(reinterpret_cast<const uint8_t *>(&descriptor), sizeof descriptor));
        if (contains(d, kind, s))
            result.push_back({{"kind", kind}, {"slot", slot}, {"descriptor", d}});
    };
    if (!compute) {
        std::array<ID3D11RenderTargetView *, 8> targets{};
        std::array<Com<ID3D11RenderTargetView>, 8> ownedTargets;
        Com<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(8, targets.data(), &depth);
        for (size_t slot = 0; slot < targets.size(); ++slot)
            ownedTargets[slot].Attach(targets[slot]);
        for (uint32_t slot = 0; slot < targets.size(); ++slot) {
            const auto &target = ownedTargets[slot];
            if (target) {
                D3D11_RENDER_TARGET_VIEW_DESC d{};
                target->GetDesc(&d);
                add(target.Get(), "rtv", slot, d);
            }
        }
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC d{};
            depth->GetDesc(&d);
            add(depth.Get(), "dsv", 0, d);
        }
    }
    // Extended UAV slots are exposed only by feature level 11.1 and later.
    const auto count = level >= D3D_FEATURE_LEVEL_11_1 ? 64u : 8u;
    std::array<ID3D11UnorderedAccessView *, 64> views{};
    std::array<Com<ID3D11UnorderedAccessView>, 64> ownedViews;
    if (compute)
        context->CSGetUnorderedAccessViews(0, count, views.data());
    else
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, views.data());
    for (uint32_t slot = 0; slot < count; ++slot)
        ownedViews[slot].Attach(views[slot]);
    for (uint32_t slot = 0; slot < count; ++slot) {
        const auto &view = ownedViews[slot];
        if (view) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
            view->GetDesc(&d);
            add(view.Get(), "uav", slot, d);
        }
    }
    return result;
}
} // namespace
Json collectNativeEventHistory(Replay &replay, const NativeHistorySelection &s,
                               const CancelCheck &cancelled) {
    if (replay.options().until || replay.options().before || replay.options().measurement ||
        replay.options().timings)
        throw std::invalid_argument("Event history requires its own complete replay boundaries");
    const auto resource = replay.frame().resource(s.resource);
    const auto info = textureInfo(resource);
    if (!info.mips || info.mips > 32 || s.mip >= info.mips || s.layer >= info.layers ||
        s.x >= std::max(1u, info.width >> s.mip) || s.y >= std::max(1u, info.height >> s.mip) ||
        s.slice >= std::max(1u, info.depth >> s.mip) || s.sample >= info.samples || !s.maxRecords)
        throw std::invalid_argument("Native history selection exceeds texture bounds");
    const auto subs = textureSubresources(resource);
    const auto storageSize = subs.back().offset + subs.back().size;
    if (!s.maxReadbackBytes || storageSize > s.maxReadbackBytes)
        throw std::invalid_argument("Native history readback exceeds its storage budget");
    const auto selected = cell(resource, s);
    if (s.typedFormat && !nativePixelFormatsCompatible(info.format, *s.typedFormat))
        throw std::invalid_argument("Native history typed format is incompatible with resource storage");
    const auto maps = auditMapRecords(replay.frame(), cancelled);
    NativePixelDecoder decoder(replay);
    Json records = Json::array(), pending = nullptr;
    uint64_t reads = 0, bytesRead = 0, pointReads = 0;
    auto snapshot = [&](const auto &objects, const Json &footprint) -> Json {
        checkCancellation(cancelled);
        if (!objects.contains(s.resource))
            return {{"available", false}, {"reason", "resource_not_materialized"}, {"defined", nullptr}};
        auto storageResource = resource;
        std::vector<uint8_t> bytes;
        bool canonicalX = false;
        bool point = false;
        if (info.samples > 1) {
            auto data = replay.readMsaa(s.resource, s.sample,
                                        s.typedFormat.value_or(defaultTextureFormat(info.format)));
            bytes = std::move(data.bytes);
            storageResource = std::move(data.resource);
            canonicalX = data.canonicalX;
        } else if (auto narrow = replay.readTexturePoint(s.resource, s.x, s.y, s.slice, s.mip, s.layer)) {
            bytes = std::move(*narrow);
            point = true;
            ++pointReads;
        } else
            bytes = replay.readTexture(s.resource);
        if (bytes.size() > s.maxReadbackBytes)
            throw std::runtime_error("Native history output exceeds its storage budget");
        auto location = cell(storageResource, s);
        if (point)
            location.offset = 0;
        if (location.offset > bytes.size() || location.bytes > bytes.size() - location.offset)
            throw std::runtime_error("Native history storage is incomplete");
        auto value = hex(Bytes(bytes).subspan(size_t(location.offset), location.bytes));
        if (location.chroma) {
            const auto [offset, count] = *location.chroma;
            if (offset > bytes.size() || count > bytes.size() - offset)
                throw std::runtime_error("Native history chroma storage is incomplete");
            value += hex(Bytes(bytes).subspan(size_t(offset), count));
        }
        ++reads;
        bytesRead += bytes.size();
        std::optional<uint32_t> interpretation = s.typedFormat;
        std::string interpretationSource = interpretation ? "explicit_selection" : "unknown";
        if (!interpretation) {
            std::set<uint32_t> formats;
            if (footprint.contains("descriptor"))
                formats.insert(footprint.at("descriptor").value("format", 0u));
            if (footprint.contains("outputs"))
                for (const auto &output : footprint.at("outputs"))
                    formats.insert(output.at("descriptor").value("format", 0u));
            formats.erase(0);
            if (formats.size() == 1 && nativePixelFormatsCompatible(info.format, *formats.begin())) {
                interpretation = *formats.begin();
                interpretationSource = footprint.contains("descriptor") ? "command_view" : "bound_view";
            } else if (formats.empty() && !nativePixelTypeless(info.format)) {
                interpretation = info.format;
                interpretationSource = "resource_format";
            }
        }
        const auto decoded = decoder.decode(Bytes(bytes).subspan(size_t(location.offset), location.bytes),
                                            interpretation.value_or(0), s.x, s.y);
        return {{"available", true},
                {"defined", nullptr},
                {"format", textureInfo(storageResource).format},
                {"encoding", location.encoding},
                {"readback_scope", point ? "single_texel" : "complete_storage"},
                {"exact_pixel_storage", location.exactPixel},
                {"bytes", value},
                {"canonical_unused_x", canonicalX},
                {"value_format", interpretation ? Json(*interpretation) : Json(nullptr)},
                {"value_format_source", interpretationSource},
                {"decoding_status", decoded.at("status")},
                {"decoding_reason", decoded.value("reason", Json(nullptr))},
                {"decoding_source", decoded.at("source")},
                {"decoding_adapter", decoded.value("gpu_adapter", Json(nullptr))},
                {"storage_math_color", decoded.value("storage_math_color", Json(nullptr))},
                {"decoded_color", decoded.at("color")},
                {"depth", decoded.at("depth")},
                {"stencil", decoded.at("stencil")}};
    };
    auto boundary = [&](Id event, bool after, const auto &objects, Json footprint, const char *boundaryName) {
        checkCancellation(cancelled);
        if (!after) {
            if (footprint.is_null())
                return;
            if (records.size() >= s.maxRecords)
                throw std::runtime_error("Native history record limit exceeded");
            if (!pending.is_null())
                throw std::runtime_error("Nested native history boundary");
            const auto type = replay.frame().entry(event).type;
            const bool suppressed =
                replay.options().suppressDraws && isDraw(type) && type != 0x35 && type != 0x36;
            pending = {{"event", event},
                       {"api", commandName(replay.frame().entry(event).type)},
                       {"record_kind", "event_snapshot"},
                       {"boundary", boundaryName},
                       {"enabled", !suppressed && !replay.options().disabled.contains(event)},
                       {"footprint", footprint},
                       {"before", snapshot(objects, footprint)},
                       {"fragment", nullptr},
                       {"shader_output", nullptr},
                       {"failure_reasons", nullptr},
                       {"write_executed", nullptr}};
        } else if (!pending.is_null()) {
            if (pending.at("event") != event)
                throw std::runtime_error("Unpaired native history boundary");
            const bool disabled = !pending.at("enabled").get<bool>();
            if (!disabled && pending.at("footprint").value("coverage", std::string()) == "discarded")
                pending["after"] = {
                    {"available", false}, {"reason", "discarded_storage"}, {"defined", false}};
            else
                pending["after"] = snapshot(objects, pending.at("footprint"));
            pending["storage_changed"] = nullptr;
            pending["pixel_storage_changed"] = nullptr;
            const auto &a = pending.at("before"), &b = pending.at("after");
            if (a.at("available") == true && b.at("available") == true) {
                pending["storage_changed"] = a.at("bytes") != b.at("bytes");
                if (a.at("exact_pixel_storage") == true && b.at("exact_pixel_storage") == true)
                    pending["pixel_storage_changed"] = pending.at("storage_changed");
            }
            if (disabled)
                pending["write_executed"] = false;
            else if (const auto operation = pending.at("footprint").value("operation", std::string());
                     operation == "cpu_update" || operation == "cpu_map")
                pending["write_executed"] = pending.at("footprint").at("coverage") == "addressed";
            records.push_back(std::move(pending));
            pending = nullptr;
        }
    };
    replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); },
               [&](Id event, bool after, ID3D11DeviceContext *context, const auto &objects) {
                   Json footprint = nullptr;
                   if (!after && objects.contains(s.resource)) {
                       Com<ID3D11Resource> selectedResource;
                       check(objects.at(s.resource).As(&selectedResource), "Get native history resource");
                       const auto type = replay.frame().entry(event).type;
                       const auto outputs = boundOutputs(context, selectedResource.Get(), s,
                                                         type == 0x35 || type == 0x36, replay.featureLevel());
                       if (!outputs.empty())
                           footprint = {{"operation", "shader_submission"},
                                        {"coverage", "potential"},
                                        {"outputs", outputs}};
                   }
                   boundary(event, after, objects, footprint, "submission_after_binding");
               },
               [&](Id event, bool after, ID3D11DeviceContext *, const auto &objects) {
                   const auto type = replay.frame().entry(event).type;
                   if (isWritableCommand(type) || isDiscardRecord(type))
                       boundary(event, after, objects,
                                after ? Json(nullptr) : commandFootprint(replay, event, s, selected, maps),
                                "resource_command");
               });
    if (!pending.is_null())
        throw std::runtime_error("Incomplete native history boundary");
    return {{"schema", 2},
            {"kind", "pixel_event_history"},
            {"source", "native_dx11_replay"},
            {"selection",
             {{"resource", s.resource},
              {"x", s.x},
              {"y", s.y},
              {"slice", s.slice},
              {"mip", s.mip},
              {"layer", s.layer},
              {"sample", s.sample},
              {"typed_format", s.typedFormat ? Json(*s.typedFormat) : Json(nullptr)}}},
            {"records", std::move(records)},
            {"replay_count", 1},
            {"readback_count", reads},
            {"point_readback_count", pointReads},
            {"readback_bytes", bytesRead},
            {"fragment_history_available", false},
            {"notes",
             {"Snapshots describe replay storage at event boundaries, not individual fragments.",
              "A shader output binding is a potential footprint; pixel coverage and failure reasons remain "
              "unknown.",
              "Addressed command footprints do not prove GPU execution when predication applies.",
              "Undefined or missing initial contents are not reconstructed; definedness remains unknown.",
              "Compressed and packed groups retain raw bytes without claiming individual pixel changes.",
              "Decoded values use an explicit selection, an unambiguous command/bound view or a typed "
              "resource format.",
              "Unknown interpretations and unsupported encodings retain raw storage without invented numeric "
              "values.",
              "The decoding source distinguishes storage math from native GPU Load results; raw storage is "
              "retained."}}};
}
} // namespace flora
