#include "TextureInspector.h"
#include "ApiCommands.h"
#include "PlanarWrites.h"
#include "core/TextureStorage.h"
#include <QDir>
#include <QImage>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
namespace flora {
using Json = nlohmann::json;
namespace {
Json infoJson(const Resource &r) {
    const auto i = textureInfo(r);
    return {{"width", i.width},         {"height", i.height},   {"depth", i.depth},
            {"mips", i.mips},           {"layers", i.layers},   {"format", i.format},
            {"dimension", i.dimension}, {"samples", i.samples}, {"misc", r.desc.back()}};
}
Json subJson(const TextureSubresource &s) {
    return {{"mip", s.mip},       {"layer", s.layer},        {"width", s.width},
            {"height", s.height}, {"depth", s.depth},        {"offset", s.offset},
            {"size", s.size},     {"row_pitch", s.rowPitch}, {"slice_pitch", s.slicePitch}};
}
Json planeJson(const TexturePlane &p, uint32_t format) {
    return {{"name", p.name},
            {"format", p.format},
            {"width", p.width},
            {"height", p.height},
            {"row_pitch", p.rowPitch},
            {"offset", p.offset},
            {"size", p.size},
            {"channels", p.name == "y" ? Json::array({"Y"}) : Json::array({"U", "V"})},
            {"color_conversion", false},
            {"storage_format", format == 103   ? "NV12"
                               : format == 104 ? "P010"
                                               : "P016"}};
}
std::vector<uint8_t> range(Bytes bytes, uint64_t offset, uint64_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("Texture export range exceeds storage");
    auto selected = bytes.subspan(size_t(offset), size_t(size));
    return {selected.begin(), selected.end()};
}
Json inspectionEvent(const Frame &frame, Id id) {
    if (!isDraw(frame.entry(id).type))
        return inspectCommand(frame, id);
    const auto e = frame.event(id);
    std::vector<const char *> names;
    unsigned slot = 0;
    switch (e.type) {
    case 0x35:
        names = {"x", "y", "z"};
        slot = 41;
        break;
    case 0x36:
        names = {"offset"};
        slot = 42;
        break;
    case 0x37:
        names = {"vertex_count", "start_vertex"};
        slot = 13;
        break;
    case 0x38:
        slot = 13;
        break;
    case 0x39:
        names = {"index_count", "start_index", "base_vertex"};
        slot = 12;
        break;
    case 0x3a:
        names = {"index_count", "instance_count", "start_index", "base_vertex", "start_instance"};
        slot = 20;
        break;
    case 0x3b:
        names = {"offset"};
        slot = 39;
        break;
    case 0x3c:
        names = {"vertex_count", "instance_count", "start_vertex", "start_instance"};
        slot = 21;
        break;
    case 0x3d:
        names = {"offset"};
        slot = 40;
        break;
    }
    Json parameters = Json::object();
    if (e.type == 0x36 || e.type == 0x3b || e.type == 0x3d)
        parameters["argument_buffer"] = e.argumentBuffer;
    for (size_t i = 0; i < names.size(); ++i)
        parameters[names[i]] =
            std::string(names[i]) == "base_vertex" ? Json(int32_t(e.args.at(i))) : Json(e.args.at(i));
    return {{"id", e.id},   {"name", commandName(e.type)}, {"state_id", e.state}, {"context", e.context},
            {"slot", slot}, {"parameters", parameters}};
}
} // namespace
TextureInspection inspectTexture(Replay &replay, Id id, const TextureInspectionOptions &o) {
    if (!std::isfinite(o.low) || !std::isfinite(o.high) || !std::isfinite(o.high - o.low) || o.high <= o.low)
        throw std::runtime_error("Display range must be finite and increasing");
    if (o.channel != "rgba" && o.channel != "rgb" && o.channel != "r" && o.channel != "g" &&
        o.channel != "b" && o.channel != "a")
        throw std::runtime_error("Invalid display channel");
    const auto &frame = replay.frame();
    auto resource = frame.resource(id);
    const auto source = resource;
    const auto sourceInfo = textureInfo(source);
    const auto event = replay.options().until;
    const bool before = replay.options().before;
    TextureInspection out;
    Json msaa = nullptr;
    Json view = {{"mip", o.mip},
                 {"layer", o.layer},
                 {"slice", o.slice},
                 {"channel", o.channel},
                 {"low", o.low},
                 {"high", o.high},
                 {"typed_format", o.typedFormat ? Json(*o.typedFormat) : Json(nullptr)},
                 {"plane", o.plane}};
    uint32_t mip = o.mip, layer = o.layer, slice = o.slice;
    const bool luma = !event && (sourceInfo.format == 104 || sourceInfo.format == 105);
    if (!event) {
        if (sourceInfo.samples != 1)
            throw std::runtime_error("Captured MSAA data is not per-sample initial storage; select an event");
        if (!resource.data)
            throw std::runtime_error("Texture has no captured initial bytes; select an event");
        const auto captured = frame.data(resource.data);
        if (luma) {
            const auto recovered =
                capturedLuma(resource, captured, o.mip, o.layer, o.slice, o.plane, o.typedFormat);
            resource = recovered.resource;
            out.storage = recovered.bytes;
            out.captured.assign(captured.begin(), captured.end());
            out.metadata = {
                {"recovered_luma_only", true},
                {"storage_scope", "captured_luma_only"},
                {"uv_available", false},
                {"source_texture", infoJson(source)},
                {"source_layout",
                 {{"source_offset", recovered.sourceOffset},
                  {"source_row_pitch", recovered.sourceRowPitch},
                  {"source_layer_size", recovered.sourceLayerSize},
                  {"source_layer", o.layer},
                  {"source_mip", 0},
                  {"export_row_pitch", uint64_t(sourceInfo.width) * 2},
                  {"export_offset", 0}}},
                {"capture_sha256", sha256(captured)},
                {"capture_plane_notice",
                 "Only captured Y rows were recovered. UV is unavailable. luma.dds contains R16 Y only; "
                 "capture_data.bin preserves the complete original GenData, including padding."}};
            mip = layer = slice = 0;
        } else
            out.storage.assign(captured.begin(), captured.end());
    } else {
        if (frame.entry(event).category != 7)
            throw std::runtime_error("Texture inspection requires an API command boundary");
        auto read = [&] {
            if (sourceInfo.samples > 1) {
                auto result = replay.readMsaa(id, o.sample,
                                              o.typedFormat && *o.typedFormat
                                                  ? *o.typedFormat
                                                  : defaultTextureFormat(sourceInfo.format));
                resource = std::move(result.resource);
                out.storage = std::move(result.bytes);
                msaa = {
                    {"mode", result.mode},
                    {"source_samples", sourceInfo.samples},
                    {"source_format", sourceInfo.format},
                    {"output_format", textureInfo(resource).format},
                    {"selected_sample", o.sample ? Json(*o.sample) : Json(nullptr)},
                    {"depth_stencil", result.depthStencil},
                    {"initial_samples_reconstructed", false},
                    {"initialization_note",
                     "Pre-capture per-sample contents are not reconstructed. Regions not written by recorded "
                     "commands or explicit experiment assets may differ from the captured application."}};
                if (result.integerBits)
                    msaa["storage_path"] = "integer_bits";
                if (result.canonicalX)
                    msaa["unused_x_byte"] = 0;
            } else {
                if (o.sample)
                    throw std::runtime_error("Sample selection requires an MSAA texture");
                out.storage = replay.readTexture(id);
            }
        };
        if (before && isDraw(frame.entry(event).type))
            replay.inspectEventInputs(event, read);
        else
            read();
    }
    const auto subs = textureSubresources(resource);
    if (out.storage.size() != subs.back().offset + subs.back().size)
        throw std::runtime_error("Texture export storage length mismatch");
    const auto selected = std::find_if(subs.begin(), subs.end(),
                                       [&](const auto &s) { return s.mip == mip && s.layer == layer; });
    if (selected == subs.end() || slice >= selected->depth)
        throw std::runtime_error("Texture export mip/layer/slice out of bounds");
    out.subresource = range(out.storage, selected->offset, selected->size);
    out.dds = textureDds(resource, out.storage);
    out.metadata.update(infoJson(resource));
    out.metadata["resource_id"] = id;
    out.metadata["value_time"] = event ? (before ? "before_event" : "after_event") : "capture_initial";
    out.metadata["preview_options"] = view;
    out.metadata["sha256"] = sha256(out.storage);
    out.metadata["subresources"] = Json::array();
    for (const auto &sub : subs)
        out.metadata["subresources"].push_back(subJson(sub));
    auto previewResource = resource;
    auto previewBytes = Bytes(out.storage);
    if (luma) {
        TexturePlane p{"y",
                       textureInfo(resource).format,
                       sourceInfo.width,
                       sourceInfo.height,
                       sourceInfo.width * 2,
                       0,
                       out.storage.size()};
        auto value = planeJson(p, sourceInfo.format);
        value.erase("offset");
        out.metadata["selected_plane"] = value;
        out.plane = out.storage;
    } else {
        out.metadata["selected_subresource"] = subJson(*selected);
        if (const auto plane = texturePlane(resource, mip, layer, o.plane, o.typedFormat)) {
            out.plane = range(out.storage, plane->offset, plane->size);
            out.metadata["selected_plane"] = planeJson(*plane, sourceInfo.format);
            out.metadata["capture_plane_notice"] =
                "Legacy GPA captures may omit chroma data. Export preserves the supplied storage; zero UV "
                "does not prove that the original application used zero UV.";
            previewResource.type = 0x85;
            previewResource.desc = {plane->width, plane->height, 1, 1, plane->format, 1, 0, 0, 8, 0, 0};
            previewBytes = out.plane;
            mip = layer = slice = 0;
        }
    }
    if (o.preview)
        out.image = replay.previewTextureStorage(
            previewResource, previewBytes, mip, layer, slice, o.low, o.high, o.channel,
            out.metadata.contains("selected_plane") ? std::optional(textureInfo(previewResource).format)
                                                    : o.typedFormat);
    out.metadata["export_files"] = {{".dds", luma ? "luma.dds" : "texture.dds"},
                                    {".bin", luma ? "plane.bin" : "subresource.bin"}};
    if (o.preview)
        out.metadata["export_files"][".png"] = "preview.png";
    if (event) {
        out.metadata["event"] = inspectionEvent(frame, event);
        const bool snapshot = isDraw(frame.entry(event).type);
        out.metadata["pipeline_snapshot_available"] = snapshot;
        out.metadata["output_bindings"] = Json::array();
        bool matching = false;
        if (snapshot) {
            auto command = frame.event(event);
            auto state = frame.state(command.state);
            for (const auto &b : textureOutputBindings(frame, command, state, id)) {
                out.metadata["output_bindings"].push_back(
                    {{"role", b.role},
                     {"stage", b.stage},
                     {"slot", b.slot < 0 ? Json(nullptr) : Json(b.slot)},
                     {"view", b.view},
                     {"format", b.format},
                     {"dimension", b.dimension},
                     {"mip", b.mip},
                     {"first_layer", b.firstLayer},
                     {"layer_count", b.layerCount},
                     {"dsv_flags", b.dsvFlags},
                     {"depth_slices", b.depthSlices ? Json{{"first_slice", (*b.depthSlices)[0]},
                                                           {"slice_count", (*b.depthSlices)[1]}}
                                                    : Json(nullptr)}});
                matching |= b.contains(o.mip, o.layer);
            }
        }
        if (sourceInfo.samples > 1) {
            try {
                if (!o.sample || *o.sample >= sourceInfo.samples || sourceInfo.samples > 32)
                    throw std::runtime_error("Explicit sample required");
                msaaEditEncoding(source, o.typedFormat);
            } catch (const std::exception &) {
                matching = false;
            }
        }
        out.metadata["output_edit_supported"] = matching;
        out.metadata["output_edit_effect"] =
            !matching                ? Json(nullptr)
            : sourceInfo.samples > 1 ? Json("Patch this sample in the selected array layer before the event; "
                                            "other samples are retained.")
                                     : Json("Patch the complete subresource before this event; submitted GPU "
                                            "writes and unoverwritten patch bytes persist.");
        out.metadata["msaa"] = msaa;
        out.metadata["msaa_initial_data_not_applied"] = replay.ignoredMsaaInitial();
        out.metadata["planar_writes"] = planarWriteReport(replay);
        out.metadata["planar_write_notice"] = planarWriteNotice(replay);
    }
    return out;
}
void exportTextureInspection(const TextureInspection &i, const std::filesystem::path &directory) {
    const auto root = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create texture export directory");
    const auto save = [&](const QString &name, Bytes bytes) {
        QSaveFile file(root + '/' + name);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                qint64(bytes.size()) ||
            !file.commit())
            throw std::runtime_error("Cannot save texture asset");
    };
    const bool luma = i.metadata.value("recovered_luma_only", false);
    save(luma ? "luma.dds" : "texture.dds", i.dds);
    if (luma)
        save("capture_data.bin", i.captured);
    else {
        save("texture.bin", i.storage);
        save("subresource.bin", i.subresource);
    }
    if (i.metadata.contains("selected_plane"))
        save("plane.bin", i.plane);
    if (i.image) {
        const auto &image = *i.image;
        QImage preview(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4),
                       QImage::Format_RGBA8888);
        if (!preview.save(root + "/preview.png"))
            throw std::runtime_error("Cannot save texture preview");
    }
    const auto text = i.metadata.dump(2) + "\n";
    save("texture.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
} // namespace flora
