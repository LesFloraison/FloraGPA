#include "Backend.h"
#include "Api.h"
#include "Assets.h"
#include "Counters.h"
#include "DebugTrace.h"
#include "InputMesh.h"
#include "PipelineState.h"
#include "application/RdcEvents.h"
#include "application/ReplayMesh.h"
#include "application/ReplayRequest.h"
#include <QCryptographicHash>
#include <QSaveFile>
#define NOMINMAX
#include <Windows.h>

#include <Psapi.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
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
    if (v.is_string()) {
        const auto &text = v.get_ref<const std::string &>();
        uint64_t result{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("Invalid decimal uint64 identity");
        return result;
    }
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<int64_t>() < 0))
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
    uint32_t d16HistoryPatch{};
    uint32_t dxbcWorkgroupPatch{};
    uint32_t dx12UsageVisibilityPatch{};
    uint32_t readOnlyDepthSnapshotPatch{};
    using DepthSnapshotCopies = uint64_t (RENDERDOC_CC *)();
    DepthSnapshotCopies depthSnapshotCopies{};
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
        using PatchVersion = uint32_t (RENDERDOC_CC *)();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraD16HistoryPatchVersion")))
            d16HistoryPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxbcWorkgroupPatchVersion")))
            dxbcWorkgroupPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12UsageVisibilityPatchVersion")))
            dx12UsageVisibilityPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraReadOnlyDepthSnapshotPatchVersion")))
            readOnlyDepthSnapshotPatch = patch();
        if (job.value("readonly_depth_snapshot", false)) {
            if (readOnlyDepthSnapshotPatch != 1)
                throw std::runtime_error("Read-only depth snapshots require the Flora RenderDoc snapshot patch version 1");
            using Enable = void (RENDERDOC_CC *)(bool);
            symbol<Enable>("RENDERDOC_SetFloraReadOnlyDepthSnapshotEnabled")(true);
            depthSnapshotCopies = symbol<DepthSnapshotCopies>("RENDERDOC_GetFloraReadOnlyDepthSnapshotCopies");
        }
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
            ReplayOptions options;
            options.apiValidation = job.value("api_validation", false);
            auto opened = file->OpenCapture(options, {});
            controller.reset(opened.second);
            checked(opened.first);
            if (!controller)
                throw std::runtime_error("RenderDoc did not create a replay controller");
            const auto api = controller->GetAPIProperties().pipelineType;
            if (job.value("readonly_depth_snapshot", false) && api != GraphicsAPI::D3D12)
                throw std::runtime_error("Read-only depth snapshots require a DX12 independent capture");
            if (api != GraphicsAPI::D3D11 && api != GraphicsAPI::D3D12)
                throw std::runtime_error(
                    "Native analysis requires a DX11 or DX12 independent replay capture");
            const auto backend =
                api == GraphicsAPI::D3D12 ? flora::ReplayBackend::Dx12 : flora::ReplayBackend::Dx11;
            if (job.contains("request_identity") &&
                flora::parseReplayIdentity(job.at("request_identity")).backend != backend)
                throw std::runtime_error("Analysis request backend does not match the independent capture");
        } catch (...) {
            const auto log = reinterpret_cast<decltype(&RENDERDOC_GetLogFile)>(
                GetProcAddress(library, "RENDERDOC_GetLogFile"));
            if (log)
                fprintf(stderr, "RenderDoc diagnostic log: %s\n", log());
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
            roots.push_back(
                {{"eventId", node.eventId},
                 {"customName", text(node.customName)},
                 {"flags", uint32_t(node.flags)},
                 {"flags_text", flagsText(node.flags)},
                 {"numIndices", node.numIndices},
                 {"numInstances", node.numInstances},
                 {"dispatchDimension",
                  {node.dispatchDimension[0], node.dispatchDimension[1], node.dispatchDimension[2]}},
                 {"dispatchThreadsDimension",
                  {node.dispatchThreadsDimension[0], node.dispatchThreadsDimension[1],
                   node.dispatchThreadsDimension[2]}},
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
ResourceId capturedResource(const rdcarray<ResourceDescription> &resources, const Json &job, bool dx12) {
    const auto &object = job.at("resource");
    auto prefix = dx12 ? "Flora DX12 resource " + std::to_string(number(object)) + " creation "
                       : "GPA resource " + std::to_string(number(object)) + " (";
    const bool exact = dx12 && job.contains("resource_creation");
    if (exact)
        prefix += std::to_string(number(job.at("resource_creation")));
    ResourceId result;
    for (const auto &resource : resources)
        if (exact ? text(resource.name) == prefix : text(resource.name).starts_with(prefix)) {
            if (result != ResourceId::Null())
                throw std::runtime_error(
                    "Resource object has multiple captured lifetimes; select resource_creation");
            result = resource.resourceId;
        }
    if (result == ResourceId::Null())
        throw std::runtime_error("No matching captured resource for the selected object/creation");
    return result;
}
const ActionDescription *findAction(const rdcarray<ActionDescription> &nodes, uint32_t eid) {
    for (const auto &node : nodes) {
        if (node.eventId == eid)
            return &node;
        if (const auto *found = findAction(node.children, eid))
            return found;
    }
    return nullptr;
}
rdcfixedarray<uint32_t, 3> coordinates(const Json &job, const char *key) {
    rdcfixedarray<uint32_t, 3> result{};
    if (!job.contains(key))
        return result;
    const auto &v = job.at(key);
    if (!v.is_array() || v.size() != 3)
        throw std::runtime_error(std::string("Expected three coordinates: ") + key);
    for (size_t i = 0; i < 3; ++i) {
        const auto n = number(v[i]);
        if (n > UINT32_MAX)
            throw std::runtime_error(std::string("Coordinate exceeds uint32: ") + key);
        result[i] = uint32_t(n);
    }
    return result;
}
Json debug(IReplayController &controller, uint32_t eid, const Json &job, uint32_t dxbcWorkgroupPatch) {
    const auto kind = job.at("action").get<std::string>();
    const auto *selected = findAction(controller.GetRootActions(), eid);
    const bool compute = kind == "debug-thread", vertex = kind == "debug-vertex";
    if (!selected || !(selected->flags & (compute ? ActionFlags::Dispatch : ActionFlags::Drawcall)))
        throw std::runtime_error(compute ? "Compute debugging requires an executed dispatch action"
                                         : "Selected analysis requires an executed draw action");
    const bool dx12 = controller.GetAPIProperties().pipelineType == GraphicsAPI::D3D12;
    const auto *pipe = dx12 ? nullptr : controller.GetD3D11PipelineState();
    const auto *pipe12 = dx12 ? controller.GetD3D12PipelineState() : nullptr;
    if (!pipe && !pipe12)
        throw std::runtime_error("No Direct3D pipeline at the selected event");
    const auto *reflection = pipe12 ? (compute  ? pipe12->computeShader.reflection
                                       : vertex ? pipe12->vertexShader.reflection
                                                : pipe12->pixelShader.reflection)
                                    : (compute  ? pipe->computeShader.reflection
                                       : vertex ? pipe->vertexShader.reflection
                                                : pipe->pixelShader.reflection);
    if (!reflection)
        throw std::runtime_error("No shader bound at the selected stage");
    if (!reflection->debugInfo.debuggable)
        throw std::runtime_error("Shader cannot be debugged: " + text(reflection->debugInfo.debugStatus));
    Json result = Json::object();
    struct ReferenceSelection {
        ResourceId resource;
        uint32_t x, y;
        Subresource sub;
        CompType cast;
    };
    std::optional<ReferenceSelection> referenceSelection;
    if (dx12 && compute && !job.value("resource", Json()).is_null()) {
        const auto resource = capturedResource(controller.GetResources(), job, true);
        const auto textures = controller.GetTextures();
        const auto found = std::find_if(textures.begin(), textures.end(),
                                        [&](const auto &t) { return t.resourceId == resource; });
        if (found == textures.end())
            throw std::runtime_error("Compute GPU reference requires a texture resource");
        const Subresource sub(index(job, "mip"), index(job, "layer"), index(job, "sample"));
        const auto x = index(job, "x"), y = index(job, "y");
        validate(*found, sub, x, y);
        Json reference{{"status", "unavailable"},
                       {"scope", "post_event_selected_texel_before_debug_replays"},
                       {"resource", resourceName(resource)},
                       {"resource_object", job.at("resource")},
                       {"resource_creation", job.value("resource_creation", Json())},
                       {"x", x},
                       {"y", y},
                       {"mip", sub.mip},
                       {"layer", sub.slice},
                       {"sample", sub.sample},
                       {"reason", "Typeless texture interpretation is unavailable"}};
        const auto cast = found->format.compType;
        if (cast != CompType::Typeless) {
            referenceSelection = ReferenceSelection{resource, x, y, sub, cast};
            reference.erase("reason");
            reference.update(
                {{"status", "read"},
                 {"format", flora::rdcResourceFormat(found->format)},
                 {"interpretation", cast == CompType::UInt   ? "uint"
                                    : cast == CompType::SInt ? "sint"
                                                             : "float"},
                 {"value", pixel(controller.PickPixel(resource, x, y, sub, cast))},
                 {"method", "RenderDoc PickPixel with texture component interpretation"},
                 {"note", "Explicit selected texel after the dispatch, before DebugThread replays. This is "
                          "not proof that the selected invocation wrote this location."}});
        }
        result["gpu_reference"] = std::move(reference);
    }
    if (dx12 && !compute && !vertex) {
        // Capture the selected event's target before DebugPixel performs any
        // diagnostic replays. This is post-event GPU output, not the selected
        // fragment's shader return value (blending/other fragments may differ).
        Json reference{{"status", "unavailable"},
                       {"scope", "post_event_target_before_debug_replays"},
                       {"output_index", 0},
                       {"reason", "No color target 0 is bound"}};
        if (!pipe12->outputMerger.renderTargets.empty()) {
            const auto &target = pipe12->outputMerger.renderTargets[0];
            if (target.resource != ResourceId()) {
                const auto textures = controller.GetTextures();
                const auto found = std::find_if(textures.begin(), textures.end(), [&](const auto &t) {
                    return t.resourceId == target.resource;
                });
                reference["reason"] = "Color target metadata is unavailable";
                if (found != textures.end()) {
                    const auto &texture = *found;
                    const auto x = index(job, "x"), y = index(job, "y"), sample = index(job, "sample");
                    reference["reason"] =
                        "Target view covers multiple layers or a 3D texture; output layer is ambiguous";
                    if (texture.dimension != 3 && target.numSlices == 1) {
                        reference["reason"] = "Pixel invocation lies outside color target 0";
                        if (target.firstMip < texture.mips && target.firstSlice < texture.arraysize &&
                            x < extent(texture.width, target.firstMip) &&
                            y < extent(texture.height, target.firstMip) &&
                            sample < (std::max)(1U, texture.msSamp)) {
                            const Subresource sub(target.firstMip, target.firstSlice, sample);
                            const auto cast = target.format.compType;
                            reference["reason"] = "Typeless color target interpretation is unavailable";
                            if (cast != CompType::Typeless) {
                                referenceSelection = ReferenceSelection{target.resource, x, y, sub, cast};
                                const auto value = controller.PickPixel(target.resource, x, y, sub, cast);
                                reference = {
                                    {"status", "read"},
                                    {"scope", "post_event_target_before_debug_replays"},
                                    {"output_index", 0},
                                    {"resource", resourceName(target.resource)},
                                    {"x", x},
                                    {"y", y},
                                    {"mip", sub.mip},
                                    {"layer", sub.slice},
                                    {"sample", sub.sample},
                                    {"format", flora::rdcResourceFormat(target.format)},
                                    {"interpretation", cast == CompType::UInt   ? "uint"
                                                       : cast == CompType::SInt ? "sint"
                                                                                : "float"},
                                    {"value", pixel(value)},
                                    {"method",
                                     "RenderDoc PickPixel with target-view component interpretation"},
                                    {"note", "Post-event target includes storage conversion and blending/all "
                                             "fragments; this is not the selected invocation's shader "
                                             "return. DebugPixel may replay again."}};
                            }
                        }
                    }
                }
            }
        }
        result["gpu_reference"] = std::move(reference);
    }
    auto freeTrace = [&](ShaderDebugTrace *p) {
        if (p)
            controller.FreeTrace(p);
    };
    std::unique_ptr<ShaderDebugTrace, decltype(freeTrace)> trace(nullptr, freeTrace);
    if (compute) {
        trace.reset(controller.DebugThread(coordinates(job, "group"), coordinates(job, "thread")));
    } else if (vertex) {
        int64_t actual = 0;
        if (job.contains("index") && !job.at("index").is_null()) {
            actual = index(job, "index");
        } else if (selected->flags & ActionFlags::Indexed) {
            const auto stride = pipe12 ? pipe12->inputAssembly.indexBuffer.byteStride
                                       : pipe->inputAssembly.indexBuffer.byteStride;
            const auto offset = pipe12 ? pipe12->inputAssembly.indexBuffer.byteOffset
                                       : pipe->inputAssembly.indexBuffer.byteOffset;
            const auto resource = pipe12 ? pipe12->inputAssembly.indexBuffer.resourceId
                                         : pipe->inputAssembly.indexBuffer.resourceId;
            if (stride != 1 && stride != 2 && stride != 4)
                throw std::runtime_error("Invalid index buffer stride");
            const auto address =
                uint64_t(offset) + (uint64_t(selected->indexOffset) + index(job, "vertex")) * stride;
            const auto bytes = controller.GetBufferData(resource, address, stride);
            if (bytes.size() != stride)
                throw std::runtime_error("Selected vertex index is outside the index buffer");
            uint32_t raw = 0;
            memcpy(&raw, bytes.data(), stride);
            actual = int64_t(raw) + selected->baseVertex;
        } else {
            actual = int64_t(selected->vertexOffset) + index(job, "vertex");
        }
        if (actual < 0 || uint64_t(actual) > UINT32_MAX)
            throw std::runtime_error("Actual vertex index is outside uint32");
        result["actual_vertex_index"] = actual;
        trace.reset(
            controller.DebugVertex(index(job, "vertex"), index(job, "instance"), uint32_t(actual), 0));
    } else {
        DebugPixelInputs inputs;
        inputs.sample = index(job, "sample");
        if (job.contains("primitive"))
            inputs.primitive = index(job, "primitive", UINT32_MAX);
        result["pixel_invocation"] = {
            {"x", index(job, "x")},
            {"y", index(job, "y")},
            {"sample", inputs.sample},
            {"primitive", inputs.primitive == UINT32_MAX ? Json() : Json(inputs.primitive)},
            {"selection_scope",
             "RenderDoc pixel/sample/primitive selection; fragment index and instance are not selected"}};
        trace.reset(controller.DebugPixel(index(job, "x"), index(job, "y"), inputs));
    }
    if (!trace || !trace->debugger)
        throw std::runtime_error("No debugger trace at the selected invocation");
    if (referenceSelection) {
        const auto &selection = *referenceSelection;
        result["gpu_reference"]["after_trace_creation_value"] =
            pixel(controller.PickPixel(selection.resource, selection.x, selection.y, selection.sub,
                                       selection.cast));
    }
    result["trace"] = flora::rdcDebugTrace(*trace);
    Json missingOffsets = Json::array();
    const auto &globals = result["trace"]["sourceVars"];
    for (size_t i = 0; i < globals.size(); ++i)
        if (globals[i]["offset"].is_null())
            missingOffsets.push_back(i);
    result["debug_scope"] = {
        {"unavailable_global_source_offsets", missingOffsets},
        {"offset_policy", "RenderDoc 1.45 DXBC signature/whole-block mappings have uninitialized offsets; "
                          "these are null, while defined member/instruction offsets are preserved."}};
    result["source_debug"] = flora::rdcDebugInfo(reflection->debugInfo);
    const auto assembly = controller.DisassembleShader(
        pipe12 ? pipe12->pipelineResourceId : ResourceId::Null(), reflection, "");
    auto exportedAssembly = QByteArray(assembly.c_str(), qsizetype(assembly.size()));
    exportedAssembly.replace("\n", "\r\n");
    QSaveFile file(QString::fromStdString(job.at("out").get<std::string>()) + "/debug_shader.asm");
    if (!file.open(QIODevice::WriteOnly) || file.write(exportedAssembly) != exportedAssembly.size() ||
        !file.commit())
        throw std::runtime_error("Cannot export shader disassembly");
    result["disassembly"] = text(assembly);
    if (dx12 && compute && text(assembly).find("dcl_tgsm_") != std::string::npos) {
        const auto group = coordinates(job, "group");
        const bool affected = dxbcWorkgroupPatch != 1 && (group[0] || group[1] || group[2]);
        result["debug_scope"]["dxbc_workgroup"] = {
            {"patch_version", dxbcWorkgroupPatch},
            {"group", {group[0], group[1], group[2]}},
            {"status", affected ? "known_incorrect_helper_group_ids" : "group_ids_initialized"}};
        if (affected) {
            result["partial"] = true;
            result["debug_scope"]["warnings"] = {
                "This RenderDoc 1.45 DX12 DXBC debugger assigns the selected "
                "Group ID only to the active lane. Other groupshared lanes use group zero, so this trace "
                "is not a reliable computation of the selected group. Select the patched analysis library."};
        }
    }
    Json steps = Json::array();
    for (;;) {
        const auto batch = controller.ContinueDebug(trace->debugger);
        if (batch.empty())
            break;
        if (batch.size() > 250000 - steps.size())
            throw std::runtime_error("Debug trace exceeded 250000 steps");
        for (const auto &state : batch)
            steps.push_back(flora::rdcDebugState(state));
    }
    if (steps.empty())
        throw std::runtime_error("Shader debugger returned no steps");
    result["step_count"] = steps.size();
    result["steps"] = std::move(steps);
    // Debugging can replay the capture again, including when a DXBC UAV is
    // fetched lazily. Do not silently compare a trace against an earlier
    // output when those replays have changed even the selected reference.
    if (referenceSelection) {
        const auto &selection = *referenceSelection;
        const auto after = pixel(controller.PickPixel(selection.resource, selection.x, selection.y,
                                                      selection.sub, selection.cast));
        auto &reference = result["gpu_reference"];
        const auto &before = reference.at("value").at("uintValue");
        const bool same = before == after.at("uintValue") &&
                          before == reference.at("after_trace_creation_value").at("uintValue");
        reference["after_debug_value"] = after;
        reference["consistency"] = same ? "selected_texel_unchanged" : "selected_texel_changed";
        reference["consistency_scope"] =
            "Selected post-event texel before debugging, after trace creation and after stepping, "
            "compared as PickPixel result bits. "
            "An unchanged texel does not establish stable inputs or validate the simulated invocation.";
        if (!same) {
            result["partial"] = true;
            auto &warnings = result["debug_scope"]["warnings"];
            if (warnings.is_null())
                warnings = Json::array();
            warnings.push_back("The selected GPU texel changed during shader-debug replays. The earlier "
                               "GPU reference cannot validate this trace; inspect replay/input stability.");
        }
    }
    return result;
}
} // namespace

extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *mem) {
    if (mem)
        release(mem);
}
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return allocate(size); }

namespace flora {
Json rdcLoadedModules() {
    Json modules = Json::array();
    HMODULE handles[2048];
    DWORD length{};
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &length))
        throw std::runtime_error("Cannot enumerate analysis modules: " + std::to_string(GetLastError()));
    if (length > sizeof(handles))
        throw std::runtime_error("Module inventory buffer too small");
    for (DWORD i = 0; i < length / DWORD(sizeof(HMODULE)); ++i) {
        wchar_t name[32768]{};
        const auto size = GetModuleFileNameW(handles[i], name, 32768);
        if (!size || size >= 32768)
            throw std::runtime_error("Cannot read complete analysis module path");
        modules.push_back(QString::fromWCharArray(name).toStdString());
    }
    return modules;
}
Json runRdcJob(const Json &job) {
    const auto action = job.at("action").get<std::string>();
    if (job.value("readonly_depth_snapshot", false) && action != "texture" && action != "debug-pixel")
        throw std::runtime_error("Read-only depth snapshots are limited to texture and debug-pixel analysis");
    const bool debugging = action == "debug-pixel" || action == "debug-vertex" || action == "debug-thread";
    if (action != "history" && action != "events" && action != "counters" && action != "inventory" &&
        action != "texture" && action != "resource-history" && action != "postmesh" && action != "pipeline" && !debugging)
        throw std::runtime_error("Unsupported native RenderDoc job action");
    if (action != "events") {
        if (!job.value("gpa_event", Json(nullptr)).is_null() && !job.value("eid", Json(nullptr)).is_null())
            throw std::runtime_error("Choose GPA or RDC event, not both");
        for (const auto *key :
             {"x", "y", "mip", "layer", "sample", "eid", "vertex", "instance", "index", "primitive"})
            index(job, key);
        for (const auto *key : {"gpa_event", "execution", "resource", "resource_creation"})
            if (job.contains(key) && !job.at(key).is_null())
                number(job.at(key));
        if (debugging) {
            coordinates(job, "group");
            coordinates(job, "thread");
        }
    }
    Session session(job);
    auto &controller = *session.controller;
    const auto properties = controller.GetAPIProperties();
    const bool dx12 = properties.pipelineType == GraphicsAPI::D3D12;
    if (dx12)
        for (const auto key : {"execution", "resource", "resource_creation"})
            if (job.contains(key) && !job[key].is_null() && !job[key].is_string())
                throw std::runtime_error("DX12 64-bit identities must be decimal strings");
    if ((dx12 && job.contains("gpa_event") && !job["gpa_event"].is_null()) ||
        (!dx12 && job.contains("execution")))
        throw std::runtime_error("Select an execution instance for DX12 or a GPA event for DX11");
    if (dx12 && !job.value("execution", Json(nullptr)).is_null() &&
        !job.value("eid", Json(nullptr)).is_null())
        throw std::runtime_error("Choose DX12 execution or RDC event, not both");
    const auto vendor = uint32_t(properties.vendor);
    // These are RenderDoc 1.45 enum values, not PCI vendor IDs. Report the active
    // replay controller rather than inferring its adapter from the capture host.
    constexpr const char *vendorNames[]{"Unknown",     "ARM",      "AMD",    "Broadcom",
                                        "Imagination", "Intel",    "NVIDIA", "Qualcomm",
                                        "Verisilicon", "Software", "Samsung"};
    Events events;
    const auto roots = events.walk(controller.GetRootActions(), controller.GetStructuredFile());
    auto mapping = dx12 ? indexDx12RdcEvents(roots, events.native) : indexRdcEvents(roots, events.native);
    Json result{
        {"ok", true},
        {"action", action},
        {"backend", dx12 ? "dx12" : "dx11"},
        {"backend_version", session.version},
        {"backend_commit", session.commit},
        {"backend_d16_history_patch", session.d16HistoryPatch},
        {"backend_dxbc_workgroup_patch", session.dxbcWorkgroupPatch},
        {"backend_dx12_usage_visibility_patch", session.dx12UsageVisibilityPatch},
        {"backend_readonly_depth_snapshot_patch", session.readOnlyDepthSnapshotPatch},
        {"replay_environment",
         {{"gpu_vendor_enum", vendor},
          {"gpu_vendor_name", vendor < std::size(vendorNames) ? Json(vendorNames[vendor]) : Json(nullptr)},
          {"remote", properties.remoteReplay},
          {"degraded", properties.degraded},
          {"pipeline_type_enum", uint32_t(properties.pipelineType)},
          {"local_renderer_enum", uint32_t(properties.localRenderer)}}},
        {"gpa_event_map", mapping["gpa_event_map"]},
        {"gpa_command_map", mapping["gpa_command_map"]}};
    // Match the original audit point before the replay controller is shut down.
    auto complete = [&] {
        if (session.depthSnapshotCopies) {
            result["readonly_depth_snapshot"] = {
                {"requested", true}, {"copies", std::to_string(session.depthSnapshotCopies())},
                {"experimental", true},
                {"scope", "Isolated direct DX12 draws with read-only depth/stencil state, D32 single-mip, "
                          "single-layer, single-sample 2D depth on one node. Preserves the depth comparison "
                          "using a GPU copy; full replay is unchanged. Copies add GPU work and waits. "
                          "A nonzero copy count does not prove replay correctness."}};
        }
        if (job.value("api_validation", false)) {
            const auto messages = controller.GetDebugMessages();
            Json rows = Json::array();
            for (size_t i = 0; i < messages.size() && i < 4096; ++i) {
                const auto &message = messages[i];
                rows.push_back({{"event_id", message.eventId},
                                {"category_enum", uint32_t(message.category)},
                                {"severity_enum", uint32_t(message.severity)},
                                {"source_enum", uint32_t(message.source)},
                                {"message_id", message.messageID},
                                {"description", text(message.description)}});
            }
            result["api_validation"] = {
                {"requested", true}, {"messages", std::move(rows)},
                {"message_count", std::to_string(messages.size())},
                {"truncated", messages.size() > 4096},
                {"message_phase", dx12 ? "capture_loading" : "backend_supplied"},
                {"scope", dx12
                    ? "RenderDoc 1.45 D3D12 GetDebugMessages exports loading-stage messages, not the "
                      "API queue from subsequent analysis replays. An empty list does not validate "
                      "the selected analysis or prove that the validation layer was available."
                    : "Diagnostics exported by the backend. An empty list does not prove replay "
                      "correctness or that the validation layer was available."}};
        }
        if (dx12) {
            result["execution_map"] = mapping["execution_map"];
            result["record_executions"] = mapping["record_executions"];
            result["execution"] = job.value("execution", Json(nullptr));
            result.erase("gpa_event");
        }
        result["loaded_modules"] = rdcLoadedModules();
        return std::move(result);
    };
    if (action == "events") {
        result["index"] = mapping;
        result["cpu_writes"] = writes(events);
        return complete();
    }
    const auto resources = controller.GetResources();
    const auto gpa = job.value(dx12 ? "execution" : "gpa_event", Json(nullptr));
    const auto selectionMap = dx12 ? "execution_map" : "gpa_event_map";
    if (!gpa.is_null() && !mapping[selectionMap].contains(std::to_string(number(gpa)))) {
        const auto key = std::to_string(number(gpa));
        if (dx12 && mapping["execution_commands"].contains(key)) {
            const auto &command = mapping["execution_commands"][key];
            const auto status = command.value("status", std::string());
            if (command.value("name", std::string()) == "ID3D12GraphicsCommandListExecuteIndirect" &&
                (status == "mapped" || status == "unrepresented")) {
                if (command.at("event_ids").empty())
                    throw std::runtime_error("Selected indirect call generated no Draw/Dispatch action");
                if (command.at("event_ids").size() > 1)
                    throw std::runtime_error("Selected indirect call generated multiple actions; individual "
                                             "invocation selection is not implemented");
            }
        }
        throw std::runtime_error("Selected capture event has no uniquely mapped executed action");
    }
    uint32_t eid = index(job, "eid");
    if (!eid) {
        if (!gpa.is_null())
            eid = mapping[selectionMap].at(std::to_string(number(gpa))).get<uint32_t>();
        else if (!mapping["actions"].empty())
            eid = mapping["actions"].back().at("eid").get<uint32_t>();
        else
            for (const auto &v : mapping[selectionMap].items())
                eid = (std::max)(eid, v.value().get<uint32_t>());
    }
    if (!eid && action == "inventory" && !events.chunks.empty())
        eid = events.chunks.rbegin()->first;
    if ((!eid || (!events.actions.contains(eid) && !events.chunks.contains(eid))) &&
        !(eid == 0 && action == "inventory"))
        throw std::runtime_error("No selectable replay action");
    controller.SetFrameEvent(eid, true);
    if (action == "pipeline") {
        const auto *selected = findAction(controller.GetRootActions(), eid);
        if (!dx12 || !selected || !(selected->flags & (ActionFlags::Drawcall | ActionFlags::Dispatch | ActionFlags::MeshDispatch)))
            throw std::runtime_error("Pipeline inspection requires an executed DX12 Draw or Dispatch");
        result.update(readDx12Pipeline(controller));
        result["eid"] = eid;
        return complete();
    }
    if (action == "postmesh") {
        const auto *selected = findAction(controller.GetRootActions(), eid);
        const auto stage = job.value("stage", std::string("VSOut"));
        const bool meshOutput = stage == "MeshOut";
        if (!selected ||
            !(selected->flags & (meshOutput ? ActionFlags::MeshDispatch : ActionFlags::Drawcall)))
            throw std::runtime_error(meshOutput ? "Mesh output requires an executed mesh dispatch"
                                                : "Selected analysis requires an executed draw action");
        if (meshOutput && index(job, "instance") != 0)
            throw std::runtime_error("Mesh output returns all meshlets; draw instances do not apply");
        if (stage == "VSIn") {
            if (!dx12) throw std::runtime_error("Raw input mesh adapter currently requires DX12");
            const auto data = readDx12InputMesh(controller, *selected, index(job, "instance"), index(job, "attribute"));
            const auto decoded = ReplayMesh::decode(data.metadata, data.vertices, data.indices);
            auto save = [&](const char *name, const auto &bytes) {
                QSaveFile file(QString::fromStdWString((path(job.at("out")) / name).wstring()));
                if (!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
                    throw std::runtime_error("Cannot save input mesh artifact");
            };
            save("post_vertices.bin", data.vertices); save("post_indices.bin", data.indices);
            save("input_vertices.bin", data.rawVertices); save("input_indices.bin", data.rawIndices);
            save("post_vertices.csv", decoded.csv()); save("post_geometry.obj", decoded.obj());
            result.update({{"eid", eid}, {"gpa_event", gpa}, {"stage", stage}, {"mesh", data.metadata},
                {"vertex_count", decoded.positions.size()}, {"index_count", decoded.indexCount}, {"face_count", decoded.candidateFaceCount}});
            return complete();
        }
        const auto nativeStage = stage == "VSOut"   ? MeshDataStage::VSOut
                                 : stage == "GSOut" ? MeshDataStage::GSOut
                                 : meshOutput       ? MeshDataStage::MeshOut
                                                    : MeshDataStage::VSIn;
        if (stage != "VSOut" && stage != "GSOut" && stage != "VSIn" && !meshOutput)
            throw std::runtime_error("Unsupported post-shader stage");
        const auto mesh = controller.GetPostVSData(index(job, "instance"), 0, nativeStage);
        if (mesh.vertexResourceId == ResourceId::Null())
            throw std::runtime_error("Selected event/stage has no post-shader mesh data");
        auto save = [&](const wchar_t *name, const void *bytes, size_t size) {
            QSaveFile file(QString::fromStdWString((path(job.at("out")) / name).wstring()));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(static_cast<const char *>(bytes), qint64(size)) != qint64(size) || !file.commit())
                throw std::runtime_error("Cannot save post-shader mesh artifact");
        };
        uint64_t vertexBytes = mesh.vertexByteSize;
        Json meshlets = Json::array();
        if (meshOutput) {
            uint64_t vertices{}, indices{};
            for (const auto &size : mesh.meshletSizes) {
                vertices += size.numVertices;
                indices += size.numIndices;
                if (vertices > UINT32_MAX || indices > UINT32_MAX)
                    throw std::runtime_error("Mesh output count exceeds supported bounds");
                meshlets.push_back({{"vertices", size.numVertices}, {"indices", size.numIndices}});
            }
            vertexBytes = vertices * mesh.vertexByteStride;
            if (indices != mesh.numIndices || !vertices || !mesh.vertexByteStride ||
                vertexBytes > mesh.vertexByteSize || vertexBytes > (1ULL << 30))
                throw std::runtime_error("Empty, inconsistent or oversized mesh output");
            // The D3D12 backend compacts vertices first, then per-primitive
            // attributes, then rebased indices. Its reported primitive offset
            // can disagree with the vertex count/stride; do not use it to size
            // the position table or interpret primitive attributes.
            if (mesh.topology != Topology::TriangleList || indices % 3)
                throw std::runtime_error("Mesh output currently requires triangle-list topology");
            if (mesh.indexResourceId == mesh.vertexResourceId &&
                (mesh.indexByteOffset < mesh.vertexByteOffset ||
                 vertexBytes + (indices / 3) * mesh.perPrimitiveStride >
                     mesh.indexByteOffset - mesh.vertexByteOffset))
                throw std::runtime_error("Mesh vertex/primitive data overlaps index storage");
        }
        const auto raw = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, vertexBytes);
        if (meshOutput && raw.size() != vertexBytes)
            throw std::runtime_error("Truncated mesh vertex readback");
        save(L"post_vertices.bin", raw.data(), raw.size());
        bytebuf ib;
        if (mesh.indexResourceId != ResourceId::Null()) {
            ib = controller.GetBufferData(mesh.indexResourceId, mesh.indexByteOffset,
                                          uint64_t(mesh.numIndices) * mesh.indexByteStride);
            save(L"post_indices.bin", ib.data(), ib.size());
        }
        Json metadata{{"vertexResourceId", resourceName(mesh.vertexResourceId)},
                      {"vertexByteOffset", mesh.vertexByteOffset},
                      {"vertexByteStride", mesh.vertexByteStride},
                      {"indexResourceId", resourceName(mesh.indexResourceId)},
                      {"indexByteOffset", mesh.indexByteOffset},
                      {"indexByteStride", mesh.indexByteStride},
                      {"baseVertex", mesh.baseVertex},
                      {"numIndices", mesh.numIndices},
                      {"topology", uint32_t(mesh.topology)},
                      {"format", rdcResourceFormat(mesh.format)},
                      {"unproject", mesh.unproject},
                      {"nearPlane", real(mesh.nearPlane)},
                      {"farPlane", real(mesh.farPlane)},
                      {"status", text(mesh.status)}};
        if (meshOutput)
            metadata.update({{"meshlets", meshlets},
                             {"position_bytes", std::to_string(vertexBytes)},
                             {"per_primitive_offset", std::to_string(mesh.perPrimitiveOffset)},
                             {"per_primitive_stride", mesh.perPrimitiveStride},
                             {"per_primitive_attributes_exported", false},
                             {"scope", "all_meshlets_positions_and_triangle_indices"}});
        const auto decoded = ReplayMesh::decode(metadata, {raw.data(), raw.size()}, {ib.data(), ib.size()});
        if (meshOutput && decoded.faces.size() != decoded.candidateFaceCount)
            throw std::runtime_error("Mesh output contains out-of-range triangle indices");
        const auto csv = decoded.csv(), obj = decoded.obj();
        save(L"post_vertices.csv", csv.data(), csv.size());
        save(L"post_geometry.obj", obj.data(), obj.size());
        result.update({{"eid", eid},
                       {"gpa_event", gpa},
                       {"stage", stage},
                       {"mesh", metadata},
                       {"vertex_count", decoded.positions.size()},
                       {"index_count", decoded.indexCount},
                       {"face_count", decoded.candidateFaceCount}});
        return complete();
    }
    if (action == "inventory") {
        Json described = Json::array(), textures = Json::array(), buffers = Json::array();
        for (const auto &r : resources)
            described.push_back(rdcResourceDescription(r));
        for (const auto &t : controller.GetTextures())
            textures.push_back(rdcTextureDescription(t));
        for (const auto &b : controller.GetBuffers())
            buffers.push_back(rdcBufferDescription(b));
        result.update({{"eid", eid},
                       {"gpa_event", gpa},
                       {"actions", mapping.at("actions")},
                       {"resources", described},
                       {"textures", textures},
                       {"buffers", buffers}});
        return complete();
    }
    if (action == "counters") {
        rdcarray<GPUCounter> selected;
        Json available = Json::array(), values = Json::array();
        std::map<GPUCounter, CounterDescription> descriptions;
        for (const auto counter : controller.EnumerateCounters()) {
            auto description = controller.DescribeCounter(counter);
            if (description.counter != counter || !descriptions.emplace(counter, description).second)
                throw std::runtime_error("Invalid or duplicate counter description");
            available.push_back(rdcCounterDescription(description));
            if (uint32_t(counter) < uint32_t(GPUCounter::FirstAMD))
                selected.push_back(counter);
        }
        const auto measured = controller.FetchCounters(selected);
        checked(controller.GetFatalErrorStatus());
        if (!selected.empty() && measured.empty())
            throw std::runtime_error("RenderDoc returned no measured counters; available descriptions are not measurements");
        for (const auto &counter : measured) {
            const auto it = descriptions.find(counter.counter);
            if (it == descriptions.end())
                throw std::runtime_error("Counter result has no description");
            const auto &description = it->second;
            Json row{{"eventId", counter.eventId},
                     {"counter", uint32_t(counter.counter)},
                     {"name", text(description.name)},
                     {"unit", rdcCounterUnit(description.unit)},
                     {"value", rdcCounterValue(description, counter)}};
            row.update(rdcProvenance(counter.eventId, mapping["reverse"]));
            values.push_back(std::move(row));
        }
        result.update({{"eid", eid},
                       {"gpa_event", gpa},
                       {"available", available},
                       {"values", values},
                       {"result_count", values.size()},
                       {"note", "Measured on the replay GPU; GPA vendor-specific metrics are not implied."}});
        return complete();
    }
    if (debugging) {
        result.update(debug(controller, eid, job, session.dxbcWorkgroupPatch));
        result["eid"] = eid;
        result["gpa_event"] = gpa;
        return complete();
    }
    ResourceId resource;
    const auto gpaResource = job.value("resource", Json(nullptr));
    if (gpaResource.is_null()) {
        const auto *pipe = controller.GetD3D11PipelineState();
        const auto *pipe12 = controller.GetD3D12PipelineState();
        if (dx12 && pipe12 && !pipe12->outputMerger.renderTargets.empty())
            resource = pipe12->outputMerger.renderTargets[0].resource;
        else if (pipe && !pipe->outputMerger.renderTargets.empty())
            resource = pipe->outputMerger.renderTargets[0].resource;
    } else {
        resource = capturedResource(resources, job, dx12);
    }
    if (resource == ResourceId::Null())
        throw std::runtime_error("Select an explicit texture resource; no matching target");
    if (action == "resource-history") {
        static constexpr const char *usageNames[]{"Unused",         "VertexBuffer",
                                                  "IndexBuffer",    "VS_Constants",
                                                  "HS_Constants",   "DS_Constants",
                                                  "GS_Constants",   "PS_Constants",
                                                  "CS_Constants",   "TS_Constants",
                                                  "MS_Constants",   "All_Constants",
                                                  "StreamOut",      "VS_Resource",
                                                  "HS_Resource",    "DS_Resource",
                                                  "GS_Resource",    "PS_Resource",
                                                  "CS_Resource",    "TS_Resource",
                                                  "MS_Resource",    "All_Resource",
                                                  "VS_RWResource",  "HS_RWResource",
                                                  "DS_RWResource",  "GS_RWResource",
                                                  "PS_RWResource",  "CS_RWResource",
                                                  "TS_RWResource",  "MS_RWResource",
                                                  "All_RWResource", "InputTarget",
                                                  "ColorTarget",    "DepthStencilTarget",
                                                  "Indirect",       "Clear",
                                                  "Discard",        "GenMips",
                                                  "Resolve",        "ResolveSrc",
                                                  "ResolveDst",     "Copy",
                                                  "CopySrc",        "CopyDst",
                                                  "Barrier",        "CPUWrite"};
        static_assert(std::size(usageNames) == size_t(ResourceUsage::CPUWrite) + 1);
        Json rows = Json::array(), initialization = Json::array();
        size_t unmapped = 0;
        for (const auto &usage : controller.GetUsage(resource)) {
            if (usage.eventId > eid)
                continue;
            if (rows.size() >= 1000000)
                throw std::runtime_error("Resource history exceeds one million uses");
            const auto type = size_t(usage.usage);
            const bool operation = usage.usage >= ResourceUsage::Clear;
            Json row{{"eventId", usage.eventId},
                     {"usage", type},
                     {"usage_name", type < std::size(usageNames) ? usageNames[type] : "Unknown"},
                     {"evidence", type >= std::size(usageNames) ? "unknown"
                                  : operation                   ? "recorded_operation"
                                                                : "bound_use"},
                     {"content_change", "not_measured"},
                     {"subresource", nullptr},
                     {"subresource_status", "not_provided_by_usage_index"}};
            row.update(rdcProvenance(usage.eventId, mapping["reverse"]));
            if (dx12 && !row.value("selectable", false))
                ++unmapped;
            rows.push_back(std::move(row));
        }
        Json name;
        const auto &structured = controller.GetStructuredFile();
        for (const auto &r : resources)
            if (r.resourceId == resource) {
                name = text(r.name);
                for (const auto chunk : r.initialisationChunks) {
                    Json entry{{"chunk_index", chunk},
                               {"scope", "replay_resource_initialization"},
                               {"execution", nullptr},
                               {"selectable", false}};
                    entry["name"] = chunk < structured.chunks.size() && structured.chunks[chunk]
                                        ? Json(text(structured.chunks[chunk]->name))
                                        : Json();
                    initialization.push_back(std::move(entry));
                }
            }
        result.update(
            {{"eid", eid},
             {"resource", resourceName(resource)},
             {"resource_name", name},
             {"resource_object", gpaResource},
             {"resource_creation", job.value("resource_creation", Json())},
             {"resource_history", rows},
             {"replay_initialization", initialization},
             {"partial", true},
             {"history_scope",
              {{"through_event", eid},
               {"unmapped_uses", unmapped},
               {"content_changes_measured", false},
               {"limitations",
                {"Bindings indicate possible use, not observed reads or writes.",
                 "Usage records do not supply subresource ranges or fence dependencies.",
                 "Replay initialization is separate from original execution markers.",
                 "Only the captured replay prefix is covered; empty usage is not proof of no access."}}}}});
        if (dx12) {
            result["history_scope"]["shader_stage_binding_status"] =
                session.dx12UsageVisibilityPatch == 1 ? "stage_visibility_matched"
                                                       : "known_stage_visibility_gap";
            if (session.dx12UsageVisibilityPatch != 1)
                result["history_scope"]["warnings"] = {
                    "This RenderDoc 1.45 usage index does not match root-parameter visibility to the "
                    "shader stage. Overlapping stage registers can omit resources or misattribute uses. "
                    "Select the patched analysis library."};
        }
        return complete();
    }
    const auto textures = controller.GetTextures();
    const auto found = std::find_if(textures.begin(), textures.end(),
                                    [&](const auto &t) { return t.resourceId == resource; });
    if (found == textures.end())
        throw std::runtime_error("Selected resource is not a texture");
    const auto &tex = *found;
    const Subresource sub(index(job, "mip"), index(job, "layer"), index(job, "sample"));
    if (action == "texture") {
        const auto description = rdcTextureReadbackDescription(tex, sub);
        const auto data = controller.GetTextureData(resource, sub);
        if (data.empty())
            throw std::runtime_error("Texture readback returned no data");
        const auto digest = QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())),
            QCryptographicHash::Sha256);
        const auto destination = path(job.at("out")) / L"texture.bin";
        QSaveFile file(QString::fromStdWString(destination.wstring()));
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(data.data()), qint64(data.size())) !=
                qint64(data.size()) ||
            !file.commit())
            throw std::runtime_error("Cannot save raw replay texture");
        Json name = nullptr;
        for (const auto &r : resources)
            if (r.resourceId == resource)
                name = text(r.name);
        result.update({{"eid", eid},
                       {"gpa_event", gpa},
                       {"resource", resourceName(resource)},
                       {"resource_name", name},
                       {"resource_object", gpaResource},
                       {"resource_creation", job.value("resource_creation", Json())},
                       {"readback", description},
                       {"sha256", digest.toHex().toStdString()},
                       {"byte_length", data.size()},
                       {"subresource", {{"mip", sub.mip}, {"slice", sub.slice}, {"sample", sub.sample}}}});
        return complete();
    }
    const auto x = index(job, "x"), y = index(job, "y");
    validate(tex, sub, x, y);
    const std::string kind = tex.format.compType == CompType::UInt   ? "uint"
                             : tex.format.compType == CompType::SInt ? "sint"
                                                                     : "float";
    Json records = Json::array(), gaps = Json::array();
    // RenderDoc 1.45's D3D12 GetDepthFormat returns the resource's R16_TYPELESS
    // format for a depth target. GetDepthValue then decodes UNORM bits as half
    // floats. Fragment values cannot be recovered reliably (NaNs lose bits,
    // and intermediate fragments take a different conversion path).
    const bool d16History = dx12 && session.version == "1.45" &&
                               bool(tex.creationFlags & TextureCategory::DepthTarget) &&
                               tex.format.type == ResourceFormatType::Regular && tex.format.compCount == 1 &&
                               tex.format.compByteWidth == 2;
    const bool unreliableD16 = d16History && session.d16HistoryPatch != 1;
    Json depthBoundaries = Json::array(), depthGaps = Json::array();
    std::set<uint32_t> existing;
    const bool depthTarget = bool(tex.creationFlags & TextureCategory::DepthTarget);
    const bool stencilPresent = tex.format.type == ResourceFormatType::D24S8 || tex.format.type == ResourceFormatType::D32S8;
    size_t reconstructedDepthValues = 0;
    result["pixel_value_kind"] = depthTarget ? "depth_stencil" : "color";
    const auto modifications = controller.PixelHistory(resource, x, y, sub, CompType::Typeless);
    for (size_t i = 0; i < modifications.size(); ++i) {
        const auto &v = modifications[i];
        auto r = record(v);
        r.update(rdcProvenance(v.eventId, mapping["reverse"]));
        r["record_kind"] = "backend_modification";
        r["color_interpretation"] = kind;
        r["pixel_value_kind"] = depthTarget ? "depth_stencil" : "color";
        r["stencil_present"] = stencilPresent;
        if (d16History && !unreliableD16) {
            // RenderDoc reconstructs intermediate fragments in a D32 scratch DSV.
            // Only the first pre/last post values use the captured D16 format.
            r["depth_value_sources"] = {
                {"preMod", i && modifications[i - 1].eventId == v.eventId
                               ? "backend_d32_intermediate" : "captured_format_event_boundary"},
                {"postMod", i + 1 < modifications.size() && modifications[i + 1].eventId == v.eventId
                                ? "backend_d32_intermediate" : "captured_format_event_boundary"}};
            r["depth_test_reason_status"] = "backend_analysis";
            for (const auto &source : r["depth_value_sources"])
                if (source == "backend_d32_intermediate") ++reconstructedDepthValues;
        }
        if (unreliableD16) {
            r["backend_depth_diagnostic"] = {{"pre", r["preMod"]["depth"]}, {"post", r["postMod"]["depth"]}};
            r["preMod"]["depth"] = nullptr;
            r["postMod"]["depth"] = nullptr;
            r["depth_value_status"] = "unavailable_backend_d16_format";
            r["depth_test_reason_status"] = "unverified_backend_d16_format";
        }
        records.push_back(std::move(r));
        existing.insert(v.eventId);
    }
    const auto parsed = writes(events);
    size_t added = 0;
    try {
        if (unreliableD16) {
            // Read actual R16 bytes at each action boundary. These observations
            // are separate from PixelHistory fragments and shader outputs.
            const uint64_t width = extent(tex.width, sub.mip), height = extent(tex.height, sub.mip);
            const bool bounded = tex.dimension == 2 && tex.msSamp <= 1 &&
                                 width * height <= 128ULL * 1024 * 1024 && existing.size() <= 256;
            uint64_t remainingReadback = 256ULL * 1024 * 1024;
            auto depthAt = [&](uint32_t event) -> Json {
                remainingReadback -= width * height * 2;
                controller.SetFrameEvent(event, true);
                const auto bytes = controller.GetTextureData(resource, sub);
                const uint64_t offset = (uint64_t(y) * width + x) * 2;
                if (bytes.size() != width * height * 2 || offset + 2 > bytes.size())
                    throw std::runtime_error("Unexpected D16 readback layout");
                const auto raw = uint16_t(bytes[size_t(offset)]) | uint16_t(bytes[size_t(offset + 1)]) << 8;
                return {{"raw_unorm16", raw}, {"depth", double(raw) / 65535.0}};
            };
            for (const auto event : existing) {
                if (!bounded || event == 0 || width * height * 4 > remainingReadback) {
                    depthGaps.push_back(
                        {{"eventId", event},
                         {"reason", "D16 boundary readback requires single-sample 2D data, at most 256 "
                                    "events and a 256 MiB total readback budget"}});
                    continue;
                }
                try {
                    auto before = depthAt(event - 1), after = depthAt(event);
                    Json observation{
                        {"eventId", event},        {"record_kind", "observed_event_depth_boundary"},
                        {"before_eid", event - 1}, {"pre", before},
                        {"post", after},           {"mip", sub.mip},
                        {"layer", sub.slice},      {"sample", sub.sample}};
                    observation.update(rdcProvenance(event, mapping["reverse"]));
                    depthBoundaries.push_back(std::move(observation));
                } catch (const std::exception &e) {
                    depthGaps.push_back({{"eventId", event}, {"reason", e.what()}});
                }
            }
        }
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
        if (r.value("mapping_status", std::string()) != "mapped")
            ++unmapped;
    result.update(
        {{"eid", eid},
         {"gpa_event", gpa},
         {"resource", resourceName(resource)},
         {"x", x},
         {"y", y},
         {"history", records},
         {"depth_event_boundaries", depthBoundaries},
         {"history_events", records.size()},
         {"history_scope",
          {{"backend", "RenderDoc PixelHistory of independent replay"},
           {"unmapped_records", unmapped},
           {"cpu_write_snapshots", added},
           {"cpu_write_gaps", gaps},
           {"depth_value_status", unreliableD16 ? "event_boundaries_only" : d16History ? "patched_backend_values" : "backend_values"},
           {"depth_boundary_gaps", depthGaps},
           {"reconstructed_depth_values", reconstructedDepthValues},
           {"unrepresented_api_commands", unrepresented},
           {"limitations",
            {"CPU writes use API-boundary pixel snapshots, not shader fragments; Map coverage is a "
             "subresource candidate.",
             "Old captures may lack GPA CPU-write markers; unknown structured layouts remain explicit gaps.",
             "Resource operations can appear with unchanged selected-subresource values; no shader fragment "
             "or test pass is implied.",
             unreliableD16 ? "Stock RenderDoc 1.45 DX12 D16 fragment depths are withheld because of typeless decoding; separate raw UNORM event boundaries do not recover fragment depths or validate depth-test reasons."
                           : d16History ? "D16 event boundaries use the Flora decoding patch. Intermediate fragment depths come from a D32 scratch DSV and may differ in quantization or depth bias; they are not observed D16 storage values. Depth-test reasons include backend comparisons of reconstructed depths, not independent per-fragment hardware queries."
                                        : "Pixel values and test-failure reasons retain RenderDoc's backend limitations."}}}}});
    result["resource_name"] = nullptr;
    if (unreliableD16 || reconstructedDepthValues)
        result["partial"] = true;
    for (const auto &r : resources)
        if (r.resourceId == resource)
            result["resource_name"] = text(r.name);
    return complete();
}
} // namespace flora
