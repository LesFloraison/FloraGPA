// Manual diagnosis of a selected texel changing during Hellblade2 history.
// MSVC C++20, include src and third_party; link bcrypt.lib. No RenderDoc import library.
// Arguments: renderdoc.dll, dxcompiler.dll, capture.rdc, terminal RDC event,
// x, y, new report.json, [--readback] [--repeat-only] [--between-event event] resource creation records...
// Two forced selections in one controller; only if both agree, query history
// on the first resource. This is bounded fault diagnosis, not a soak test.
// Textures inspect mip/layer/sample 0. Buffers require --readback and are
// bounded to 32 MiB; a first buffer never triggers PixelHistory.
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include "rdc/Api.h"
#include "../../tools/native/process_modules.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
pRENDERDOC_AllocArrayMem allocate{};
pRENDERDOC_FreeArrayMem release{};
HMODULE library{};
template<class T> T symbol(const char *name) {
    auto result = GetProcAddress(library, name);
    if (!result) throw std::runtime_error(std::string("Missing entry point: ") + name);
    return reinterpret_cast<T>(result);
}
std::string utf8(const fs::path &path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
void check(ResultDetails status) {
    if (!status.OK())
        throw std::runtime_error(status.internal_msg ? status.internal_msg->c_str() : "RenderDoc failure");
}
struct Shutdown {
    template<class T> void operator()(T *value) const { if (value) value->Shutdown(); }
};
uint32_t number(const wchar_t *text) {
    const std::wstring value(text);
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error("Expected decimal uint32");
    const auto parsed = std::stoull(value);
    if (parsed > UINT32_MAX) throw std::runtime_error("Index exceeds uint32");
    return uint32_t(parsed);
}
}
extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *mem) { if (mem) release(mem); }
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return allocate(size); }
extern "C" __declspec(dllexport) void __cdecl renderdoc__replay__marker() {}
int wmain(int argc, wchar_t **argv) try {
    if (argc < 9) throw std::runtime_error("Expected library, compiler, capture, event, x, y, report, creations");
    const auto reportPath = fs::absolute(argv[7]);
    if (fs::exists(reportPath)) throw std::runtime_error("Report must be new");
    library = LoadLibraryExW(fs::canonical(argv[1]).c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) throw std::runtime_error("Cannot load replay library");
    allocate = symbol<pRENDERDOC_AllocArrayMem>("RENDERDOC_AllocArrayMem");
    release = symbol<pRENDERDOC_FreeArrayMem>("RENDERDOC_FreeArrayMem");
    const std::string version = symbol<decltype(&RENDERDOC_GetVersionString)>("RENDERDOC_GetVersionString")();
    if (version != "1.45") throw std::runtime_error("Expected RenderDoc 1.45");
    symbol<decltype(&RENDERDOC_InitialiseReplay)>("RENDERDOC_InitialiseReplay")(GlobalEnvironment{}, {});
    struct ReplayShutdown {
        ~ReplayShutdown() { symbol<decltype(&RENDERDOC_ShutdownReplay)>("RENDERDOC_ShutdownReplay")(); }
    } shutdown;
    auto *setting = symbol<decltype(&RENDERDOC_SetConfigSetting)>("RENDERDOC_SetConfigSetting")("D3D12.DXCPath");
    if (!setting) throw std::runtime_error("Missing compiler path setting");
    setting->data.str = utf8(fs::canonical(argv[2])).c_str();
    std::unique_ptr<ICaptureFile, Shutdown> file(
        symbol<decltype(&RENDERDOC_OpenCaptureFile)>("RENDERDOC_OpenCaptureFile")());
    if (!file) throw std::runtime_error("Cannot create capture reader");
    check(file->OpenFile(utf8(fs::canonical(argv[3])).c_str(), "", {}));
    if (file->DriverName() != "D3D12") throw std::runtime_error("Expected DX12");
    ReplayOptions options;
    options.optimisation = ReplayOptimisationLevel::Balanced;
    auto opened = file->OpenCapture(options, {});
    std::unique_ptr<IReplayController, Shutdown> controller(opened.second);
    check(opened.first);
    if (!controller) throw std::runtime_error("No replay controller");
    symbol<void(RENDERDOC_CC *)(bool)>("RENDERDOC_SetFloraDx12DiscardPatternsEnabled")(false);
    const auto event = number(argv[4]), x = number(argv[5]), y = number(argv[6]);
    const auto resources = controller->GetResources();
    const auto textures = controller->GetTextures();
    const auto buffers = controller->GetBuffers();
    bool readback = false, repeatOnly = false;
    uint32_t betweenEvent{};
    int firstCreation = 8;
    while (firstCreation < argc) {
        if (std::wstring(argv[firstCreation]) == L"--readback" && !readback) {
            readback = true;
            ++firstCreation;
        } else if (std::wstring(argv[firstCreation]) == L"--repeat-only" && !repeatOnly) {
            repeatOnly = true;
            ++firstCreation;
        } else if (std::wstring(argv[firstCreation]) == L"--between-event" && !betweenEvent &&
                   firstCreation + 1 < argc) {
            betweenEvent = number(argv[firstCreation + 1]);
            if (!betweenEvent) throw std::runtime_error("Between-event must be nonzero");
            firstCreation += 2;
        } else break;
    }
    if (argc <= firstCreation || argc - firstCreation > 8)
        throw std::runtime_error("Select one to eight resources");
    std::vector<ResourceId> selected;
    std::vector<uint64_t> bufferLengths;
    Json descriptions = Json::array();
    for (int i = firstCreation; i < argc; ++i) {
        const auto creation = utf8(argv[i]);
        const auto suffix = " creation " + creation;
        ResourceId found;
        for (const auto &resource : resources) {
            const std::string name(resource.name.c_str());
            if (name.starts_with("Flora DX12 resource ") && name.ends_with(suffix)) {
                if (found != ResourceId::Null()) throw std::runtime_error("Ambiguous resource lifetime");
                found = resource.resourceId;
                descriptions.push_back({{"creation", creation}, {"name", name}});
            }
        }
        if (found == ResourceId::Null()) throw std::runtime_error("Missing resource lifetime");
        bool valid = false;
        uint64_t bufferLength{};
        for (const auto &texture : textures)
            if (texture.resourceId == found) {
                valid = texture.dimension == 2 && x < texture.width && y < texture.height && texture.msSamp <= 1 &&
                    (!readback || (texture.byteSize && texture.byteSize <= 32ULL * 1024 * 1024));
                descriptions.back().update({{"kind", "texture"}, {"width", texture.width}, {"height", texture.height},
                    {"array_size", texture.arraysize}, {"mips", texture.mips}, {"samples", texture.msSamp},
                    {"selected_subresource", {{"mip", 0}, {"layer", 0}, {"sample", 0}}},
                    {"format_type", uint32_t(texture.format.type)}, {"component_count", texture.format.compCount},
                    {"component_bytes", texture.format.compByteWidth}});
            }
        for (const auto &buffer : buffers)
            if (buffer.resourceId == found) {
                bufferLength = buffer.length;
                valid = readback && bufferLength && bufferLength <= 32ULL * 1024 * 1024;
                descriptions.back().update({{"kind", "buffer"}, {"byte_offset", "0"},
                    {"byte_length", std::to_string(bufferLength)}});
            }
        if (!valid) throw std::runtime_error("Expected in-bounds single-sample 2D texture or bounded buffer with --readback");
        selected.push_back(found);
        bufferLengths.push_back(bufferLength);
    }
    Json readbackFiles = Json::array();
    auto observe = [&](const char *phase) {
        Json values = Json::array();
        for (size_t i = 0; i < selected.size(); ++i) {
            const auto resource = selected[i];
            values.push_back(Json::object());
            if (!bufferLengths[i]) {
                const auto value = controller->PickPixel(resource, x, y, Subresource(0, 0, 0), CompType::Typeless);
                values.back().update({{"float", {value.floatValue[0], value.floatValue[1], value.floatValue[2], value.floatValue[3]}},
                    {"bits", {value.uintValue[0], value.uintValue[1], value.uintValue[2], value.uintValue[3]}}});
            }
            if (readback) {
                const auto bytes = bufferLengths[i] ? controller->GetBufferData(resource, 0, bufferLengths[i])
                    : controller->GetTextureData(resource, Subresource(0, 0, 0));
                if (bytes.empty() || bytes.size() > 32ULL * 1024 * 1024)
                    throw std::runtime_error("Unexpected bounded resource readback");
                if (bufferLengths[i] && bytes.size() != bufferLengths[i])
                    throw std::runtime_error("Incomplete bounded buffer readback");
                unsigned char hash[32]{};
                if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                               ULONG(bytes.size()), hash, sizeof(hash)) < 0)
                    throw std::runtime_error("Cannot hash readback");
                std::ostringstream digest;
                for (const auto byte : hash) digest << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
                values.back().update({{"storage_sha256", digest.str()}, {"bytes", std::to_string(bytes.size())}});
                const auto name = reportPath.stem().string() + "-" + phase + "-" +
                    descriptions[i].at("creation").get<std::string>() + ".bin";
                const auto destination = reportPath.parent_path() / name;
                if (fs::exists(destination)) throw std::runtime_error("Readback output must be new");
                std::ofstream raw(destination, std::ios::binary);
                raw.exceptions(std::ios::badbit | std::ios::failbit);
                raw.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
                raw.close();
                readbackFiles.push_back({{"phase", phase}, {"creation", descriptions[i].at("creation")},
                    {"file", utf8(destination)}});
            }
        }
        return values;
    };
    Json report{{"scope", "Two forced selections in one controller; conditional PixelHistory. No native/original-frame fidelity claim."},
        {"capture", utf8(fs::canonical(argv[3]))}, {"event", event}, {"x", x}, {"y", y},
        {"resources", descriptions}, {"history_requested", false},
        {"repeat_only", repeatOnly},
        {"between_event", betweenEvent ? Json(betweenEvent) : Json()}};
    std::function<void(const rdcarray<ActionDescription> &)> inspectActions;
    inspectActions = [&](const auto &actions) {
        for (const auto &action : actions) {
            const std::string name(action.customName.c_str());
            if (action.eventId == event) report["selection_action_name"] = name;
            if (name.starts_with("Flora DX12 execution ") &&
                (!report.contains("first_execution_marker") ||
                 action.eventId < report["first_execution_marker"]["event"].get<uint32_t>()))
                report["first_execution_marker"] = {{"event", action.eventId}, {"name", name}};
            inspectActions(action.children);
        }
    };
    inspectActions(controller->GetRootActions());
    controller->SetFrameEvent(event, true);
    check(controller->GetFatalErrorStatus());
    report["first"] = observe("first");
    check(controller->GetFatalErrorStatus());
    if (betweenEvent) {
        controller->SetFrameEvent(betweenEvent, true);
        check(controller->GetFatalErrorStatus());
    }
    controller->SetFrameEvent(event, true);
    check(controller->GetFatalErrorStatus());
    report["second"] = observe("second");
    check(controller->GetFatalErrorStatus());
    report["repeat_equal"] = report["first"] == report["second"];
    // An explicit round trip tests restoration only; do not add unrelated
    // PixelHistory work at an initialization boundary.
    if (report["repeat_equal"] == true && !betweenEvent && !repeatOnly && !bufferLengths.front()) {
        report["history_requested"] = true;
        const auto modifications = controller->PixelHistory(selected.front(), x, y, Subresource(0, 0, 0), CompType::Typeless);
        report["history_events"] = modifications.size();
        controller->SetFrameEvent(event, true);
        report["after_history"] = observe("after-history");
        check(controller->GetFatalErrorStatus());
    }
    report["readback_files"] = readbackFiles;
    report["loaded_modules"] = flora::diagnostic::processModules();
    std::ofstream output(reportPath, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << report.dump(2) << '\n';
    output.close();
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
