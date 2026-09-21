#include "Backend.h"
#include "Api.h"
#include "application/RdcEvents.h"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
pRENDERDOC_AllocArrayMem allocate{};
pRENDERDOC_FreeArrayMem release{};
HMODULE library{};
bool used{};
template <typename T> T symbol(const char *name) {
    const auto result = GetProcAddress(library, name);
    if (!result)
        throw std::runtime_error(std::string("Missing RenderDoc entry point: ") + name);
    return reinterpret_cast<T>(result);
}
using Json = nlohmann::json;
std::string text(const rdcstr &value) { return std::string(value.c_str(), value.size()); }
std::filesystem::path path(const Json &value) {
    const auto s = value.get<std::string>();
    return std::filesystem::path(reinterpret_cast<const char8_t *>(s.c_str()));
}
uint64_t id(ResourceId value) {
    static_assert(sizeof(value) == sizeof(uint64_t));
    uint64_t result{};
    memcpy(&result, &value, sizeof(result));
    return result;
}
std::string resourceName(ResourceId value) { return "ResourceId::" + std::to_string(id(value)); }
uint64_t number(const Json &v) {
    if (!(v.is_number_unsigned() || v.is_number_integer()) || v < 0)
        throw std::runtime_error("Expected an unsigned integer");
    return v.get<uint64_t>();
}
uint32_t index(const Json &job, const char *key, uint32_t fallback = 0) {
    if (!job.contains(key) || job.at(key).is_null())
        return fallback;
    const auto value = number(job.at(key));
    if (value > UINT32_MAX)
        throw std::runtime_error(std::string("Index exceeds uint32: ") + key);
    return uint32_t(value);
}
Json real(float value) {
    if (std::isfinite(value))
        return double(value);
    return std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
}
Json pixel(const PixelValue &value) {
    Json f = Json::array(), u = Json::array(), i = Json::array();
    for (size_t k = 0; k < 4; ++k) {
        f.push_back(real(value.floatValue[k]));
        u.push_back(value.uintValue[k]);
        i.push_back(value.intValue[k]);
    }
    return {{"floatValue", f}, {"uintValue", u}, {"intValue", i}};
}
Json modification(const ModificationValue &value) {
    return {{"col", pixel(value.col)}, {"depth", real(value.depth)}, {"stencil", value.stencil}};
}
Json record(const PixelModification &v) {
    return {{"eventId", v.eventId},
            {"fragIndex", v.fragIndex},
            {"primitiveID", v.primitiveID},
            {"directShaderWrite", v.directShaderWrite},
            {"unboundPS", v.unboundPS},
            {"preMod", modification(v.preMod)},
            {"postMod", modification(v.postMod)},
            {"shaderOut", modification(v.shaderOut)},
            {"sampleMasked", v.sampleMasked},
            {"backfaceCulled", v.backfaceCulled},
            {"depthClipped", v.depthClipped},
            {"depthBoundsFailed", v.depthBoundsFailed},
            {"viewClipped", v.viewClipped},
            {"scissorClipped", v.scissorClipped},
            {"shaderDiscarded", v.shaderDiscarded},
            {"depthTestFailed", v.depthTestFailed},
            {"stencilTestFailed", v.stencilTestFailed},
            {"predicationSkipped", v.predicationSkipped}};
}
std::string flagsText(ActionFlags flags) {
    static const char *names[] = {
        "Clear",          "Drawcall",          "Dispatch",  "MeshDispatch", "CmdList",
        "SetMarker",      "PushMarker",        "PopMarker", "Present",      "MultiAction",
        "Copy",           "Resolve",           "GenMips",   "PassBoundary", "DispatchRay",
        "BuildAccStruct", "Indexed",           "Instanced", "Auto",         "Indirect",
        "ClearColor",     "ClearDepthStencil", "BeginPass", "EndPass",      "CommandBufferBoundary"};
    std::string result = "ActionFlags.";
    const auto bits = uint32_t(flags);
    if (!bits)
        return result + "NoFlags";
    if (bits >> 25)
        throw std::runtime_error("Unknown RenderDoc action flags");
    bool first = true;
    for (int bit = 24; bit >= 0; --bit) {
        if (!(bits & (1U << bit)))
            continue;
        if (!first)
            result += "|";
        result += names[bit];
        first = false;
    }
    return result;
}
struct Shutdown {
    template <typename T> void operator()(T *p) const {
        if (p)
            p->Shutdown();
    }
};
void checked(ResultDetails status) {
    if (!status.OK())
        throw std::runtime_error(status.internal_msg
                                     ? text(*status.internal_msg)
                                     : "RenderDoc error " + std::to_string(uint32_t(status.code)));
}
class Session {
    bool initialized_{};

  public:
    std::unique_ptr<IReplayController, Shutdown> controller;
    std::string version, commit;
    explicit Session(const Json &job) {
        if (used)
            throw std::runtime_error("Only one RenderDoc job is allowed per process");
        used = true;
        const auto chosen = std::filesystem::canonical(path(job.at("renderdoc")));
        library = LoadLibraryExW(chosen.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library)
            throw std::runtime_error("Cannot load selected RenderDoc replay library");
        wchar_t actual[32768]{};
        const auto count = GetModuleFileNameW(library, actual, 32768);
        if (!count || count >= 32768 || !std::filesystem::equivalent(chosen, actual))
            throw std::runtime_error("A different RenderDoc library is already loaded");
        version = symbol<decltype(&RENDERDOC_GetVersionString)>("RENDERDOC_GetVersionString")();
        if (version != "1.45" || !symbol<decltype(&RENDERDOC_IsReleaseBuild)>("RENDERDOC_IsReleaseBuild")())
            throw std::runtime_error("Native replay requires the RenderDoc 1.45 release ABI");
        commit = symbol<decltype(&RENDERDOC_GetCommitHash)>("RENDERDOC_GetCommitHash")();
        allocate = symbol<pRENDERDOC_AllocArrayMem>("RENDERDOC_AllocArrayMem");
        release = symbol<pRENDERDOC_FreeArrayMem>("RENDERDOC_FreeArrayMem");
        const auto open = symbol<decltype(&RENDERDOC_OpenCaptureFile)>("RENDERDOC_OpenCaptureFile");
        const auto initialize = symbol<decltype(&RENDERDOC_InitialiseReplay)>("RENDERDOC_InitialiseReplay");
        symbol<decltype(&RENDERDOC_ShutdownReplay)>("RENDERDOC_ShutdownReplay");
        try {
            initialize(GlobalEnvironment{}, {});
            initialized_ = true;
            std::unique_ptr<ICaptureFile, Shutdown> file(open());
            if (!file)
                throw std::runtime_error("RenderDoc did not create a capture handle");
            const auto filename = std::filesystem::canonical(path(job.at("capture"))).u8string();
            checked(file->OpenFile(reinterpret_cast<const char *>(filename.c_str()), "", {}));
            auto opened = file->OpenCapture(ReplayOptions{}, {});
            controller.reset(opened.second);
            checked(opened.first);
            if (!controller)
                throw std::runtime_error("RenderDoc did not create a replay controller");
            if (controller->GetAPIProperties().pipelineType != GraphicsAPI::D3D11)
                throw std::runtime_error("Pixel history requires a DX11 independent replay capture");
        } catch (...) {
            close();
            throw;
        }
    }
    void close() noexcept {
        controller.reset();
        if (initialized_) {
            symbol<decltype(&RENDERDOC_ShutdownReplay)>("RENDERDOC_ShutdownReplay")();
            initialized_ = false;
        }
    }
    ~Session() { close(); }
};
struct Events {
    Json native = Json::object();
    std::map<uint32_t, const SDObject *> chunks;
    std::set<uint32_t> actions;
    size_t visited{};
    Json walk(const rdcarray<ActionDescription> &nodes, const SDFile &file, unsigned depth = 0) {
        if (depth > 128)
            throw std::runtime_error("RenderDoc action tree is too deep");
        Json roots = Json::array();
        for (const auto &node : nodes) {
            if (++visited > 1000000)
                throw std::runtime_error("Too many RenderDoc actions");
            actions.insert(node.eventId);
            Json events = Json::array();
            for (const auto &event : node.events) {
                events.push_back({{"eventId", event.eventId}});
                if (event.chunkIndex >= file.chunks.size())
                    continue;
                const auto *chunk = file.chunks[event.chunkIndex];
                if (!chunk)
                    continue;
                native[std::to_string(event.eventId)] = {{"name", text(chunk->name)}};
                chunks[event.eventId] = chunk;
            }
            roots.push_back({{"eventId", node.eventId},
                             {"customName", text(node.customName)},
                             {"flags", uint32_t(node.flags)},
                             {"flags_text", flagsText(node.flags)},
                             {"numIndices", node.numIndices},
                             {"numInstances", node.numInstances},
                             {"events", events},
                             {"children", walk(node.children, file, depth + 1)}});
        }
        return roots;
    }
};
const SDObject &child(const SDObject &obj, const char *name) {
    const SDObject *found{};
    for (size_t k = 0; k < obj.NumChildren(); ++k) {
        const auto *value = obj.GetChild(k);
        if (value && value->name == name) {
            if (found)
                throw std::runtime_error(std::string("Missing or ambiguous structured field ") + name);
            found = value;
        }
    }
    if (!found)
        throw std::runtime_error(std::string("Missing or ambiguous structured field ") + name);
    return *found;
}
uint64_t field(const SDObject &obj, const char *name) {
    const auto &value = child(obj, name);
    if (value.type.basetype != SDBasic::UnsignedInteger && value.type.basetype != SDBasic::Enum &&
        value.type.basetype != SDBasic::Resource)
        throw std::runtime_error(std::string("Invalid structured unsigned field ") + name);
    return value.data.basic.u;
}
Json writes(const Events &events) {
    Json result = Json::object();
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::pair<uint32_t, uint64_t>> maps;
    for (const auto &[eid, obj] : events.chunks) {
        const auto name = text(obj->name);
        if (name != "ID3D11DeviceContext::Map" && name != "ID3D11DeviceContext::Unmap" &&
            name != "ID3D11DeviceContext::UpdateSubresource")
            continue;
        const auto key = std::to_string(eid);
        try {
            if (name.ends_with("UpdateSubresource")) {
                const auto &box = child(*obj, "pDstBox");
                Json bounds = nullptr;
                if (box.type.basetype != SDBasic::Null) {
                    bounds = Json::array();
                    for (const char *f : {"left", "top", "front", "right", "bottom", "back"})
                        bounds.push_back(field(box, f));
                }
                result[key] = {{"method", "UpdateSubresource"},
                               {"resource", field(*obj, "pDstResource")},
                               {"subresource", field(*obj, "DstSubresource")},
                               {"box", bounds},
                               {"before_eid", int64_t(eid) - 1},
                               {"coverage", bounds.is_null() ? "whole_subresource" : "destination_box"}};
            } else {
                const auto resource = field(*obj, "pResource"), sub = field(*obj, "Subresource"),
                           context = field(*obj, "Context"), kind = field(*obj, "MapType");
                const auto mapKey = std::tuple(context, resource, sub);
                if (name.ends_with("::Map"))
                    maps[mapKey] = {eid, kind};
                else {
                    const auto begin = maps.extract(mapKey);
                    if (kind < 2 || kind > 5)
                        continue;
                    if (begin.empty() || begin.mapped().second != kind)
                        throw std::runtime_error("Unmap has no matching writable Map");
                    result[key] = {{"method", "MapCapturedWrites"},
                                   {"resource", resource},
                                   {"subresource", sub},
                                   {"box", nullptr},
                                   {"map_type", kind},
                                   {"before_eid", int64_t(begin.mapped().first) - 1},
                                   {"coverage", "mapped_subresource_candidate"}};
                }
            }
        } catch (const std::exception &e) {
            result[key] = {{"error", e.what()}, {"method", name}};
        }
    }
    return result;
}
uint32_t extent(uint32_t size, uint32_t mip) { return mip >= 32 ? 1 : (std::max)(1U, size >> mip); }
void validate(const TextureDescription &tex, const Subresource &sub, uint32_t x, uint32_t y) {
    if (sub.mip >= tex.mips)
        throw std::runtime_error("Pixel history mip is outside the texture");
    const auto layers = tex.dimension == 3 ? extent(tex.depth, sub.mip) : tex.arraysize;
    if (sub.slice >= layers)
        throw std::runtime_error("Pixel history layer is outside the selected mip");
    if (sub.sample >= (std::max)(1U, tex.msSamp))
        throw std::runtime_error("Pixel history sample is outside the texture");
    if (x >= extent(tex.width, sub.mip) || y >= extent(tex.height, sub.mip))
        throw std::runtime_error("Pixel history coordinates are outside the selected mip");
}
bool selected(const Json &write, const TextureDescription &tex, const Subresource &sub, uint32_t x,
              uint32_t y) {
    const uint64_t index = tex.dimension == 3 ? sub.mip : uint64_t(sub.mip) + uint64_t(sub.slice) * tex.mips;
    if (write.at("subresource") != index)
        return false;
    const auto &box = write.at("box");
    const auto z = tex.dimension == 3 ? sub.slice : 0U;
    return box.is_null() ||
           (box[0] <= x && x < box[3] && box[1] <= y && y < box[4] && box[2] <= z && z < box[5]);
}
} // namespace

extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *mem) {
    if (mem)
        release(mem);
}
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return allocate(size); }

namespace flora {
Json runRdcJob(const Json &job) {
    const auto action = job.at("action").get<std::string>();
    if (action != "history" && action != "events")
        throw std::runtime_error("Unsupported native RenderDoc job action");
    if (action == "history") {
        if (!job.value("gpa_event", Json(nullptr)).is_null() && !job.value("eid", Json(nullptr)).is_null())
            throw std::runtime_error("Choose GPA or RDC event, not both");
        for (const auto *key : {"x", "y", "mip", "layer", "sample", "eid"})
            index(job, key);
        for (const auto *key : {"gpa_event", "resource"})
            if (job.contains(key) && !job.at(key).is_null())
                number(job.at(key));
    }
    Session session(job);
    auto &controller = *session.controller;
    Events events;
    const auto roots = events.walk(controller.GetRootActions(), controller.GetStructuredFile());
    auto mapping = indexRdcEvents(roots, events.native);
    Json result{{"ok", true},
                {"action", action},
                {"backend_version", session.version},
                {"backend_commit", session.commit},
                {"gpa_event_map", mapping["gpa_event_map"]},
                {"gpa_command_map", mapping["gpa_command_map"]}};
    if (action == "events") {
        result["index"] = mapping;
        result["cpu_writes"] = writes(events);
        return result;
    }
    const auto gpa = job.value("gpa_event", Json(nullptr));
    if (!gpa.is_null() && !mapping["gpa_event_map"].contains(std::to_string(number(gpa))))
        throw std::runtime_error("Selected GPA event has no uniquely mapped executed action");
    uint32_t eid = index(job, "eid");
    if (!eid) {
        if (!gpa.is_null())
            eid = mapping["gpa_event_map"].at(std::to_string(number(gpa))).get<uint32_t>();
        else if (!mapping["actions"].empty())
            eid = mapping["actions"].back().at("eid").get<uint32_t>();
        else
            for (const auto &v : mapping["gpa_event_map"].items())
                eid = (std::max)(eid, v.value().get<uint32_t>());
    }
    if (!eid || (!events.actions.contains(eid) && !events.chunks.contains(eid)))
        throw std::runtime_error("No selectable replay action");
    controller.SetFrameEvent(eid, true);
    const auto resources = controller.GetResources();
    ResourceId resource;
    const auto gpaResource = job.value("resource", Json(nullptr));
    if (gpaResource.is_null()) {
        const auto *pipe = controller.GetD3D11PipelineState();
        if (pipe && !pipe->outputMerger.renderTargets.empty())
            resource = pipe->outputMerger.renderTargets[0].resource;
    } else {
        const auto prefix = "GPA resource " + std::to_string(number(gpaResource)) + " (";
        for (const auto &r : resources)
            if (text(r.name).starts_with(prefix)) {
                resource = r.resourceId;
                break;
            }
    }
    if (resource == ResourceId::Null())
        throw std::runtime_error("Select an explicit texture resource; no matching target");
    const auto textures = controller.GetTextures();
    const auto found = std::find_if(textures.begin(), textures.end(),
                                    [&](const auto &t) { return t.resourceId == resource; });
    if (found == textures.end())
        throw std::runtime_error("Selected resource is not a texture");
    const auto &tex = *found;
    const Subresource sub(index(job, "mip"), index(job, "layer"), index(job, "sample"));
    const auto x = index(job, "x"), y = index(job, "y");
    validate(tex, sub, x, y);
    const std::string kind = tex.format.compType == CompType::UInt   ? "uint"
                             : tex.format.compType == CompType::SInt ? "sint"
                                                                     : "float";
    Json records = Json::array(), gaps = Json::array();
    std::set<uint32_t> existing;
    for (const auto &v : controller.PixelHistory(resource, x, y, sub, CompType::Typeless)) {
        auto r = record(v);
        r.update(rdcProvenance(v.eventId, mapping["reverse"]));
        r["record_kind"] = "backend_modification";
        r["color_interpretation"] = kind;
        records.push_back(std::move(r));
        existing.insert(v.eventId);
    }
    const auto parsed = writes(events);
    size_t added = 0;
    try {
        for (const auto &usage : controller.GetUsage(resource)) {
            if (usage.usage != ResourceUsage::CPUWrite || existing.contains(usage.eventId))
                continue;
            const auto key = std::to_string(usage.eventId);
            if (!parsed.contains(key) || parsed[key].contains("error")) {
                gaps.push_back(
                    {{"eventId", usage.eventId},
                     {"reason", parsed.contains(key) ? parsed[key].value("error", "Unknown CPU write layout")
                                                     : "Unknown CPU write layout"}});
                continue;
            }
            const auto &write = parsed[key];
            if (!selected(write, tex, sub, x, y))
                continue;
            try {
                const auto beforeId = write.at("before_eid").get<int64_t>();
                if (beforeId < 0 || beforeId > UINT32_MAX)
                    throw std::runtime_error("Invalid CPU write boundary");
                controller.SetFrameEvent(uint32_t(beforeId), true);
                const auto before = pixel(controller.PickPixel(resource, x, y, sub, CompType::Typeless));
                controller.SetFrameEvent(usage.eventId, true);
                const auto after = pixel(controller.PickPixel(resource, x, y, sub, CompType::Typeless));
                const auto field = kind == "uint" ? "uintValue" : kind == "sint" ? "intValue" : "floatValue";
                Json r{{"eventId", usage.eventId},
                       {"fragIndex", nullptr},
                       {"primitiveID", nullptr},
                       {"record_kind", "cpu_write_snapshot"},
                       {"preMod", {{"col", before}, {"depth", nullptr}, {"stencil", nullptr}}},
                       {"postMod", {{"col", after}, {"depth", nullptr}, {"stencil", nullptr}}},
                       {"shaderOut", nullptr},
                       {"color_interpretation", kind},
                       {"pixel_value_changed", before[field] != after[field]},
                       {"write", write}};
                r.update(rdcProvenance(usage.eventId, mapping["reverse"]));
                records.push_back(std::move(r));
                ++added;
            } catch (const std::exception &e) {
                gaps.push_back({{"eventId", usage.eventId}, {"reason", e.what()}});
            }
        }
    } catch (...) {
        controller.SetFrameEvent(eid, true);
        throw;
    }
    controller.SetFrameEvent(eid, true);
    std::stable_sort(records.begin(), records.end(), [](const Json &a, const Json &b) {
        return std::pair(a.at("eventId").get<uint32_t>(),
                         a.at("fragIndex").is_null() ? int64_t(-1) : a.at("fragIndex").get<int64_t>()) <
               std::pair(b.at("eventId").get<uint32_t>(),
                         b.at("fragIndex").is_null() ? int64_t(-1) : b.at("fragIndex").get<int64_t>());
    });
    Json unrepresented = Json::array();
    for (const auto &c : mapping["gpa_command_map"].items())
        if (c.value()["kind"] == "api" && c.value()["status"] == "unrepresented")
            unrepresented.push_back(number(c.value()["gpa_event"]));
    size_t unmapped = 0;
    for (const auto &r : records)
        if (r["gpa_event"].is_null())
            ++unmapped;
    result.update(
        {{"eid", eid},
         {"gpa_event", gpa},
         {"resource", resourceName(resource)},
         {"x", x},
         {"y", y},
         {"history", records},
         {"history_events", records.size()},
         {"history_scope",
          {{"backend", "RenderDoc PixelHistory of independent replay"},
           {"unmapped_records", unmapped},
           {"cpu_write_snapshots", added},
           {"cpu_write_gaps", gaps},
           {"unrepresented_api_commands", unrepresented},
           {"limitations",
            {"CPU writes use API-boundary pixel snapshots, not shader fragments; Map coverage is a "
             "subresource candidate.",
             "Old captures may lack GPA CPU-write markers; unknown structured layouts remain explicit gaps.",
             "Resource operations can appear with unchanged selected-subresource values; no shader fragment "
             "or test pass is implied."}}}}});
    result["resource_name"] = nullptr;
    for (const auto &r : resources)
        if (r.resourceId == resource)
            result["resource_name"] = text(r.name);
    return result;
}
} // namespace flora
