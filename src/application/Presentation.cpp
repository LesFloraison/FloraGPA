#include "FrameOutput.h"
#include <algorithm>
#include <charconv>
namespace flora {
using Json = nlohmann::json;
namespace {
Json nullable(Id id) { return id ? Json(id) : Json(nullptr); }
bool compatible(const Json &a, const Json &b) {
    for (size_t i = 0; i < 7; ++i)
        if (a.at(i) != b.at(i))
            return false;
    return true;
}
Json boundOutputs(const Replay &replay) {
    Json result = Json::array();
    replay.inspectNativeState([&](ID3D11DeviceContext *context, const auto &objects) {
        std::array<Com<ID3D11RenderTargetView>, 8> owned;
        std::array<ID3D11RenderTargetView *, 8> rt{};
        Com<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargetsAndUnorderedAccessViews(8, rt.data(), &depth, 0, 0, nullptr);
        for (unsigned i = 0; i < 8; ++i)
            owned[i].Attach(rt[i]);
        for (unsigned slot = 0; slot < 9; ++slot) {
            Json item{{"slot", slot < 8 ? Json(slot) : Json(nullptr)},
                      {"target", slot < 8 ? "rt" + std::to_string(slot) : "depth"},
                      {"view", nullptr},
                      {"resource", nullptr},
                      {"status", "unbound"}};
            IUnknown *native = slot < 8 ? static_cast<IUnknown *>(rt[slot]) : depth.Get();
            if (replay.unresolvedOutputs()) {
                item["status"] = "unresolved";
                item["reason"] = "Missing captured output bindings require a later full snapshot";
            } else if (native) {
                Json ids = Json::array();
                for (const auto &[id, object] : objects) {
                    const auto it = replay.frame().entries().find(id);
                    if (it == replay.frame().entries().end() || it->second.category != 5 ||
                        it->second.type != (slot < 8 ? 0x8d : 0x8e))
                        continue;
                    Com<IUnknown> left, right;
                    check(object.As(&left), "Identify replay view");
                    check(native->QueryInterface(IID_PPV_ARGS(&right)), "Identify bound view");
                    if (left.Get() == right.Get())
                        ids.push_back(id);
                }
                if (ids.size() == 1) {
                    item["status"] = "bound";
                    item["view"] = ids[0];
                    Reader record(replay.frame().payload(ids[0].get<Id>()));
                    record.skip(16);
                    item["resource"] = record.read<Id>();
                } else {
                    item["status"] = "unresolved";
                    item["reason"] = "Native output view identity is unknown or ambiguous";
                    item["candidate_views"] = ids;
                }
            }
            result.push_back(std::move(item));
        }
    });
    Json colors = Json::array();
    for (size_t i = 0; i < 8; ++i)
        colors.push_back(result[i]);
    return {{"colors", colors}, {"depth", result[8]}};
}
} // namespace
std::string parseOutputTarget(const std::string &target) {
    if (target == "auto" || target == "present" || target == "depth" || target == "stencil" ||
        (target.size() == 3 && target.substr(0, 2) == "rt" && target[2] >= '0' && target[2] <= '7'))
        return target;
    if (target.starts_with("swap:") && target.size() >= 6 && target.size() <= 25 && target[5] >= '1' &&
        target[5] <= '9') {
        Id id = 0;
        auto result = std::from_chars(target.data() + 5, target.data() + target.size(), id);
        if (result.ec == std::errc{} && result.ptr == target.data() + target.size())
            return target;
    }
    throw std::runtime_error(
        "Unknown frame output target; use auto, present, rt0..rt7, depth, stencil or swap:<resource ID>");
}
Json presentationInventory(const Frame &frame) {
    Json snapshots = Json::array(), textures = Json::array(), issues = Json::array(), chains = Json::array();
    for (const auto &[id, entry] : frame.entries()) {
        if (entry.category != 5 || (entry.type != 0x85 && entry.type != 0x87))
            continue;
        try {
            const auto r = frame.resource(id);
            const auto parent = frame.entries().find(r.device);
            if (parent == frame.entries().end() || parent->second.category != 5 ||
                parent->second.type != 0x38) {
                if (entry.type == 0x87)
                    issues.push_back({{"resource", id}, {"reason", "Missing swap-chain parent"}});
                continue;
            }
            if (parent->second.size != 88)
                throw std::runtime_error("Unsupported swap-chain record size");
            Json item{{"resource", id}, {"swap_chain", r.device}, {"desc", r.desc}};
            if (entry.type == 0x87) {
                if (r.original != UINT64_MAX - 1)
                    throw std::runtime_error("Unexpected capture framebuffer identity");
                snapshots.push_back(item);
            } else
                textures.push_back(item);
        } catch (const std::exception &error) {
            issues.push_back({{"resource", id}, {"reason", error.what()}});
        }
    }
    Json target;
    if (snapshots.size() == 1 && issues.empty()) {
        const auto &marker = snapshots[0];
        Json matching = Json::array();
        for (const auto &t : textures)
            if (t["swap_chain"] == marker["swap_chain"])
                matching.push_back(t);
        if (matching.size() == 1 && compatible(matching[0]["desc"], marker["desc"]))
            target = {{"texture", matching[0]["resource"]},
                      {"swap_chain", marker["swap_chain"]},
                      {"reference_framebuffer", marker["resource"]},
                      {"provenance", "captured_framebuffer_and_live_texture_share_swap_chain"},
                      {"reference_pixels_used", false}};
        else
            issues.push_back(
                {{"reason", "Live swap-chain backbuffer identity is missing, ambiguous or incompatible"}});
    } else if (!snapshots.empty())
        issues.push_back({{"reason", "Capture framebuffer identity is ambiguous or invalid"}});
    for (const auto &[id, entry] : frame.entries()) {
        if (entry.category != 5 || entry.type != 0x38)
            continue;
        Json matching = Json::array(), markers = Json::array(), candidate, reason;
        for (const auto &t : textures)
            if (t["swap_chain"] == id)
                matching.push_back(t);
        for (const auto &t : snapshots)
            if (t["swap_chain"] == id)
                markers.push_back(t);
        if (entry.size != 88)
            reason = "Unsupported swap-chain record size";
        else if (matching.size() != 1)
            reason = "Live swap-chain backbuffer identity is missing or ambiguous";
        else if (markers.size() > 1 || std::any_of(markers.begin(), markers.end(), [&](const auto &marker) {
                     return !compatible(marker["desc"], matching[0]["desc"]);
                 }))
            reason = "Capture framebuffer metadata is ambiguous or incompatible";
        else {
            bool invalid = false;
            for (const auto &issue : issues) {
                if (!issue.contains("resource"))
                    continue;
                auto rid = issue["resource"].get<Id>();
                if (frame.entry(rid).type != 0x87)
                    continue;
                try {
                    invalid |= frame.resource(rid).device == id;
                } catch (const std::exception &) {
                    invalid = true;
                }
            }
            if (invalid)
                reason = "Invalid capture framebuffer metadata";
            else
                candidate = {
                    {"texture", matching[0]["resource"]},
                    {"swap_chain", id},
                    {"reference_framebuffer", markers.empty() ? Json(nullptr) : markers[0]["resource"]},
                    {"provenance", "live_texture_swap_chain_parent"},
                    {"reference_pixels_used", false}};
        }
        chains.push_back({{"swap_chain", id},
                          {"selector", "swap:" + std::to_string(id)},
                          {"target", candidate},
                          {"reason", reason}});
    }
    return {{"target", target},
            {"reference_framebuffers", snapshots},
            {"live_backbuffers", textures},
            {"issues", issues},
            {"swap_chains", chains}};
}
Json selectFrameOutput(const Replay &replay, const std::string &requested) {
    parseOutputTarget(requested);
    auto info = presentationInventory(replay.frame());
    auto target = info["target"];
    const bool explicitSwap = requested.starts_with("swap:");
    if (explicitSwap) {
        target = nullptr;
        Json reason = "Swap-chain resource is absent";
        for (const auto &chain : info["swap_chains"])
            if (chain["selector"] == requested) {
                target = chain["target"];
                reason = chain["reason"];
            }
        info["automatic_target"] = info["target"];
        info["target"] = target;
        if (target.is_null())
            info["issues"].push_back({{"swap_chain", std::stoull(requested.substr(5))}, {"reason", reason}});
    }
    auto bindings = boundOutputs(replay);
    info.update(Json{{"requested_target", requested},
                     {"color_slots", bindings["colors"]},
                     {"depth_slot", bindings["depth"]},
                     {"binding_provenance", "native_OMGetRenderTargetsAndUnorderedAccessViews"},
                     {"boundary", replay.options().until ? "api_event" : "capture_end"},
                     {"event", nullable(replay.lastEvent())},
                     {"navigation_event", nullable(replay.lastWorkEvent())}});
    auto selected = [&](Json values) {
        auto result = info;
        result.update(values);
        return result;
    };
    if (requested == "depth" || requested == "stencil" || requested.starts_with("rt")) {
        const bool depth = requested == "depth" || requested == "stencil";
        auto slot = depth ? bindings["depth"] : bindings["colors"][requested[2] - '0'];
        if (requested == "stencil" && slot["status"] == "bound") {
            Reader r(replay.frame().payload(slot["view"].get<Id>()));
            r.skip(24);
            const auto format = r.read<uint32_t>();
            if (format != 20 && format != 45)
                return selected({{"resource", nullptr},
                                 {"view", nullptr},
                                 {"aspect", "stencil"},
                                 {"kind", "unavailable_stencil_plane"}});
        }
        const std::string kind = slot["status"] == "bound"
                                     ? (depth ? "bound_depth_stencil_target" : "bound_color_target")
                                 : slot["status"] == "unbound" ? "unbound_output_slot"
                                                               : "unresolved_output_slot";
        Json values{{"resource", slot["resource"]}, {"view", slot["view"]}, {"kind", kind}};
        if (depth)
            values["aspect"] = requested;
        else
            values["slot"] = slot["slot"];
        return selected(values);
    }
    if (!target.is_null() &&
        (explicitSwap || requested == "present" || !replay.options().until || !replay.lastOutputResource())) {
        const auto resource = target["texture"].get<Id>();
        if (replay.resourceAvailable(resource))
            return selected({{"resource", resource}, {"view", 0}, {"kind", "swap_chain_backbuffer"}});
        info["issues"].push_back({{"resource", resource},
                                  {"reason", "Backbuffer has no replayed storage or captured initial data"}});
    }
    if (explicitSwap || requested == "present")
        return selected(
            {{"resource", nullptr}, {"view", nullptr}, {"kind", "unavailable_presentation_target"}});
    return selected({{"resource", nullable(replay.lastOutputResource())},
                     {"view", nullable(replay.lastOutputView())},
                     {"kind", replay.lastOutputResource() ? "draw_color_target" : "no_draw_color_target"}});
}
} // namespace flora
