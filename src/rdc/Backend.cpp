#include "Backend.h"
#include "Api.h"
#include "Assets.h"
#include "Counters.h"
#include "DebugTrace.h"
#include "application/RdcEvents.h"
#include "application/ReplayMesh.h"
#include <QSaveFile>
#define NOMINMAX
#include <Windows.h>

#include <Psapi.h>
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
                throw std::runtime_error("Native analysis requires a DX11 independent replay capture");
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
Json debug(IReplayController &controller, uint32_t eid, const Json &job) {
    const auto kind = job.at("action").get<std::string>();
    const auto *selected = findAction(controller.GetRootActions(), eid);
    const bool compute = kind == "debug-thread", vertex = kind == "debug-vertex";
    if (!selected || !(selected->flags & (compute ? ActionFlags::Dispatch : ActionFlags::Drawcall)))
        throw std::runtime_error(compute ? "Compute debugging requires an executed dispatch action"
                                         : "Selected analysis requires an executed draw action");
    const auto *pipe = controller.GetD3D11PipelineState();
    if (!pipe)
        throw std::runtime_error("No D3D11 pipeline at the selected event");
    const auto *reflection = compute  ? pipe->computeShader.reflection
                             : vertex ? pipe->vertexShader.reflection
                                      : pipe->pixelShader.reflection;
    if (!reflection)
        throw std::runtime_error("No shader bound at the selected stage");
    if (!reflection->debugInfo.debuggable)
        throw std::runtime_error("Shader cannot be debugged: " + text(reflection->debugInfo.debugStatus));
    Json result = Json::object();
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
            const auto &ib = pipe->inputAssembly.indexBuffer;
            if (ib.byteStride != 1 && ib.byteStride != 2 && ib.byteStride != 4)
                throw std::runtime_error("Invalid index buffer stride");
            const auto address = uint64_t(ib.byteOffset) +
                                 (uint64_t(selected->indexOffset) + index(job, "vertex")) * ib.byteStride;
            const auto bytes = controller.GetBufferData(ib.resourceId, address, ib.byteStride);
            if (bytes.size() != ib.byteStride)
                throw std::runtime_error("Selected vertex index is outside the index buffer");
            uint32_t raw = 0;
            memcpy(&raw, bytes.data(), ib.byteStride);
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
        trace.reset(controller.DebugPixel(index(job, "x"), index(job, "y"), inputs));
    }
    if (!trace || !trace->debugger)
        throw std::runtime_error("No debugger trace at the selected invocation");
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
    // D3D11 has no monolithic pipeline object, matching PipeState's null ID.
    const auto assembly = controller.DisassembleShader(ResourceId::Null(), reflection, "");
    auto exportedAssembly = QByteArray(assembly.c_str(), qsizetype(assembly.size()));
    exportedAssembly.replace("\n", "\r\n");
    QSaveFile file(QString::fromStdString(job.at("out").get<std::string>()) + "/debug_shader.asm");
    if (!file.open(QIODevice::WriteOnly) || file.write(exportedAssembly) != exportedAssembly.size() ||
        !file.commit())
        throw std::runtime_error("Cannot export shader disassembly");
    result["disassembly"] = text(assembly);
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
    const bool debugging = action == "debug-pixel" || action == "debug-vertex" || action == "debug-thread";
    if (action != "history" && action != "events" && action != "counters" && action != "inventory" &&
        action != "texture" && action != "postmesh" && !debugging)
        throw std::runtime_error("Unsupported native RenderDoc job action");
    if (action != "events") {
        if (!job.value("gpa_event", Json(nullptr)).is_null() && !job.value("eid", Json(nullptr)).is_null())
            throw std::runtime_error("Choose GPA or RDC event, not both");
        for (const auto *key : {"x", "y", "mip", "layer", "sample", "eid", "vertex", "instance", "index"})
            index(job, key);
        for (const auto *key : {"gpa_event", "resource"})
            if (job.contains(key) && !job.at(key).is_null())
                number(job.at(key));
        if (debugging) {
            coordinates(job, "group");
            coordinates(job, "thread");
        }
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
    // Match the original audit point before the replay controller is shut down.
    auto complete = [&] {
        result["loaded_modules"] = rdcLoadedModules();
        return std::move(result);
    };
    if (action == "events") {
        result["index"] = mapping;
        result["cpu_writes"] = writes(events);
        return complete();
    }
    const auto resources = controller.GetResources();
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
    if (!eid && action == "inventory" && !events.chunks.empty())
        eid = events.chunks.rbegin()->first;
    if ((!eid || (!events.actions.contains(eid) && !events.chunks.contains(eid))) &&
        !(eid == 0 && action == "inventory"))
        throw std::runtime_error("No selectable replay action");
    controller.SetFrameEvent(eid, true);
    if (action == "postmesh") {
        const auto *selected = findAction(controller.GetRootActions(), eid);
        if (!selected || !(selected->flags & ActionFlags::Drawcall))
            throw std::runtime_error("Selected analysis requires an executed draw action");
        const auto stage = job.value("stage", std::string("VSOut"));
        const auto nativeStage = stage == "VSOut"   ? MeshDataStage::VSOut
                                 : stage == "GSOut" ? MeshDataStage::GSOut
                                                    : MeshDataStage::VSIn;
        if (stage != "VSOut" && stage != "GSOut" && stage != "VSIn")
            throw std::runtime_error("Unsupported DX11 post-shader stage");
        const auto mesh = controller.GetPostVSData(index(job, "instance"), 0, nativeStage);
        if (mesh.vertexResourceId == ResourceId::Null())
            throw std::runtime_error("Selected event/stage has no post-shader mesh data");
        auto save = [&](const wchar_t *name, const void *bytes, size_t size) {
            QSaveFile file(QString::fromStdWString((path(job.at("out")) / name).wstring()));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(static_cast<const char *>(bytes), qint64(size)) != qint64(size) || !file.commit())
                throw std::runtime_error("Cannot save post-shader mesh artifact");
        };
        const auto raw =
            controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, mesh.vertexByteSize);
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
        const auto decoded = ReplayMesh::decode(metadata, {raw.data(), raw.size()}, {ib.data(), ib.size()});
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
        for (const auto &counter : controller.FetchCounters(selected)) {
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
        result.update(debug(controller, eid, job));
        result["eid"] = eid;
        result["gpa_event"] = gpa;
        return complete();
    }
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
    if (action == "texture") {
        const auto data = controller.GetTextureData(resource, sub);
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
    return complete();
}
} // namespace flora
