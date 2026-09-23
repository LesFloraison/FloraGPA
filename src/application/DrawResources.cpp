#include "DrawResources.h"
#include "FrameOutput.h"
#include "ViewEdits.h"
#include "core/TextureStorage.h"
#include <QDir>
#include <QImage>
#include <QSaveFile>
#include <algorithm>
#include <set>

namespace flora {
using Json = nlohmann::json;
std::string ResourceRequestContext::cacheKey() const {
    return Json{{"capture", capture}, {"experiment", experiment}, {"device", device}, {"event", event}}
        .dump();
}
std::vector<DrawResourceBinding> drawResources(const Frame &capture, Id event, const ReplayOptions &options) {
    const auto &frame = effectiveFrame(capture, options);
    const auto work = frame.event(event);
    const auto state = effectiveBindings(frame, event, frame.state(work.state), options);
    std::vector<DrawResourceBinding> rows;
    const char *stages[] = {"VS", "HS", "DS", "GS", "PS", "CS"};
    auto add = [&](ResourceRole role, const std::string &kind, const std::string &stage, unsigned slot, Id id,
                   std::optional<unsigned> shaderStage = {}) {
        if (!id)
            return;
        DrawResourceBinding b;
        b.role = role;
        b.kind = kind;
        b.stage = stage;
        b.slot = slot;
        b.key =
            (role == ResourceRole::Input ? "in/" : "out/") + stage + "/" + kind + "/" + std::to_string(slot);
        b.image.event = event;
        b.image.boundary = role == ResourceRole::Input ? ImageBoundary::Before : ImageBoundary::After;
        const bool view = kind == "SRV" || kind == "RTV" || kind == "DSV" || kind == "UAV";
        b.image.view = view ? id : 0;
        b.image.resource = view ? 0 : id;
        try {
            Json desc = Json::object();
            if (view) {
                const auto info = describeView(frame, id);
                b.image.resource = info.at("resource").get<Id>();
                if (info.at("kind") != (kind == "SRV"   ? "srv"
                                        : kind == "RTV" ? "rtv"
                                        : kind == "DSV" ? "dsv"
                                                        : "uav"))
                    throw std::runtime_error("Binding view kind mismatch");
                desc = info.at("descriptor");
                b.image.format = desc.value("format", 0u);
                if (shaderStage) {
                    if (auto edits = options.srvEdits.find(event); edits != options.srvEdits.end())
                        if (auto edit = edits->second.find({*shaderStage, slot}); edit != edits->second.end())
                            desc = unpackView("srv", Bytes(reinterpret_cast<const uint8_t *>(&edit->second),
                                                           sizeof edit->second));
                }
            }
            const auto resource = frame.resource(b.image.resource);
            b.texture = resource.type >= 0x84 && resource.type <= 0x87;
            if (b.texture) {
                const auto t = textureInfo(resource);
                b.image.format = desc.value("format", t.format);
                b.image.mip = desc.value("most_detailed_mip", desc.value("mip_slice", 0u));
                b.image.layer = desc.value("first_array_slice", desc.value("first_2d_array_face", 0u));
                b.image.slice = desc.value("first_w_slice", 0u);
                if (!t.mips || b.image.mip >= t.mips || b.image.layer >= t.layers)
                    throw std::runtime_error("View subresource exceeds texture");
                b.width = std::max(1u, t.width >> b.image.mip);
                b.height = std::max(1u, t.height >> b.image.mip);
                b.samples = t.samples;
                const auto depth = std::max(1u, t.depth >> b.image.mip);
                auto end = [](uint32_t first, uint64_t count, uint32_t limit) {
                    if (first >= limit || !count)
                        throw std::runtime_error("Empty or invalid view range");
                    if (count == UINT32_MAX)
                        return limit;
                    if (count > uint64_t(limit - first))
                        throw std::runtime_error("View range exceeds texture");
                    return first + uint32_t(count);
                };
                b.mipEnd = end(b.image.mip, desc.value("mip_levels", 1u), t.mips);
                const uint64_t layers =
                    desc.contains("num_cubes")
                        ? uint64_t(desc.at("num_cubes").get<uint32_t>()) * 6
                        : (kind == "SRV" && desc.value("dimension", 0u) == 9 ? 6
                                                                             : desc.value("array_size", 1u));
                b.layerEnd = end(b.image.layer, layers, t.layers);
                b.sliceEnd = end(b.image.slice, desc.value("w_size", depth), depth);
            }
        } catch (const std::exception &e) {
            b.error = e.what();
        }
        rows.push_back(std::move(b));
    };
    for (unsigned stage = 0; stage < 6; ++stage)
        for (unsigned slot = 0; slot < 128; ++slot)
            add(ResourceRole::Input, "SRV", stages[stage], slot, state.stages[stage].srv[slot], stage);
    for (unsigned slot = 0; slot < 32; ++slot)
        add(ResourceRole::Input, "VB", "IA", slot, state.vb[slot]);
    add(ResourceRole::Input, "IB", "IA", 0, state.ib);
    for (unsigned stage = 0; stage < 6; ++stage)
        for (unsigned slot = 0; slot < 14; ++slot)
            add(ResourceRole::Input, "CB", stages[stage], slot, state.stages[stage].cb[slot]);
    for (unsigned slot = 0; slot < std::min({state.rtCount, state.omStart, 8u}); ++slot)
        add(ResourceRole::Output, "RTV", "OM", slot, state.rtv[slot]);
    add(ResourceRole::Output, "DSV", "OM", 0, state.dsv);
    for (unsigned slot = state.omStart; slot < std::min(state.rtCount, 64u); ++slot)
        add(ResourceRole::Output, "UAV", "OM", slot, slot < 8 ? state.rtv[slot] : state.omExtended[slot - 8]);
    for (unsigned slot = 0; slot < 64; ++slot)
        add(ResourceRole::Output, "UAV", "CS", slot,
            slot < 8 ? state.csUav[slot] : state.csExtended[slot - 8]);
    return rows;
}
Json drawResourceJson(const DrawResourceBinding &b) {
    const auto &s = b.image;
    return {{"key", b.key},
            {"role", b.role == ResourceRole::Input ? "input" : "output"},
            {"kind", b.kind},
            {"stage", b.stage},
            {"slot", b.slot},
            {"event", s.event},
            {"view", s.view},
            {"resource", s.resource},
            {"format", s.format},
            {"mip", s.mip},
            {"layer", s.layer},
            {"slice", s.slice},
            {"sample", s.sample ? Json(*s.sample) : Json(nullptr)},
            {"boundary", s.boundary == ImageBoundary::Before ? "before" : "after"},
            {"texture", b.texture},
            {"width", b.width},
            {"height", b.height},
            {"samples", b.samples},
            {"mip_end", b.mipEnd},
            {"layer_end", b.layerEnd},
            {"slice_end", b.sliceEnd},
            {"error", b.error}};
}
Json drawResourceInventory(const std::vector<DrawResourceBinding> &bindings, Id event) {
    Json rows = Json::array();
    std::set<Id> textures;
    unsigned inputs = 0, rt = 0, ds = 0, uav = 0;
    for (const auto &b : bindings) {
        rows.push_back(drawResourceJson(b));
        if (b.role == ResourceRole::Input && b.kind == "SRV") {
            ++inputs;
            if (b.texture)
                textures.insert(b.image.resource);
        }
        rt += b.kind == "RTV";
        ds += b.kind == "DSV";
        uav += b.kind == "UAV";
    }
    return {{"completed", true},
            {"event", event},
            {"bindings", rows},
            {"counts",
             {{"input_textures", textures.size()},
              {"input_bindings", inputs},
              {"rtv", rt},
              {"dsv", ds},
              {"uav", uav}}},
            {"source", "effective_capture_bindings"},
            {"note", "Bound resources; bindings do not prove sampling or writes."}};
}
Json exportDrawResources(const Frame &frame, Id event, ReplayOptions options, const Json &request,
                         const std::filesystem::path &directory) {
    if (!request.is_object() || request.size() != 1 || !request.contains("previews") ||
        !request.at("previews").is_array() || request.at("previews").size() > 16)
        throw std::runtime_error("Expected at most 16 preview binding keys");
    auto rows = drawResources(frame, event, options);
    auto result = drawResourceInventory(rows, event);
    std::set<std::string> keys;
    for (const auto &key : request.at("previews")) {
        const auto value = key.get<std::string>();
        if (std::none_of(rows.begin(), rows.end(), [&](const auto &b) { return b.key == value; }))
            throw std::runtime_error("Unknown preview binding key");
        keys.insert(value);
    }
    options.until = event;
    options.before = false;
    options.timings = false;
    if (!keys.empty()) {
        Replay replay(frame, options);
        bool observed[2]{};
        std::map<std::string, std::string> images;
        replay.run({}, [&](Id id, bool after, auto *, const auto &) {
            if (id != event)
                return;
            observed[after] = true;
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto &b = rows[i];
                if (!keys.contains(b.key) || after != (b.role == ResourceRole::Output))
                    continue;
                auto &r = result["bindings"][i];
                if (!b.error.empty() || !b.texture)
                    continue;
                try {
                    const auto cache = std::to_string(after) + ":" + std::to_string(b.image.resource) + ":" +
                                       std::to_string(b.image.format) + ":" + std::to_string(b.image.mip) +
                                       ":" + std::to_string(b.image.layer) + ":" +
                                       std::to_string(b.image.slice) + ":" + b.kind;
                    if (auto found = images.find(cache); found != images.end()) {
                        r["preview"] = found->second;
                        continue;
                    }
                    Image image;
                    if (b.kind == "RTV" || b.kind == "DSV") {
                        FrameDisplayOptions display;
                        display.layer = b.image.slice ? b.image.slice : b.image.layer;
                        image = readFrameOutput(replay, b.image.resource, display, b.image.view,
                                                b.kind == "DSV" ? "depth" : "")
                                    .image;
                    } else {
                        auto resource = replay.frame().resource(b.image.resource);
                        std::vector<uint8_t> bytes;
                        if (b.samples > 1) {
                            auto storage = replay.readMsaa(b.image.resource, {}, b.image.format);
                            resource = std::move(storage.resource);
                            bytes = std::move(storage.bytes);
                        } else
                            bytes = replay.readTexture(b.image.resource);
                        image = replay.previewTextureStorage(resource, bytes, b.image.mip, b.image.layer,
                                                             b.image.slice, 0, 1, "rgba", b.image.format);
                    }
                    if (!image.width || !image.height || image.width > INT_MAX / 4 ||
                        image.height > INT_MAX ||
                        image.rgba.size() != uint64_t(image.width) * image.height * 4)
                        throw std::runtime_error("Invalid thumbnail dimensions");
                    const auto name = "thumb-" + std::to_string(i) + ".png";
                    QImage pixels(image.rgba.data(), int(image.width), int(image.height),
                                  int(image.width * 4), QImage::Format_RGBA8888);
                    if (!pixels.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                             .save(QString::fromStdWString((directory / name).wstring())))
                        throw std::runtime_error("Cannot save thumbnail");
                    r["preview"] = name;
                    images[cache] = name;
                } catch (const std::exception &e) {
                    r["preview_error"] = e.what();
                }
            }
        });
        if (!observed[0] || !observed[1])
            throw std::runtime_error("Draw preview boundaries not observed");
    }
    QSaveFile file(QString::fromStdWString((directory / "report.json").wstring()));
    const auto text = result.dump(2);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()) || !file.commit())
        throw std::runtime_error("Cannot save draw resource report");
    return result;
}
} // namespace flora
