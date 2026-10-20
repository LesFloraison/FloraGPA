#include "Backend.h"
#include "Api.h"
#include "Assets.h"
#include "ClearCoverage.h"
#include "TransferCoverage.h"
#include "OperationRanges.h"
#include "Counters.h"
#include "DebugTrace.h"
#include "Invocation.h"
#include "InputMesh.h"
#include "PipelineState.h"
#include "application/RdcEvents.h"
#include "application/RdcJobs.h"
#include "application/ReplayMesh.h"
#include "application/ReplayRequest.h"
#include "application/DxcRuntime.h"
#include <QCoreApplication>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#define NOMINMAX
#include <Windows.h>

#include "application/ProcessModules.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
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
    QString failureLog_;

    void preserveFailureLog() noexcept {
        // ShutdownReplay may remove its temporary log. Preserve a bounded tail
        // before closing so an isolated failed analysis remains diagnosable.
        try {
            const auto getLog = reinterpret_cast<decltype(&RENDERDOC_GetLogFile)>(
                GetProcAddress(library, "RENDERDOC_GetLogFile"));
            if (!getLog || failureLog_.isEmpty()) return;
            QFile source(QString::fromUtf8(getLog()));
            if (!source.open(QIODevice::ReadOnly)) return;
            constexpr qint64 limit = 1024 * 1024;
            const bool truncated = source.size() > limit;
            if (truncated && !source.seek(source.size() - limit)) return;
            auto bytes = source.read(limit);
            if (truncated) bytes.prepend("[Earlier RenderDoc diagnostics omitted; last 1 MiB follows]\n");
            QSaveFile destination(failureLog_);
            if (destination.open(QIODevice::WriteOnly) && destination.write(bytes) == bytes.size() && destination.commit())
                fprintf(stderr, "RenderDoc diagnostics: %s\n", failureLog_.toUtf8().constData());
        } catch (...) {
            // Diagnostics must not replace the original backend exception.
        }
    }

  public:
    std::unique_ptr<IReplayController, Shutdown> controller;
    std::string version, commit;
    Json dxcRuntime;
    uint32_t d16HistoryPatch{};
    uint32_t dxbcWorkgroupPatch{};
    uint32_t dx12UsageVisibilityPatch{};
    uint32_t readOnlyDepthSnapshotPatch{};
    uint32_t depthRoundtripPatch{};
    uint32_t dxilInstructionFlagsPatch{};
    uint32_t dxbcGpuDotPatch{};
    uint32_t dxbcGpuMadPatch{};
    uint32_t dxilContractPatch{};
    uint32_t retainedComputePatch{};
    uint32_t uavTexturePatch{};
    uint32_t textureStoresPatch{};
    uint32_t packedStorePatch{};
    uint32_t preparedComputePatch{};
    uint32_t placedHeapRestorePatch{};
    uint32_t discardLayoutPatch{};
    uint32_t discardPatternControlPatch{};
    uint32_t cubeCoordinatesPatch{};
    uint32_t vertexInputsPatch{};
    uint32_t halfColorHistoryPatch{};
    uint32_t resolveHistoryPatch{};
    uint32_t defaultViewHistoryPatch{};
    uint32_t routedLayerHistoryPatch{};
    uint32_t layerTestHistoryPatch{};
    uint32_t bgraHistoryPatch{};
    uint32_t emptyPixelInputsPatch{};
    uint32_t uploadWriteWatchPatch{};
    using LayerTestStats = void (RENDERDOC_CC *)(uint32_t *, uint32_t *, uint32_t *);
    LayerTestStats layerTestStats{};
    using SetDiscardPatterns = void (RENDERDOC_CC *)(bool);
    using DiscardPatternCount = uint64_t (RENDERDOC_CC *)();
    SetDiscardPatterns setDiscardPatterns{};
    DiscardPatternCount discardPatternsSkipped{};
    bool computePrepared{};
    using PrepareCompute = uint32_t (RENDERDOC_CC *)(IReplayController *, uint32_t);
    PrepareCompute prepareCompute{};
    using TextureStoreCount = uint32_t (RENDERDOC_CC *)(ShaderDebugger *);
    using TextureStoreRead = bool (RENDERDOC_CC *)(ShaderDebugger *, uint32_t, uint32_t *, uint32_t);
    TextureStoreCount textureStoreCount{};
    TextureStoreRead textureStoreRead{};
    using DepthSnapshotCopies = uint64_t (RENDERDOC_CC *)();
    DepthSnapshotCopies depthSnapshotCopies{};
    DepthSnapshotCopies depthRoundtripCopies{};
    using SetDepthRoundtripEvent = void (RENDERDOC_CC *)(uint32_t);
    SetDepthRoundtripEvent setDepthRoundtripEvent{};
    using DotCounter = uint64_t (RENDERDOC_CC *)();
    DotCounter dotOperations{}, dotFailures{};
    DotCounter madOperations{}, madFailures{};
    DotCounter contractOperations{}, contractFailures{};
    DotCounter retainedComputeTraces{};
    DotCounter uavTextureReads{}, uavTextureFailures{};
    DotCounter packedStores{};
    DotCounter preparedComputeConsumed{};
    explicit Session(const Json &job) {
        failureLog_ = QString::fromStdString(job.at("out").get<std::string>()) + "/backend-diagnostic.log";
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
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxilInstructionFlagsPatchVersion"))) {
            dxilInstructionFlagsPatch = patch();
            if (dxilInstructionFlagsPatch != 1) throw std::runtime_error("Unsupported DXIL instruction flags patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraD16HistoryPatchVersion")))
            d16HistoryPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxbcWorkgroupPatchVersion")))
            dxbcWorkgroupPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12UsageVisibilityPatchVersion")))
            dx12UsageVisibilityPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraReadOnlyDepthSnapshotPatchVersion")))
            readOnlyDepthSnapshotPatch = patch();
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxbcGpuDotPatchVersion"))) {
            dxbcGpuDotPatch = patch();
            if (dxbcGpuDotPatch != 1)
                throw std::runtime_error("Unsupported Flora DXBC GPU dot patch version");
            dotOperations = symbol<DotCounter>("RENDERDOC_GetFloraDxbcGpuDotOperations");
            dotFailures = symbol<DotCounter>("RENDERDOC_GetFloraDxbcGpuDotFailures");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxbcGpuMadPatchVersion"))) {
            dxbcGpuMadPatch = patch();
            if (dxbcGpuMadPatch != 1)
                throw std::runtime_error("Unsupported Flora DXBC GPU multiply-add patch version");
            madOperations = symbol<DotCounter>("RENDERDOC_GetFloraDxbcGpuMadOperations");
            madFailures = symbol<DotCounter>("RENDERDOC_GetFloraDxbcGpuMadFailures");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxilContractPatchVersion"))) {
            dxilContractPatch = patch();
            if (dxilContractPatch != 1) throw std::runtime_error("Unsupported DXIL contraction patch version");
            contractOperations = symbol<DotCounter>("RENDERDOC_GetFloraDxilContractOperations");
            contractFailures = symbol<DotCounter>("RENDERDOC_GetFloraDxilContractFailures");
        }
        if (job.value("dxil_fast_contraction", false)) {
            if (dxilContractPatch != 1) throw std::runtime_error("Fast contraction requires the Flora DXIL contraction patch");
            using Enable = void (RENDERDOC_CC *)(bool);
            symbol<Enable>("RENDERDOC_SetFloraDxilContractEnabled")(true);
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraRetainedComputePatchVersion"))) {
            retainedComputePatch = patch();
            if (retainedComputePatch != 1 && retainedComputePatch != 2)
                throw std::runtime_error("Unsupported DXIL retained compute patch version");
            retainedComputeTraces = symbol<DotCounter>("RENDERDOC_GetFloraRetainedComputeTraces");
            using Enable = void (RENDERDOC_CC *)(bool);
            // Version 1 misses ReplayController's post-DebugThread replay. Keep its
            // retention disabled while allowing unrelated analysis and DX11 jobs.
            symbol<Enable>("RENDERDOC_SetFloraRetainedComputeEnabled")(
                retainedComputePatch == 2 && job.value("action", "") == "debug-thread");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraPreparedComputePatchVersion"))) {
            preparedComputePatch = patch();
            if (preparedComputePatch != 1) throw std::runtime_error("Unsupported prepared compute patch version");
            prepareCompute = symbol<PrepareCompute>("RENDERDOC_PrepareFloraComputeDebug");
            preparedComputeConsumed = symbol<DotCounter>("RENDERDOC_GetFloraPreparedComputeConsumed");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12PlacedHeapRestorePatchVersion"))) {
            placedHeapRestorePatch = patch();
            if (placedHeapRestorePatch != 1) throw std::runtime_error("Unsupported placed heap restoration patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12DiscardLayoutPatchVersion"))) {
            discardLayoutPatch = patch();
            if (discardLayoutPatch != 1) throw std::runtime_error("Unsupported discard layout patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12DiscardPatternControlVersion"))) {
            discardPatternControlPatch = patch();
            if (discardPatternControlPatch != 1) throw std::runtime_error("Unsupported discard pattern control version");
            setDiscardPatterns = symbol<SetDiscardPatterns>("RENDERDOC_SetFloraDx12DiscardPatternsEnabled");
            discardPatternsSkipped = symbol<DiscardPatternCount>("RENDERDOC_GetFloraDx12DiscardPatternsSkipped");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12CubeCoordinatesPatchVersion"))) {
            cubeCoordinatesPatch = patch();
            if (cubeCoordinatesPatch != 1) throw std::runtime_error("Unsupported DX12 cube-coordinate patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12VertexInputsPatchVersion"))) {
            vertexInputsPatch = patch();
            if (vertexInputsPatch != 1) throw std::runtime_error("Unsupported DX12 vertex-input patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12HalfColorHistoryPatchVersion"))) {
            halfColorHistoryPatch = patch();
            if (halfColorHistoryPatch != 1 && halfColorHistoryPatch != 2)
                throw std::runtime_error("Unsupported DX12 half-color history patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12ResolveHistoryPatchVersion"))) {
            resolveHistoryPatch = patch();
            if (resolveHistoryPatch != 1) throw std::runtime_error("Unsupported DX12 Resolve history patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12DefaultViewHistoryPatchVersion"))) {
            defaultViewHistoryPatch = patch();
            if (defaultViewHistoryPatch != 1 && defaultViewHistoryPatch != 2)
                throw std::runtime_error("Unsupported DX12 default-view history patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12RoutedLayerHistoryPatchVersion"))) {
            routedLayerHistoryPatch = patch();
            if (routedLayerHistoryPatch != 1)
                throw std::runtime_error("Unsupported DX12 routed-layer history patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12LayerTestHistoryPatchVersion"))) {
            layerTestHistoryPatch = patch();
            if (layerTestHistoryPatch != 1)
                throw std::runtime_error("Unsupported DX12 layer-test history patch version");
            layerTestStats = symbol<LayerTestStats>("RENDERDOC_GetFloraDx12LayerTestHistoryStats");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12BgraHistoryPatchVersion"))) {
            bgraHistoryPatch = patch();
            if (bgraHistoryPatch != 1) throw std::runtime_error("Unsupported DX12 BGRA history patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12EmptyPixelInputsPatchVersion"))) {
            emptyPixelInputsPatch = patch();
            if (emptyPixelInputsPatch != 1) throw std::runtime_error("Unsupported DX12 empty pixel inputs patch version");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDx12UploadWriteWatchPatchVersion"))) {
            uploadWriteWatchPatch = patch();
            if (uploadWriteWatchPatch != 1) throw std::runtime_error("Unsupported DX12 upload write-watch patch version");
        }
        if (job.value("readonly_depth_snapshot", false)) {
            if (readOnlyDepthSnapshotPatch != 1)
                throw std::runtime_error("Read-only depth snapshots require the Flora RenderDoc snapshot patch version 1");
            using Enable = void (RENDERDOC_CC *)(bool);
            symbol<Enable>("RENDERDOC_SetFloraReadOnlyDepthSnapshotEnabled")(true);
            depthSnapshotCopies = symbol<DepthSnapshotCopies>("RENDERDOC_GetFloraReadOnlyDepthSnapshotCopies");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDepthRoundtripPatchVersion")))
            depthRoundtripPatch = patch();
        if (job.value("depth_roundtrip", false)) {
            if (depthRoundtripPatch != 1 && depthRoundtripPatch != 2)
                throw std::runtime_error("Depth roundtrip requires the Flora RenderDoc depth roundtrip patch version 1 or 2 (read-only D32 requires version 2)");
            setDepthRoundtripEvent = symbol<SetDepthRoundtripEvent>("RENDERDOC_SetFloraDepthRoundtripEvent");
            depthRoundtripCopies = symbol<DepthSnapshotCopies>("RENDERDOC_GetFloraDepthRoundtripCopies");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxilUavTexturePatchVersion"))) {
            uavTexturePatch = patch();
            if (uavTexturePatch != 1) throw std::runtime_error("Unsupported DXIL UAV texture patch version");
            uavTextureReads = symbol<DotCounter>("RENDERDOC_GetFloraDxilUavTextureReads");
            uavTextureFailures = symbol<DotCounter>("RENDERDOC_GetFloraDxilUavTextureFailures");
        }
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxilPackedStorePatchVersion"))) {
            packedStorePatch = patch();
            if (packedStorePatch != 1) throw std::runtime_error("Unsupported DXIL packed store patch version");
            packedStores = symbol<DotCounter>("RENDERDOC_GetFloraDxilPackedStores");
        }
        allocate = symbol<pRENDERDOC_AllocArrayMem>("RENDERDOC_AllocArrayMem");
        if (const auto patch = reinterpret_cast<PatchVersion>(GetProcAddress(library, "RENDERDOC_GetFloraDxilTextureStoresPatchVersion"))) {
            textureStoresPatch = patch();
            if (textureStoresPatch != 1) throw std::runtime_error("Unsupported DXIL texture store observer version");
            textureStoreCount = symbol<TextureStoreCount>("RENDERDOC_GetFloraDxilTextureStoreCount");
            textureStoreRead = symbol<TextureStoreRead>("RENDERDOC_GetFloraDxilTextureStore");
        }
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
            if (file->DriverName() == "D3D12") {
                const auto compiler = flora::requireDxcRuntime(
                    QDir(QCoreApplication::applicationDirPath()).filePath("DXCompiler/dxcompiler.dll"));
                auto *setting = symbol<decltype(&RENDERDOC_SetConfigSetting)>(
                    "RENDERDOC_SetConfigSetting")("D3D12.DXCPath");
                if (!setting || setting->type.basetype != SDBasic::String)
                    throw std::runtime_error("RenderDoc does not expose the DX12 compiler path setting");
                // Process-local only. Do not persist over the user's RenderDoc
                // settings or rely on its SDK-registry / plugin / PATH search.
                setting->data.str = compiler.toUtf8().constData();
                dxcRuntime = {{"library", compiler.toStdString()},
                    {"release", flora::dxcRuntimeRelease}, {"sha256", flora::dxcRuntimeSha256},
                    {"selection", "D3D12.DXCPath"},
                    {"scope", "Pinned before replay device creation; applies to RenderDoc-generated DXIL helpers, not original captured shaders"}};
            }
            ReplayOptions options;
            static_assert(uint32_t(ReplayOptimisationLevel::NoOptimisation) == 0 &&
                          uint32_t(ReplayOptimisationLevel::Conservative) == 1 &&
                          uint32_t(ReplayOptimisationLevel::Balanced) == 2 &&
                          uint32_t(ReplayOptimisationLevel::Fastest) == 3);
            options.optimisation = ReplayOptimisationLevel(flora::rdcReplayOptimisation(job));
            options.apiValidation = job.value("api_validation", false);
            auto opened = file->OpenCapture(options, {});
            controller.reset(opened.second);
            checked(opened.first);
            if (!controller)
                throw std::runtime_error("RenderDoc did not create a replay controller");
            const auto api = controller->GetAPIProperties().pipelineType;
            if (api == GraphicsAPI::D3D12) {
                const bool patterns = job.value("discard_patterns", false);
                if (setDiscardPatterns) setDiscardPatterns(patterns);
                else if (!patterns && options.optimisation != ReplayOptimisationLevel::Fastest)
                    throw std::runtime_error("Native DX12 discard replay requires the Flora discard-pattern control patch version 1; select a current patched library");
            } else if (job.value("discard_patterns", false))
                throw std::runtime_error("Diagnostic discard pattern selection requires DX12");
            if (job.value("readonly_depth_snapshot", false) && api != GraphicsAPI::D3D12)
                throw std::runtime_error("Read-only depth snapshots require a DX12 independent capture");
            if (job.value("depth_roundtrip", false) && api != GraphicsAPI::D3D12)
                throw std::runtime_error("Depth roundtrip requires a DX12 independent capture");
            if (job.value("dxil_fast_contraction", false) && api != GraphicsAPI::D3D12)
                throw std::runtime_error("Fast contraction requires a DX12 independent capture");
            if (api != GraphicsAPI::D3D11 && api != GraphicsAPI::D3D12)
                throw std::runtime_error(
                    "Native analysis requires a DX11 or DX12 independent replay capture");
            const auto backend =
                api == GraphicsAPI::D3D12 ? flora::ReplayBackend::Dx12 : flora::ReplayBackend::Dx11;
            if (job.contains("request_identity") &&
                flora::parseReplayIdentity(job.at("request_identity")).backend != backend)
                throw std::runtime_error("Analysis request backend does not match the independent capture");
        } catch (...) {
            preserveFailureLog();
            const auto log = reinterpret_cast<decltype(&RENDERDOC_GetLogFile)>(
                GetProcAddress(library, "RENDERDOC_GetLogFile"));
            if (log)
                fprintf(stderr, "RenderDoc diagnostic log: %s\n", log());
            close();
            throw;
        }
    }
    Json compilerInformation() const {
        if (dxcRuntime.is_null()) return nullptr;
        auto result = dxcRuntime;
        const auto module = GetModuleHandleW(L"dxcompiler.dll");
        result["status"] = module ? "loaded" : "configured_not_loaded";
        if (module) {
            wchar_t actual[32768]{};
            const auto count = GetModuleFileNameW(module, actual, 32768);
            if (!count || count >= 32768 ||
                !std::filesystem::equivalent(path(result.at("library")), actual))
                throw std::runtime_error("DX12 analysis loaded a different DXC runtime");
            result["loaded_library"] = QString::fromWCharArray(actual).toStdString();
        }
        return result;
    }
    void close() noexcept {
        controller.reset();
        if (initialized_) {
            symbol<decltype(&RENDERDOC_ShutdownReplay)>("RENDERDOC_ShutdownReplay")();
            initialized_ = false;
        }
    }
    ~Session() {
        if (std::uncaught_exceptions()) preserveFailureLog();
        close();
    }
};
struct Events {
    Json native = Json::object();
    std::map<uint32_t, const SDObject *> chunks;
    std::set<uint32_t> actions;
    std::map<uint32_t, const ActionDescription *> actionDetails;
    size_t visited{};
    Json walk(const rdcarray<ActionDescription> &nodes, const SDFile &file, unsigned depth = 0) {
        if (depth > 128)
            throw std::runtime_error("RenderDoc action tree is too deep");
        Json roots = Json::array();
        for (const auto &node : nodes) {
            if (++visited > 1000000)
                throw std::runtime_error("Too many RenderDoc actions");
            actions.insert(node.eventId);
            actionDetails.emplace(node.eventId, &node);
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
Json indirectDispatchInputs(IReplayController &controller, const Events &events,
                            const Json &provenance, const ActionDescription &action) {
    Json result{{"status", "unavailable"}, {"event_id", action.eventId},
        {"indexed_groups", {action.dispatchDimension[0], action.dispatchDimension[1], action.dispatchDimension[2]}},
        {"scope", "Argument buffer after selected-event replay. This observes buffer contents, not the arguments consumed by the GPU; subsequent analysis may replay the prefix again."}};
    try {
        const SDObject *call{};
        for (const auto &eid : provenance.at("api_event_ids")) {
            const auto found = events.chunks.find(eid.get<uint32_t>());
            if (found != events.chunks.end() && text(found->second->name).find("ExecuteIndirect") != std::string::npos) {
                call = found->second; break;
            }
        }
        if (!call) throw std::runtime_error("Original ExecuteIndirect API chunk is unavailable");
        const auto signature = field(*call, "pCommandSignature");
        const auto argument = field(*call, "pArgumentBuffer");
        const auto offset = field(*call, "ArgumentBufferOffset");
        const auto maximum = field(*call, "MaxCommandCount");
        const auto counter = field(*call, "pCountBuffer");
        const auto counterOffset = field(*call, "CountBufferOffset");
        const auto resources = controller.GetResources();
        const auto &structured = controller.GetStructuredFile();
        const SDObject *description{};
        for (const auto &resource : resources) {
            if (id(resource.resourceId) != signature) continue;
            for (const auto chunk : resource.initialisationChunks) {
                if (chunk >= structured.chunks.size() || !structured.chunks[chunk]) continue;
                const auto &entry = *structured.chunks[chunk];
                if (text(entry.name).find("CreateCommandSignature") != std::string::npos)
                    description = &child(entry, "pDesc");
            }
        }
        if (!description) throw std::runtime_error("Indirect signature description is unavailable");
        const auto &arguments = child(*description, "pArgumentDescs");
        // D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH = 2. Other layouts need separate decoding.
        if (field(*description, "NumArgumentDescs") != 1 || arguments.NumChildren() != 1 || !arguments.GetChild(0) ||
            field(*arguments.GetChild(0), "Type") != 2)
            throw std::runtime_error("Argument snapshot supports dispatch-only signatures");
        const auto stride = field(*description, "ByteStride");
        // D3D12 drawIndex is the argument's position in the signature, not
        // the command's ordinal. Check our execution mapping against the
        // synthetic subcommand recorded by RenderDoc before reading its bytes.
        auto selectedProvenance = provenance;
        selectedProvenance["eid"] = action.eventId;
        const auto ordinal = flora::indirectActionOrdinal(selectedProvenance);
        const auto subcommand = events.chunks.find(action.eventId);
        if (ordinal < 0 || subcommand == events.chunks.end() ||
            field(*subcommand->second, "CommandIndex") != uint64_t(ordinal) ||
            field(*subcommand->second, "ArgumentIndex") != 0)
            throw std::runtime_error("Indirect command index differs from its execution mapping");
        const auto commandIndex = uint64_t(ordinal);
        if (stride < 12 || stride % 4 || offset % 4 || commandIndex >= maximum ||
            commandIndex > (UINT64_MAX - offset) / stride)
            throw std::runtime_error("Invalid indirect argument range");
        const auto buffers = controller.GetBuffers();
        auto read = [&](uint64_t resourceId, uint64_t start, uint64_t length) {
            const auto buffer = std::find_if(buffers.begin(), buffers.end(),
                [&](const auto &b) { return id(b.resourceId) == resourceId; });
            if (buffer == buffers.end() || start > buffer->length || length > buffer->length - start)
                throw std::runtime_error("Indirect argument read exceeds buffer");
            const auto bytes = controller.GetBufferData(buffer->resourceId, start, length);
            if (bytes.size() != length) throw std::runtime_error("Incomplete indirect argument readback");
            return bytes;
        };
        uint64_t count = maximum;
        if (counter) {
            const auto bytes = read(counter, counterOffset, 4);
            uint32_t value{}; memcpy(&value, bytes.data(), 4);
            count = (std::min)(count, uint64_t(value));
            result["count_buffer_value"] = value;
        }
        const auto selectedOffset = offset + commandIndex * stride;
        const auto bytes = read(argument, selectedOffset, 12);
        uint32_t groups[3]{}; memcpy(groups, bytes.data(), 12);
        result.update({{"status", "read"}, {"argument_resource", std::to_string(argument)},
            {"argument_offset", std::to_string(selectedOffset)}, {"signature_resource", std::to_string(signature)},
            {"stride", std::to_string(stride)}, {"maximum_count", maximum}, {"effective_count", count},
            {"action_index", commandIndex}, {"action_present", commandIndex < count},
            {"groups", {groups[0], groups[1], groups[2]}},
            {"matches_index", groups[0] == action.dispatchDimension[0] && groups[1] == action.dispatchDimension[1] && groups[2] == action.dispatchDimension[2]}});
        for (const auto &resource : resources)
            if (id(resource.resourceId) == argument) {
                result["argument_buffer"] = flora::rdcDx12ResourceReference(resource.resourceId, text(resource.name));
                break;
            }
    } catch (const std::exception &e) { result["reason"] = e.what(); }
    return result;
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
Json debug(IReplayController &controller, uint32_t eid, const Json &job, const Session &session) {
    const auto dxbcWorkgroupPatch = session.dxbcWorkgroupPatch;
    const auto retainedComputePatch = session.retainedComputePatch;
    const auto uavTexturePatch = session.uavTexturePatch;
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
    if (job.value("dxil_fast_contraction", false) && reflection->encoding != ShaderEncoding::DXIL)
        throw std::runtime_error("Fast contraction requires a DXIL shader at the selected stage");
    if (!reflection->debugInfo.debuggable)
        throw std::runtime_error("Shader cannot be debugged: " + text(reflection->debugInfo.debugStatus));
    const bool retainedCompute = dx12 && compute && reflection->encoding == ShaderEncoding::DXIL && retainedComputePatch == 2;
    const std::optional<D3D12Pipe::InputAssembly> vertexIA = dx12 && vertex
        ? std::optional<D3D12Pipe::InputAssembly>(pipe12->inputAssembly) : std::nullopt;
    const auto vertexInputs = [&] {
        return flora::rdcVertexInputSnapshot(*selected, *vertexIA, index(job, "vertex"), index(job, "instance"),
            [&](ResourceId resource, uint64_t offset, uint64_t length) {
                return controller.GetBufferData(resource, offset, length);
            });
    };
    Json result = Json::object();
    bool unpatchedPackedUav = false;
    if (dx12 && reflection->encoding == ShaderEncoding::DXIL && !session.packedStorePatch) {
        const auto stage = compute ? ShaderStage::Compute : vertex ? ShaderStage::Vertex : ShaderStage::Pixel;
        const auto &accesses = controller.GetDescriptorAccess();
        if (accesses.size() > 16384) throw std::runtime_error("Shader descriptor access limit exceeded");
        for (const auto &access : accesses) {
            if (access.stage != stage || CategoryForDescriptorType(access.type) != DescriptorCategory::ReadWriteResource) continue;
            rdcarray<DescriptorRange> ranges; ranges.push_back(DescriptorRange(access));
            const auto views = controller.GetDescriptors(access.descriptorStore, ranges);
            if (views.size() != 1) throw std::runtime_error("Incomplete shader UAV descriptor response");
            if (views[0].format.type == ResourceFormatType::R11G11B10) unpatchedPackedUav = true;
        }
    }
    if (dx12 && compute)
        result["compute_invocation"] = flora::rdcComputeInvocation(
            *selected, *reflection, coordinates(job, "group"), coordinates(job, "thread"));
    std::vector<flora::RdcBufferInputRange> computeBufferRanges;
    if (dx12 && compute) {
        const auto buffers = controller.GetBuffers();
        const auto resources = controller.GetResources();
        Json bindings = Json::array();
        const auto &accesses = controller.GetDescriptorAccess();
        if (accesses.size() > 16384) throw std::runtime_error("Compute descriptor access limit exceeded");
        for (const auto &access : accesses) {
            if (access.stage != ShaderStage::Compute || CategoryForDescriptorType(access.type) != DescriptorCategory::ReadOnlyResource) continue;
            rdcarray<DescriptorRange> ranges; ranges.push_back(DescriptorRange(access));
            const auto views = controller.GetDescriptors(access.descriptorStore, ranges);
            if (views.size() != 1) throw std::runtime_error("Incomplete compute input descriptor response");
            const auto &view = views[0];
            const auto buffer = std::find_if(buffers.begin(), buffers.end(), [&](const auto &b) { return b.resourceId == view.resource; });
            if (buffer == buffers.end()) continue; // Texture/UAV/CBV inputs are outside this check.
            if (computeBufferRanges.size() == 256) throw std::runtime_error("Compute input buffer view limit exceeded");
            computeBufferRanges.push_back({view.resource, view.byteOffset, view.byteSize, buffer->length});
            const auto resource = std::find_if(resources.begin(), resources.end(), [&](const auto &r) { return r.resourceId == view.resource; });
            bindings.push_back({{"resource", resourceName(view.resource)}, {"name", resource == resources.end() ? "" : text(resource->name)},
                {"reflection_index", access.index}, {"array_element", access.arrayElement}, {"statically_unused", access.staticallyUnused}});
        }
        result["compute_buffer_inputs"]["bindings"] = std::move(bindings);
    }
    const auto computeInputs = [&] {
        return flora::rdcBufferInputSnapshot(computeBufferRanges, [&](ResourceId resource, uint64_t offset, uint64_t length) {
            return controller.GetBufferData(resource, offset, length);
        });
    };
    if (dx12 && compute) result["compute_buffer_inputs"]["before_debug"] = computeInputs();
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
                 {"value", session.computePrepared ? Json() : pixel(controller.PickPixel(resource, x, y, sub, cast))},
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
        if (dx12) {
            const auto invocation = flora::rdcVertexInvocation(*selected, pipe12->inputAssembly,
                index(job, "vertex"), index(job, "instance"),
                [&](ResourceId resource, uint64_t offset, uint64_t length) {
                    return controller.GetBufferData(resource, offset, length);
                });
            actual = invocation.at("actual_vertex_index").get<uint32_t>();
            if (job.contains("index") && !job.at("index").is_null() && index(job, "index") != actual)
                throw std::runtime_error("Explicit DX12 vertex index does not match the selected Draw invocation");
            result["vertex_invocation"] = invocation;
            result["vertex_input_snapshots"]["before_debug"] = vertexInputs();
        } else if (job.contains("index") && !job.at("index").is_null()) {
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
    if (dx12 && compute) result["compute_buffer_inputs"]["after_trace_creation"] = computeInputs();
    if (referenceSelection && !retainedCompute) {
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
    if (retainedCompute) {
        result["compute_input_lifecycle"] = {
            {"status", "retained_until_trace_release"}, {"patch_version", retainedComputePatch},
            {"scope", "DXIL compute pre-dispatch application resources retained after lane probing; no prefix replays between trace creation and stepping. FreeTrace executes the original dispatch to restore post-event state."}};
    }
    if (dx12 && reflection->encoding == ShaderEncoding::DXIL && !uavTexturePatch &&
        std::any_of(reflection->readWriteResources.begin(), reflection->readWriteResources.end(),
                    [](const auto &resource) { return resource.isTexture; })) {
        result["partial"] = true;
        result["debug_scope"]["warnings"] = {
            "This RenderDoc library lacks the DXIL UAV texture fix. Texture coordinates and view mip/slices may be incorrect; this trace is not validated. Use the patched analysis library."};
    }
    result["source_debug"] = flora::rdcDebugInfo(reflection->debugInfo);
    if (unpatchedPackedUav) {
        result["partial"] = true;
        auto &warnings = result["debug_scope"]["warnings"];
        if (warnings.is_null()) warnings = Json::array();
        warnings.push_back("This RenderDoc library lacks the DXIL R11G11B10 store-conversion fix. The bound packed UAV can round stores upward or turn finite overflow into infinity; later loads may be incorrect. Use the patched analysis library.");
    }
    const auto assembly = controller.DisassembleShader(
        pipe12 ? pipe12->pipelineResourceId : ResourceId::Null(), reflection, "");
    auto exportedAssembly = QByteArray(assembly.c_str(), qsizetype(assembly.size()));
    exportedAssembly.replace("\n", "\r\n");
    QSaveFile file(QString::fromStdString(job.at("out").get<std::string>()) + "/debug_shader.asm");
    if (!file.open(QIODevice::WriteOnly) || file.write(exportedAssembly) != exportedAssembly.size() ||
        !file.commit())
        throw std::runtime_error("Cannot export shader disassembly");
    result["disassembly"] = text(assembly);
    if (reflection->encoding == ShaderEncoding::DXIL) {
        uint64_t annotated{};
        const auto listing = text(assembly);
        for (size_t at = 0; (at = listing.find("fast-math: ", at)) != std::string::npos; at += 11) ++annotated;
        result["debug_scope"]["dxil_instruction_flags"] = {
            {"patch_version", session.dxilInstructionFlagsPatch},
            {"status", session.dxilInstructionFlagsPatch == 1 ? "included" : "unavailable"},
            {"annotated_lines", annotated}, {"driver_instructions_observed", false},
            {"scope", "Fast-math comments describe captured DXIL optimization permissions. They do not identify driver machine instructions or validate interpreter intermediates."}};
    }
    // Keep the actual bound program alongside the trace. A decompiled listing
    // alone cannot preserve fast-math flags or the original compiler context.
    result["shader_bytecode"] = {{"status", "unavailable"},
        {"reason", "No DXBC/DXIL bytecode is available from the selected shader reflection"}};
    if (!reflection->rawBytes.empty() &&
        (reflection->encoding == ShaderEncoding::DXBC || reflection->encoding == ShaderEncoding::DXIL)) {
        const bool dxil = reflection->encoding == ShaderEncoding::DXIL;
        const auto filename = dxil ? "debug_shader.dxil" : "debug_shader.dxbc";
        const auto bytes = QByteArrayView(reinterpret_cast<const char *>(reflection->rawBytes.data()),
                                         qsizetype(reflection->rawBytes.size()));
        QSaveFile bytecodeFile(QString::fromStdString(job.at("out").get<std::string>()) + '/' + filename);
        if (!bytecodeFile.open(QIODevice::WriteOnly) ||
            bytecodeFile.write(bytes.data(), bytes.size()) != bytes.size() || !bytecodeFile.commit())
            throw std::runtime_error("Cannot export selected shader bytecode");
        result["shader_bytecode"] = {{"status", "exported"}, {"format", dxil ? "DXIL" : "DXBC"},
            {"file", filename}, {"byte_length", std::to_string(reflection->rawBytes.size())},
            {"sha256", QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString()},
            {"scope", "Currently bound replay shader, including any experiment replacement. "
                      "This is not a claim of equality with the original capture shader or root-signature compatibility."}};
    }
    if (dx12 && compute && reflection->encoding == ShaderEncoding::DXIL && !session.computePrepared) {
        result["partial"] = true;
        auto &warnings = result["debug_scope"]["warnings"];
        if (warnings.is_null()) warnings = Json::array();
        warnings.push_back("This analysis uses legacy post-event preparation. DebugThread replays the prefix again, which can change indirect work and inputs. Use the prepared-compute backend patch for a single pre-dispatch preparation.");
    }
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
    Json batches = Json::array();
    for (;;) {
        const auto batch = controller.ContinueDebug(trace->debugger);
        if (batch.empty())
            break;
        batches.push_back(batch.size());
        if (batch.size() > 250000 - steps.size())
            throw std::runtime_error("Debug trace exceeded 250000 steps");
        for (const auto &state : batch)
            steps.push_back(flora::rdcDebugState(state));
    }
    if (steps.empty())
        throw std::runtime_error("Shader debugger returned no steps");
    result["step_count"] = steps.size();
    result["steps"] = std::move(steps);
    result["debug_batch_sizes"] = std::move(batches);
    if (retainedCompute && session.textureStoresPatch == 1) {
        const auto count = session.textureStoreCount(trace->debugger);
        if (count > 4096) throw std::runtime_error("Selected invocation exceeds 4096 texture store observations");
        Json rows = Json::array();
        const auto resources = controller.GetResources();
        std::map<uint32_t, size_t> positions;
        const auto &recordedSteps = result.at("steps");
        for (size_t i = 0; i < recordedSteps.size(); ++i)
            if (!positions.emplace(recordedSteps[i].at("stepIndex").get<uint32_t>(), i).second)
                throw std::runtime_error("Duplicate debug step identity in texture store trace");
        for (uint32_t i = 0; i < count; ++i) {
            std::array<uint32_t, 32> words{};
            if (!session.textureStoreRead(trace->debugger, i, words.data(), uint32_t(words.size())))
                throw std::runtime_error("Incomplete texture store observer response");
            auto row = flora::rdcTextureStoreRecord(words);
            const auto position = positions.find(words[30]);
            if (position == positions.end() || position->second == 0 ||
                recordedSteps[position->second - 1].at("nextInstruction") != words[0])
                throw std::runtime_error("Texture store does not match its executed instruction occurrence");
            row["trace_position"] = position->second - 1;
            row["ordinal"] = i;
            const auto rid = number(row.at("rdc_resource"));
            const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto &r) { return id(r.resourceId) == rid; });
            row["resource_name"] = found == resources.end() ? "" : text(found->name);
            row["resource_object"] = row["resource_creation"] = nullptr;
            const auto name = row.at("resource_name").get<std::string>();
            constexpr std::string_view prefix = "Flora DX12 resource ";
            if (name.starts_with(prefix)) {
                const std::string_view tail(name.data() + prefix.size(), name.size() - prefix.size());
                const auto split = tail.find(" creation ");
                if (split != std::string_view::npos) {
                    uint64_t object{}, creation{};
                    const auto a = tail.substr(0, split), b = tail.substr(split + 10);
                    const auto pa = std::from_chars(a.data(), a.data() + a.size(), object);
                    const auto pb = std::from_chars(b.data(), b.data() + b.size(), creation);
                    if (pa.ec == std::errc{} && pb.ec == std::errc{} && pa.ptr == a.data() + a.size() && pb.ptr == b.data() + b.size() && object && creation) {
                        row["resource_object"] = std::to_string(object);
                        row["resource_creation"] = std::to_string(creation);
                    }
                }
            }
            rows.push_back(std::move(row));
        }
        result["texture_stores"] = {{"patch_version", 1}, {"rows", rows}, {"validation", "not_validated"},
            {"scope", "Executed DXIL TextureStore attempts by the selected compute lane. Values precede storage conversion. Excludes buffer stores, atomics and other lanes; post-dispatch GPU texels can include later writes."}};
    }
    if (dx12 && compute) {
        auto &snapshots = result["compute_buffer_inputs"];
        snapshots["after_steps"] = computeInputs();
        checked(controller.GetFatalErrorStatus());
        const auto &before = snapshots.at(retainedCompute ? "after_trace_creation" : "before_debug");
        const bool complete = before.at("status") == "read" && snapshots.at("after_trace_creation").at("status") == "read" && snapshots.at("after_steps").at("status") == "read";
        const bool same = before == snapshots.at("after_trace_creation") && before == snapshots.at("after_steps");
        snapshots["status"] = computeBufferRanges.empty() ? "no_buffer_views" : !complete ? "incomplete" : same ? "observed_bytes_unchanged" : "observed_bytes_changed";
        snapshots["scope"] = retainedCompute
            ? "Potential CS read-only buffer views compared after trace creation and after stepping, up to 16 MiB each point. before_debug belongs to an earlier post-event replay and is excluded from this comparison. Textures, UAVs and constant buffers are not checked; unchanged observations are not continuous validation."
            : "Potential CS read-only buffer views at three observation points, up to 16 MiB each point. Textures, UAVs and constant buffers are not checked. Unchanged bytes do not prove stable invocation inputs during simulation.";
        if (!computeBufferRanges.empty() && (!complete || !same)) {
            result["partial"] = true;
            auto &warnings = result["debug_scope"]["warnings"];
            if (warnings.is_null()) warnings = Json::array();
            warnings.push_back(complete
                ? "Compute read-only buffer inputs changed during debug replays. The same Group/Thread coordinates may select different work; an earlier GPU texel cannot validate this invocation."
                : "Compute read-only buffer input snapshots are incomplete; input stability is unverified.");
        }
    }
    if (retainedCompute) {
        // While the trace is alive, the application is intentionally before the dispatch.
        // Never label a PickPixel read in that interval as a post-event GPU reference.
        trace.reset();
        checked(controller.GetFatalErrorStatus());
        result["compute_input_lifecycle"]["status"] = "released_dispatch_restored";
        result["compute_buffer_inputs"]["after_dispatch_restoration"] = computeInputs();
        if (result.contains("texture_stores")) {
            const auto textures = controller.GetTextures();
            std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, Json> samples;
            for (auto &row : result["texture_stores"]["rows"]) {
                row["gpu_reference"] = {{"status", "unavailable"}, {"scope", "post_dispatch_after_trace_release"}};
                if (!row.contains("location")) { row["gpu_reference"]["reason"] = "Out-of-bounds store attempt"; continue; }
                const auto rid = number(row.at("rdc_resource"));
                const auto texture = std::find_if(textures.begin(), textures.end(), [&](const auto &t) { return id(t.resourceId) == rid; });
                const auto &p = row.at("location");
                const auto x = p.at("x").get<uint32_t>(), y = p.at("y").get<uint32_t>(), mip = p.at("mip").get<uint32_t>(), layer = p.at("layer").get<uint32_t>();
                const auto cast = CompType(row.at("component_type_enum").get<uint32_t>());
                if (texture == textures.end() || mip >= texture->mips || x >= extent(texture->width, mip) || y >= extent(texture->height, mip) ||
                    layer >= (texture->dimension == 3 ? extent(texture->depth, mip) : texture->arraysize) || cast == CompType::Typeless) {
                    row["gpu_reference"]["reason"] = "Texture metadata or interpretation does not match the store"; continue;
                }
                const auto key = std::make_tuple(rid, x, y, mip, layer, uint32_t(cast));
                if (!samples.contains(key)) {
                    if (samples.size() >= 64) { row["gpu_reference"]["reason"] = "64 distinct texel observation limit reached"; continue; }
                    samples.emplace(key, pixel(controller.PickPixel(texture->resourceId, x, y, Subresource(mip, layer, 0), cast)));
                    checked(controller.GetFatalErrorStatus());
                }
                row["gpu_reference"].update({{"status", "read"}, {"value", samples.at(key)},
                    {"interpretation", cast == CompType::UInt ? "uint" : cast == CompType::SInt ? "sint" : "float"}});
            }
        }
    }
    // Debugging can replay the capture again, including when a DXBC UAV is
    // fetched lazily. Do not silently compare a trace against an earlier
    // output when those replays have changed even the selected reference.
    if (referenceSelection) {
        const auto &selection = *referenceSelection;
        const auto after = pixel(controller.PickPixel(selection.resource, selection.x, selection.y,
                                                      selection.sub, selection.cast));
        auto &reference = result["gpu_reference"];
        if (session.computePrepared) {
            reference["value"] = after;
            reference["scope"] = "post_dispatch_after_trace_release";
            reference["consistency"] = "single_post_dispatch_observation";
            reference["note"] = "Read after FreeTrace executes the original dispatch against the prepared inputs. No earlier post-dispatch reference was taken; this does not prove which invocation wrote the texel.";
        } else {
            const auto &before = reference.at("value").at("uintValue");
            const bool same = before == after.at("uintValue") &&
                              (retainedCompute || before == reference.at("after_trace_creation_value").at("uintValue"));
            reference["after_debug_value"] = after;
            reference["consistency"] = same ? "selected_texel_unchanged" : "selected_texel_changed";
            reference["consistency_scope"] =
                retainedCompute ? "Selected post-event texel before debugging and after trace release restores the dispatch. No post-event observation is made while inputs are retained. Unchanged values do not validate the simulated invocation."
                : "Selected post-event texel before debugging, after trace creation and after stepping, "
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
    }
    if (dx12 && vertex) {
        auto &snapshots = result["vertex_input_snapshots"];
        snapshots["after_steps"] = vertexInputs();
        // Free the interpreter before the stream-output replay. This is a
        // separate reference phase, not a stability check of the original draw.
        trace.reset();
        const auto mesh = controller.GetPostVSData(index(job, "instance"), 0, MeshDataStage::VSOut);
        checked(controller.GetFatalErrorStatus());
        auto reference = flora::rdcVertexOutputReference(*selected, mesh, *reflection,
            index(job, "vertex"), index(job, "instance"),
            [&](ResourceId resource, uint64_t offset, uint64_t length) {
                return controller.GetBufferData(resource, offset, length);
            });
        checked(controller.GetFatalErrorStatus());
        snapshots["after_output_replay"] = vertexInputs();
        checked(controller.GetFatalErrorStatus());
        const bool completeInputs = snapshots.at("before_debug").at("status") == "read" &&
            snapshots.at("after_steps").at("status") == "read" && snapshots.at("after_output_replay").at("status") == "read";
        const bool sameInputs = snapshots.at("before_debug").value("invocation", Json()) == result.at("vertex_invocation") &&
                                snapshots.at("before_debug") == snapshots.at("after_steps") &&
                                snapshots.at("before_debug") == snapshots.at("after_output_replay");
        snapshots["status"] = completeInputs ? sameInputs ? "observed_bytes_unchanged" : "observed_bytes_changed" : "unavailable";
        snapshots["scope"] = "Selected index and supported regular/packed vertex attributes at three observation points. "
                             "Unchanged bytes do not prove stable inputs during execution; textures, constants and other vertices are not checked.";
        if (completeInputs && !sameInputs) {
            result["partial"] = true;
            auto &warnings = result["debug_scope"]["warnings"];
            if (warnings.is_null()) warnings = Json::array();
            warnings.push_back("Selected vertex input bytes changed during debug/output replays; output equality cannot validate this trace.");
        }
        reference["trace_output_comparison"] = flora::rdcCompareVertexPosition(
            *reflection, result.at("trace"), result.at("steps"), reference);
        reference["output_comparison"] = flora::rdcCompareVertexOutputs(
            *reflection, result.at("trace"), result.at("steps"), reference);
        if (reference.at("trace_output_comparison").at("status") == "output_mismatch" ||
            reference.at("output_comparison").at("status") == "output_mismatch") {
            result["partial"] = true;
            auto &warnings = result["debug_scope"]["warnings"];
            if (warnings.is_null()) warnings = Json::array();
            warnings.push_back("A final simulated VS output differs from the instrumented GPU VS output. "
                               "Inspect replay inputs and shader arithmetic; this trace is not validated.");
        }
        result["gpu_vertex_reference"] = std::move(reference);
    }
    if (job.value("pixel_history_reference", false)) {
        // This query has its own replay phase. Never present its shader output
        // as the blended target or silently select among duplicate primitives.
        Json reference{{"status", "unavailable"}, {"event_id", eid},
                       {"primitive", index(job, "primitive")}, {"output_index", 0},
                       {"method", "RenderDoc PixelHistory shaderOut after debugger stepping"},
                       {"scope", "GPU fragment output before blending/storage conversion. Separate instrumented "
                                 "replays; matching output alone does not validate every instruction or input."}};
        if (!referenceSelection) {
            reference["reason"] = "No unambiguous color target 0 reference is available";
        } else if (selected->numInstances != 1) {
            reference["reason"] = "Pixel history primitive selection does not identify a draw instance";
        } else {
            const auto &selection = *referenceSelection;
            reference.update({{"resource", resourceName(selection.resource)}, {"x", selection.x}, {"y", selection.y},
                              {"mip", selection.sub.mip}, {"layer", selection.sub.slice}, {"sample", selection.sub.sample},
                              {"interpretation", result.at("gpu_reference").at("interpretation")}});
            trace.reset();
            const auto modifications = controller.PixelHistory(selection.resource, selection.x, selection.y,
                                                                 selection.sub, selection.cast);
            Json matches = Json::array();
            Json historyBoundary;
            for (const auto &entry : modifications) {
                if (entry.eventId != eid) continue;
                historyBoundary = pixel(entry.postMod.col);
                if (entry.primitiveID == index(job, "primitive")) matches.push_back(record(entry));
            }
            reference["matching_fragments"] = matches.size();
            if (matches.size() != 1) {
                reference["reason"] = matches.empty() ? "No matching GPU fragment was returned"
                                                      : "Multiple GPU fragments share this primitive; selection is ambiguous";
            } else {
                const auto &entry = matches.front();
                reference["fragment"] = entry;
                bool rejected = false;
                for (const auto key : {"directShaderWrite", "unboundPS", "sampleMasked", "backfaceCulled",
                                        "depthClipped", "depthBoundsFailed", "viewClipped", "scissorClipped",
                                        "shaderDiscarded", "depthTestFailed", "stencilTestFailed", "predicationSkipped"})
                    rejected |= entry.at(key).get<bool>();
                if (rejected)
                    reference["reason"] = "The matching fragment did not pass all tests; shader output is not validated";
                else {
                    reference["status"] = "read";
                    reference["value"] = entry.at("shaderOut").at("col");
                }
            }
            controller.SetFrameEvent(eid, true);
            reference["post_history_target"] = pixel(controller.PickPixel(selection.resource, selection.x,
                                                                           selection.y, selection.sub, selection.cast));
            const bool same = reference.at("post_history_target").at("uintValue") ==
                              result.at("gpu_reference").at("after_debug_value").at("uintValue");
            reference["target_consistency"] = same ? "selected_texel_unchanged" : "selected_texel_changed";
            reference["history_post_event_value"] = historyBoundary;
            const bool boundaryMatches = !historyBoundary.is_null() && historyBoundary.at("uintValue") ==
                                         reference.at("post_history_target").at("uintValue");
            reference["history_boundary_consistency"] = boundaryMatches ? "exact_match" : "not_exactly_matched";
            if (!boundaryMatches && reference.at("status") == "read") {
                reference["status"] = "unverified_history_boundary";
                reference["reason"] = "Pixel History's final event value does not exactly match the post-event target";
                result["partial"] = true;
            }
            if (!same || result.at("gpu_reference").value("consistency", "") != "selected_texel_unchanged") {
                reference["status"] = "unverified_replay_changed";
                reference["reason"] = "The selected target changed during debugger or pixel-history replays";
                result["partial"] = true;
                auto &warnings = result["debug_scope"]["warnings"];
                if (warnings.is_null()) warnings = Json::array();
                warnings.push_back("The GPU fragment reference comes from a changing replay and cannot validate this trace.");
            }
        }
        reference["trace_output_comparison"] = flora::rdcComparePixelOutput(
            *reflection, result.at("trace"), result.at("steps"), reference);
        if (reference.at("trace_output_comparison").at("status") == "output_mismatch") {
            result["partial"] = true;
            auto &warnings = result["debug_scope"]["warnings"];
            if (warnings.is_null()) warnings = Json::array();
            warnings.push_back("The simulated target-0 output differs from the GPU fragment output. "
                               "This trace is not validated; inspect inputs and shader arithmetic.");
        }
        result["gpu_invocation_reference"] = std::move(reference);
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
Json rdcLoadedModules() { return processModules(); }
Json runRdcJob(const Json &job) {
    const auto action = job.at("action").get<std::string>();
    const auto optimisation = rdcReplayOptimisation(job);
    if (action == "buffer") rdcBufferRange(job);
    if (job.value("isolate_missing_queries", false) && action != "inventory" && action != "texture" &&
        !((action == "pipeline" || action == "postmesh" || rdcShaderDebugAction(action)) && job.contains("execution") && !job.at("execution").is_null()) &&
        !((action == "resource-history" || action == "history") && job.contains("execution") && job.contains("resource_creation") &&
          !job.value("resource", Json()).is_null()))
        throw std::runtime_error("Unsupported query-isolated analysis or missing explicit DX12 selection");
    if (job.value("depth_roundtrip", false) &&
        ((action != "texture" && action != "debug-pixel" && action != "history") || job.value("readonly_depth_snapshot", false)))
        throw std::runtime_error("Depth roundtrip requires texture, history or debug-pixel analysis without read-only depth snapshots");
    if (job.value("readonly_depth_snapshot", false) && action != "texture" && action != "debug-pixel")
        throw std::runtime_error("Read-only depth snapshots are limited to texture and debug-pixel analysis");
    if (job.value("pixel_history_reference", false) &&
        (action != "debug-pixel" || index(job, "primitive", UINT32_MAX) == UINT32_MAX))
        throw std::runtime_error("Pixel history reference requires debug-pixel and an explicit primitive");
    const bool debugging = rdcShaderDebugAction(action);
    if (job.value("dxil_fast_contraction", false) && (!debugging || !job.contains("execution")))
        throw std::runtime_error("Fast contraction requires a DX12 shader-debug request");
    if (action != "history" && action != "events" && action != "counters" && action != "counter-catalog" && action != "inventory" &&
        action != "texture" && action != "buffer" && action != "resource-history" && action != "postmesh" && action != "pipeline" && !debugging)
        throw std::runtime_error("Unsupported native RenderDoc job action");
    if (job.contains("counter_ids") && (action != "counters" || job.at("counter_ids").is_null()))
        throw std::runtime_error("Counter IDs require a counters request");
    if (action == "counter-catalog")
        for (const auto key : {"execution", "gpa_event", "eid", "indirect_action"})
            if (job.contains(key) && !job.at(key).is_null())
                throw std::runtime_error("Counter catalog is device-wide and does not accept event selectors");
    // Direct worker jobs must not bypass the resource-lifetime check performed
    // by prepareRdcJob. Otherwise the implicit RTV gets the requested creation
    // label even though it belongs to a different captured resource.
    if (job.contains("resource_creation")) {
        if (job.value("resource", Json()).is_null())
            throw std::runtime_error("Resource creation requires a DX12 resource object");
        for (const auto key : {"resource", "resource_creation"})
            if (!job.at(key).is_string() || !number(job.at(key)))
                throw std::runtime_error("DX12 resource identities must be nonzero decimal strings");
    }
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
    if (!dx12 && job.contains("resource_creation"))
        throw std::runtime_error("Resource creation selection requires a DX12 capture");
    if (job.value("pixel_history_reference", false) && !dx12)
        throw std::runtime_error("Pixel history invocation reference currently requires DX12");
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
    const auto queryPolicy = readRdcQueryIsolation(roots);
    requireRdcQueryIsolation(queryPolicy, job);
    const auto frameEnd = readRdcFrameEnd(roots);
    requireRdcFrameEnd(frameEnd, job);
    const bool frameEndHistory = job.value("frame_end", false);
    if (frameEndHistory && !dx12) throw std::runtime_error("Frame-end history requires a native DX12 capture");
    if (!queryPolicy.is_null() && !dx12) throw std::runtime_error("Query isolation requires a DX12 capture");
    auto mapping = dx12 ? indexDx12RdcEvents(roots, events.native) : indexRdcEvents(roots, events.native);
    if (frameEndHistory) {
        const auto endEvent = frameEnd.at("event_id").get<uint32_t>();
        for (const auto &[execution, command] : mapping.at("execution_commands").items())
            for (const auto &event : command.at("event_ids"))
                if (event.get<uint32_t>() >= endEvent)
                    throw std::runtime_error("Frame-end marker precedes an executed capture command");
    }
    Json result{
        {"ok", true},
        {"action", action},
        {"backend", dx12 ? "dx12" : "dx11"},
        {"query_isolation", queryPolicy},
        {"frame_end", frameEndHistory},
        {"frame_stream_boundary", frameEnd},
        {"replay_policy", {{"optimisation", job.value("replay_optimisation", "balanced")},
            {"optimisation_enum", optimisation},
            {"dx12_discard_pattern_fills", dx12 ? Json(job.value("discard_patterns", false) && optimisation != 3) : Json()},
            {"dx12_discard_policy", dx12 ? Json(job.value("discard_patterns", false) ? "diagnostic_pattern" : "native_api") : Json()},
            {"scope", "RenderDoc replay policy; includes initial-content restoration and diagnostic work. Changing policy can change observations and does not establish capture fidelity."}}},
        {"backend_version", session.version},
        {"backend_commit", session.commit},
        {"backend_d16_history_patch", session.d16HistoryPatch},
        {"backend_dxbc_workgroup_patch", session.dxbcWorkgroupPatch},
        {"backend_dx12_usage_visibility_patch", session.dx12UsageVisibilityPatch},
        {"backend_dx12_placed_heap_restore_patch", session.placedHeapRestorePatch},
        {"backend_dx12_discard_layout_patch", session.discardLayoutPatch},
        {"backend_dx12_discard_pattern_control_patch", session.discardPatternControlPatch},
        {"backend_dx12_cube_coordinates_patch", session.cubeCoordinatesPatch},
        {"backend_dx12_vertex_inputs_patch", session.vertexInputsPatch},
        {"backend_dx12_half_color_history_patch", session.halfColorHistoryPatch},
        {"backend_dx12_resolve_history_patch", session.resolveHistoryPatch},
        {"backend_dx12_default_view_history_patch", session.defaultViewHistoryPatch},
        {"backend_dx12_routed_layer_history_patch", session.routedLayerHistoryPatch},
        {"backend_dx12_layer_test_history_patch", session.layerTestHistoryPatch},
        {"backend_dx12_bgra_history_patch", session.bgraHistoryPatch},
        {"backend_dx12_empty_pixel_inputs_patch", session.emptyPixelInputsPatch},
        {"backend_dx12_upload_write_watch_patch", session.uploadWriteWatchPatch},
        {"backend_readonly_depth_snapshot_patch", session.readOnlyDepthSnapshotPatch},
        {"backend_depth_roundtrip_patch", session.depthRoundtripPatch},
        {"backend_dxil_instruction_flags_patch", session.dxilInstructionFlagsPatch},
        {"backend_dxbc_gpu_dot_patch", session.dxbcGpuDotPatch},
        {"backend_dxbc_gpu_mad_patch", session.dxbcGpuMadPatch},
        {"replay_environment",
         {{"gpu_vendor_enum", vendor},
          {"gpu_vendor_name", vendor < std::size(vendorNames) ? Json(vendorNames[vendor]) : Json(nullptr)},
          {"remote", properties.remoteReplay},
          {"degraded", properties.degraded},
          {"pipeline_type_enum", uint32_t(properties.pipelineType)},
          {"local_renderer_enum", uint32_t(properties.localRenderer)}}},
        {"gpa_event_map", mapping["gpa_event_map"]},
        {"gpa_command_map", mapping["gpa_command_map"]}};
    if (!queryPolicy.is_null()) result["frame_complete"] = false;
    // Match the original audit point before the replay controller is shut down.
    auto complete = [&] {
        if (dx12 && session.discardPatternsSkipped) {
            result["replay_policy"]["discard_pattern_fills_skipped"] = std::to_string(session.discardPatternsSkipped());
            result["replay_policy"]["discard_scope"] = "Captured discard commands/barriers are preserved. Skipped fills are additional diagnostic writes across this analysis request, not recovered historical contents. Discarded values remain undefined.";
        }
        if (session.depthRoundtripCopies) {
            result["depth_roundtrip"] = {
                {"requested", true}, {"copies", std::to_string(session.depthRoundtripCopies())},
                {"experimental", true},
                {"supported_depth_modes", session.depthRoundtripPatch >= 2
                    ? "Read-only D32 direct Draw or depth-writing D32S8 Draw"
                    : "Depth-writing D32S8 Draw"},
                {"scope", "Selected isolated DX12 Draw only: stencil disabled, one mip/layer/sample/node "
                          "and legacy depth states. Copies all planes out and back before the Draw, "
                          "preserving the original DSV and subsequent depth writes. Count is completed "
                          "roundtrips (two GPU copies each), not unique captured actions. "
                          "Adds GPU work and a wait; does not validate full replay correctness."}};
        }
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
            if (frameEndHistory) {
                result["frame_complete"] = false;
                result["history_scope"]["output_boundary"] = "frame_end";
                result["history_scope"]["through_record"] = frameEnd.at("boundary").at("last_record");
                result["history_scope"]["through_event"] = frameEnd.at("event_id");
            }
            if (!queryPolicy.is_null() && action == "postmesh") {
                result["partial"] = true;
                result["mesh_scope"] = {
                    {"query_outputs_incomplete", true},
                    {"warning", "Incomplete query-isolated replay: historical query values remain unavailable. "
                                "Mesh data describes this replay's selected execution and instance; "
                                "it does not establish complete original-frame fidelity."}};
            }
            if (!queryPolicy.is_null() && debugging) {
                result["partial"] = true;
                result["debug_scope"]["query_outputs_incomplete"] = true;
                auto &warnings = result["debug_scope"]["warnings"];
                if (warnings.is_null()) warnings = Json::array();
                warnings.push_back("Incomplete query-isolated replay: historical query values remain unavailable. "
                    "This invocation uses the replayed resources; stepping and output comparisons do not establish complete original-frame fidelity.");
            }
            if (!queryPolicy.is_null() && action == "resource-history") {
                result["history_scope"]["query_outputs_incomplete"]=true;
                result["history_scope"]["limitations"].push_back(
                    "Isolated query resolves include only available results. Omitted writes remain in query_isolation; "
                    "usage records do not establish query-result byte availability.");
            }
            if (!queryPolicy.is_null() && action == "history") {
                result["partial"] = true;
                result["history_scope"]["query_outputs_incomplete"] = true;
                result["history_scope"]["limitations"].push_back(
                    "Incomplete query-isolated replay: historical query values remain unavailable. "
                    "Pixel History describes this replay's selected texture; its values and test results "
                    "do not establish complete original-frame fidelity.");
            }
            result["execution_map"] = mapping["execution_map"];
            result["record_executions"] = mapping["record_executions"];
            result["execution"] = job.value("execution", Json(nullptr));
            result.erase("gpa_event");
        }
        result["dxil_compiler"] = session.compilerInformation();
        result["loaded_modules"] = rdcLoadedModules();
        return std::move(result);
    };
    if (action == "events") {
        result["index"] = mapping;
        result["cpu_writes"] = writes(events);
        return complete();
    }
    std::map<GPUCounter, CounterDescription> counterDescriptions;
    Json counterCatalog = Json::array();
    if (action == "counters" || action == "counter-catalog") {
        for (const auto counter : controller.EnumerateCounters()) {
            auto description = controller.DescribeCounter(counter);
            if (description.counter != counter || !counterDescriptions.emplace(counter, description).second)
                throw std::runtime_error("Invalid or duplicate counter description");
            counterCatalog.push_back(rdcCounterDescription(description));
        }
        checked(controller.GetFatalErrorStatus());
        if (action == "counter-catalog") {
            result.update({{"available", counterCatalog}, {"values", Json::array()}, {"result_count", 0},
                {"measurement_status", "not_requested"}, {"scope", "replay_device_catalog"},
                {"note", "Catalog only; capture loading may execute GPU work. No counter sampling or additional event replay requested. Availability is not a successful measurement."}});
            return complete();
        }
    }
    const auto resources = controller.GetResources();
    // Only indexed Resource History can select the end of a verified partial
    // query resolve. Other analyses still require a unique replay action.
    std::map<std::string,uint32_t> queryResolveEnds;
    if (action == "resource-history" && !queryPolicy.is_null()) {
        result["query_resolve_mapping_gaps"]=Json::array();
        for (const auto &entry : queryPolicy.at("resolves")) {
            const auto execution=entry.at("execution").get<std::string>();
            try {
                const auto &command=mapping.at("execution_commands").at(execution);
                const auto target=std::find_if(queryPolicy.at("resources").begin(),queryPolicy.at("resources").end(),
                    [&](const auto &r) { return r.at("creation") == entry.at("creation"); });
                if (target == queryPolicy.at("resources").end()) throw std::runtime_error("Missing isolated query resource");
                const auto resource=capturedResource(resources,
                    {{"resource",target->at("object")},{"resource_creation",target->at("creation")}},true);
                Json operations=Json::array();
                for (const auto &id : command.at("event_ids")) {
                    const auto event=id.get<uint32_t>();
                    const auto chunk=events.chunks.find(event);
                    if (chunk == events.chunks.end() ||
                        !text(chunk->second->name).ends_with("::ResolveQueryData"))
                        throw std::runtime_error("Partial query resolve arguments are missing");
                    const auto ranges=rdcOperationRanges(*chunk->second,resource,{});
                    if (ranges.at("status") != "captured" || ranges.at("ranges").size() != 1)
                        throw std::runtime_error("Partial query resolve destination cannot be verified");
                    auto operation=ranges.at("ranges")[0]; operation["eventId"]=event;
                    operations.push_back(std::move(operation));
                }
                verifyRdcQueryResolveExpansion(command,entry,operations,std::to_string(std::bit_cast<uint64_t>(resource)));
                for (size_t part=0;part<operations.size();++part) {
                    auto &provenance=mapping["reverse"][std::to_string(operations[part].at("eventId").get<uint32_t>())];
                    provenance.update({{"selectable",true},{"query_resolve_part",part},
                        {"query_resolve_part_count",operations.size()},
                        {"navigation_scope","Original ResolveQueryData execution; emitted parts cover only query results available in the isolated replay"}});
                }
                queryResolveEnds[execution]=operations.back().at("eventId").get<uint32_t>();
            } catch (const std::exception &e) {
                result["query_resolve_mapping_gaps"].push_back({{"execution",execution},{"reason",e.what()}});
            }
        }
    }
    const auto gpa = job.value(dx12 ? "execution" : "gpa_event", Json(nullptr));
    const auto selectionMap = dx12 ? "execution_map" : "gpa_event_map";
    if (job.contains("indirect_action") && (!dx12 || gpa.is_null()))
        throw std::runtime_error("Indirect action selection requires a DX12 execution");
    uint32_t eid = index(job, "eid");
    if (frameEndHistory) {
        eid = frameEnd.at("event_id").get<uint32_t>();
    } else if (dx12 && !gpa.is_null()) {
        const auto execution=std::to_string(number(gpa));
        if (queryResolveEnds.contains(execution) && job.value("indirect_action",Json()).is_null()) {
            eid=queryResolveEnds.at(execution);
            result["resource_history_selection"]={{"execution",execution},{"eventId",eid},
                {"scope","Index through the last verified emitted part of the isolated query resolve; missing query values remain unavailable"}};
        } else {
            const auto selected = selectDx12RdcExecution(mapping, execution, job.value("indirect_action", Json()));
            eid = selected.at("eid").get<uint32_t>();
            if (selected.contains("indirect_action")) result["indirect_selection"] = selected;
        }
    } else if (!eid) {
        if (!gpa.is_null()) {
            const auto key = std::to_string(number(gpa));
            if (!mapping[selectionMap].contains(key))
                throw std::runtime_error("Selected capture event has no uniquely mapped executed action");
            eid = mapping[selectionMap].at(key).get<uint32_t>();
        } else if (!mapping["actions"].empty())
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
    if (session.setDepthRoundtripEvent) session.setDepthRoundtripEvent(eid);
    // GetUsage is an index built while loading the capture. An explicit DX12
    // resource identity does not require a replayed pipeline/RTV to resolve it.
    // Keep replay for implicit targets and for all value/state analysis paths.
    const bool indexedResourceHistory = dx12 && action == "resource-history" &&
                                        !job.value("resource", Json()).is_null();
    // Inventory exports loading-time action/resource descriptions and does not
    // inspect replayed state. Do not replay an arbitrary last action to list it.
    if (!indexedResourceHistory && !(dx12 && action == "inventory") && action != "counters") {
        uint32_t prepared{};
        if (dx12 && action == "debug-thread" && session.prepareCompute && session.retainedComputePatch == 2)
            prepared = session.prepareCompute(&controller, eid);
        if (prepared > 2) throw std::runtime_error("Invalid compute preparation response");
        session.computePrepared = prepared == 1;
        if (!prepared) controller.SetFrameEvent(eid, true);
        if (session.depthRoundtripCopies && session.depthRoundtripCopies() == 0)
            throw std::runtime_error("Depth roundtrip was not applied: select a read-only D32 direct Draw (patch v2) or depth-writing D32S8 Draw with "
                                     "stencil disabled, one mip/layer/sample/node and legacy depth states");
    }
    if (action == "pipeline") {
        const auto *selected = findAction(controller.GetRootActions(), eid);
        if (!dx12 || !selected || !(selected->flags & (ActionFlags::Drawcall | ActionFlags::Dispatch | ActionFlags::MeshDispatch)))
            throw std::runtime_error("Pipeline inspection requires an executed DX12 Draw or Dispatch");
        result.update(readDx12Pipeline(controller, *selected));
        if ((selected->flags & ActionFlags::Indirect) && (selected->flags & ActionFlags::Dispatch)) {
            auto inputs = indirectDispatchInputs(controller, events, rdcProvenance(eid, mapping["reverse"]), *selected);
            inputs["execution"] = gpa;
            inputs["observation_boundary"] = "after_selected_event";
            result["indirect_inputs"] = inputs;
            result["pipeline_rows"].push_back({
                {"section", "Execution"}, {"binding", "Indirect argument snapshot"},
                {"resource", inputs.value("argument_buffer", rdcDx12ResourceReference(ResourceId(), ""))},
                {"details", inputs}});
        }
        if (!queryPolicy.is_null()) {
            result["pipeline_scope"]["query_outputs_incomplete"] = true;
            result["pipeline_scope"]["gaps"].push_back(
                "Query-isolated replay: historical query results remain unavailable. Pipeline inspection reports "
                "bound state and resource metadata, not query bytes or complete original-frame fidelity.");
        }
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
        if (!meshOutput && index(job, "instance") >= selected->numInstances)
            throw std::runtime_error("Mesh instance is outside the executed Draw");
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
        if (dx12 && stage == "VSOut") {
            if (vertexBytes % selected->numInstances)
                throw std::runtime_error("Post-VS instance blocks have inconsistent sizes");
            vertexBytes /= selected->numInstances;
            if (mesh.vertexByteOffset != vertexBytes * index(job, "instance"))
                throw std::runtime_error("Post-VS instance offset differs from the selected Draw");
        }
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
        if ((meshOutput || (dx12 && stage == "VSOut")) && raw.size() != vertexBytes)
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
        if (dx12) {
            const auto mask = mesh.indexByteStride == 1 ? 0xffU : mesh.indexByteStride == 2 ? 0xffffU : UINT32_MAX;
            metadata.update({{"allowRestart", mesh.allowRestart}, {"restartIndex", mesh.restartIndex & mask}});
        }
        if (dx12 && stage == "VSOut") {
            const auto *pipeline = controller.GetD3D12PipelineState();
            if (!pipeline) throw std::runtime_error("DX12 VSOut input-assembly state is unavailable");
            // RenderDoc 1.45 preserves the original cut values while rebasing
            // VSOut indices, but MeshFormat retains its generic restart default.
            const auto cut = pipeline->inputAssembly.indexStripCutValue;
            metadata.update({{"allowRestart", bool(selected->flags & ActionFlags::Indexed) && IsStrip(mesh.topology) && cut != 0},
                             {"restartIndex", cut}});
            if (pipeline && pipeline->vertexShader.reflection)
                metadata["output_layout"] = rdcVertexOutputLayout(*pipeline->vertexShader.reflection, mesh.vertexByteStride);
        }
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
        if (!meshOutput) result.update({{"instance", index(job, "instance")},
                                        {"draw_parameters", rdcExecutedAction(*selected)}});
        return complete();
    }
    if (action == "inventory") {
        if (dx12) {
            result["inventory_scope"] = "Capture-loading metadata; no SetFrameEvent replay for inventory";
            result["indirect_executions"] = Json::object();
            for (const auto &[execution, command] : mapping.at("execution_commands").items())
                if (command.value("name", "") == "ID3D12GraphicsCommandListExecuteIndirect")
                    result["indirect_executions"][execution] = command;
        }
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
        const auto selected = selectRdcCounters(counterDescriptions, job.value("counter_ids", Json()));
        Json values = Json::array(), sampledIds = Json::array();
        std::set<GPUCounter> pending;
        std::set<std::pair<uint32_t, GPUCounter>> samples;
        for (const auto counter : selected) { pending.insert(counter); sampledIds.push_back(uint32_t(counter)); }
        const auto measured = controller.FetchCounters(selected);
        checked(controller.GetFatalErrorStatus());
        if (!selected.empty() && measured.empty())
            throw std::runtime_error("RenderDoc returned no measured counters; available descriptions are not measurements");
        for (const auto &counter : measured) {
            const auto it = counterDescriptions.find(counter.counter);
            if (it == counterDescriptions.end() || !selected.contains(counter.counter) ||
                !samples.emplace(counter.eventId, counter.counter).second)
                throw std::runtime_error("Counter result is unrequested, duplicate or has no description");
            pending.erase(counter.counter);
            const auto &description = it->second;
            auto value = rdcCounterValue(description, counter);
            if (dx12 && value.is_number_integer()) value = value.dump();
            Json row{{"eventId", counter.eventId},
                     {"counter", uint32_t(counter.counter)},
                     {"name", text(description.name)},
                     {"unit", rdcCounterUnit(description.unit)},
                     {"source", rdcCounterDescription(description).at("source")},
                     {"sampling_scope", "capture_actions"},
                     {"value", value}};
            row.update(rdcProvenance(counter.eventId, mapping["reverse"]));
            values.push_back(std::move(row));
        }
        if (!pending.empty())
            throw std::runtime_error("RenderDoc returned no measurements for one or more requested counters");
        for (auto &description : counterCatalog)
            if (selected.contains(GPUCounter(description.at("counter").get<uint32_t>())))
                description["status"] = "measured";
        result.update({{"eid", eid},
                       {"gpa_event", gpa},
                       {"available", counterCatalog},
                       {"counter_ids", sampledIds},
                       {"measurement_status", "measured"},
                       {"sampling_scope", "capture_actions"},
                       {"values", values},
                       {"result_count", values.size()},
                       {"note", "Measured across capture actions on the replay GPU, not limited to the event selector. Counter collection may require multiple replay passes. GPA vendor-specific metrics are not implied."}});
        return complete();
    }
    if (debugging) {
        if (dx12 && action == "debug-thread") {
            const auto selected = events.actionDetails.find(eid);
            if (selected != events.actionDetails.end() && (selected->second->flags & ActionFlags::Indirect)) {
                auto inputs = indirectDispatchInputs(controller, events, rdcProvenance(eid, mapping["reverse"]), *selected->second);
                if (session.computePrepared) inputs["scope"] = "Prepared pre-dispatch argument buffer; a successful DebugThread must consume this state without another prefix replay.";
                inputs["execution"] = gpa;
                result["indirect_inputs"] = inputs;
                QSaveFile evidence(QString::fromStdString(job.at("out").get<std::string>()) + "/debug_indirect_inputs.json");
                const auto bytes = QByteArray::fromStdString(inputs.dump(2));
                if (!evidence.open(QIODevice::WriteOnly) || evidence.write(bytes) != bytes.size() || !evidence.commit())
                    throw std::runtime_error("Cannot save indirect debug input evidence");
                if (session.computePrepared && inputs.value("status", "") == "read") {
                    const auto group = coordinates(job, "group");
                    if (!inputs.at("action_present").get<bool>() || group[0] >= inputs.at("groups")[0].get<uint32_t>() ||
                        group[1] >= inputs.at("groups")[1].get<uint32_t>() || group[2] >= inputs.at("groups")[2].get<uint32_t>())
                        throw std::runtime_error("Selected compute group is outside the prepared indirect dispatch; inspect debug_indirect_inputs.json");
                }
            }
        }
        const auto contractStart = session.contractOperations ? session.contractOperations() : 0;
        const auto contractFailureStart = session.contractFailures ? session.contractFailures() : 0;
        const auto dotStart = session.dotOperations ? session.dotOperations() : 0;
        const auto failureStart = session.dotFailures ? session.dotFailures() : 0;
        const auto madStart = session.madOperations ? session.madOperations() : 0;
        const auto madFailureStart = session.madFailures ? session.madFailures() : 0;
        const auto retainedStart = session.retainedComputeTraces ? session.retainedComputeTraces() : 0;
        const auto uavReadStart = session.uavTextureReads ? session.uavTextureReads() : 0;
        const auto uavFailureStart = session.uavTextureFailures ? session.uavTextureFailures() : 0;
        const auto packedStart = session.packedStores ? session.packedStores() : 0;
        const auto preparedStart = session.preparedComputeConsumed ? session.preparedComputeConsumed() : 0;
        result.update(debug(controller, eid, job, session));
        const auto preparedCount = session.preparedComputeConsumed ? session.preparedComputeConsumed() - preparedStart : 0;
        if (session.computePrepared && preparedCount != 1)
            throw std::runtime_error("Compute debugger did not consume the prepared event state");
        if (dx12 && action == "debug-thread")
            result["compute_preparation"] = {{"patch_version", session.preparedComputePatch},
                {"status", session.computePrepared ? "single_pre_dispatch_preparation" : "legacy_post_event_preparation"},
                {"consumed", std::to_string(preparedCount)},
                {"scope", session.computePrepared ? "A single event-before prefix prepares DXIL compute inputs. Lane probing and retained stepping share that state; this does not prove earlier prefix replay fidelity."
                    : "Legacy post-event preparation; the debugger may replay the prefix again and regenerate different work or inputs."}};
        result["dxil_packed_store"] = {{"patch_version", session.packedStorePatch},
            {"stores", std::to_string(session.packedStores ? session.packedStores() - packedStart : 0)},
            {"scope", "DX12 DXIL R11G11B10 typed stores: direct float32 round-toward-zero, finite saturation and subnormal preservation. Count includes all simulated lanes, not only the selected lane."}};
        const auto uavFailures = session.uavTextureFailures ? session.uavTextureFailures() - uavFailureStart : 0;
        if (uavFailures) throw std::runtime_error("DXIL UAV texture view read failed or is unsupported; incomplete shader trace rejected");
        result["dxil_uav_texture"] = {{"patch_version", session.uavTexturePatch},
            {"views_read", std::to_string(session.uavTextureReads ? session.uavTextureReads() - uavReadStart : 0)},
            {"failures", std::to_string(uavFailures)},
            {"scope", "DX12 DXIL typed UAV texture loads/stores and atomics: full-width coordinates, view mip/slices and per-axis bounds"}};
        if (result.contains("compute_input_lifecycle")) {
            const auto count = session.retainedComputeTraces() - retainedStart;
            if (count != 1) throw std::runtime_error("DXIL compute input retention was not activated exactly once");
            result["compute_input_lifecycle"]["retained_traces"] = std::to_string(count);
        }
        const auto contractFailures = session.contractFailures ? session.contractFailures() - contractFailureStart : 0;
        if (contractFailures) throw std::runtime_error("DXIL contraction GPU operation failed; incomplete trace rejected");
        result["dxil_fast_contraction"] = {{"enabled", job.value("dxil_fast_contraction", false)},
            {"patch_version", session.dxilContractPatch},
            {"operations", std::to_string(session.contractOperations ? session.contractOperations() - contractStart : 0)},
            {"failures", std::to_string(contractFailures)},
            {"scope", "Experimental adjacent single-use fast float32 FMul/FAdd; driver instruction contraction is not observed"}};
        if (job.value("dxil_fast_contraction", false)) {
            auto &warnings = result["debug_scope"]["warnings"];
            if (warnings.is_null()) warnings = Json::array();
            warnings.push_back("Experimental fast DXIL contraction is enabled. Multiply steps retain uncontracted IR values; eligible Add steps use GPU multiply-add. Original driver intermediates are not observed.");
            result["partial"] = true;
        }
        const auto failures = session.dotFailures ? session.dotFailures() - failureStart : 0;
        if (failures)
            throw std::runtime_error("DXBC GPU dot product failed; incomplete shader trace rejected");
        const auto madFailures = session.madFailures ? session.madFailures() - madFailureStart : 0;
        if (madFailures)
            throw std::runtime_error("DXBC GPU multiply-add failed; incomplete shader trace rejected");
        result["dxbc_gpu_mad"] = {
            {"patch_version", session.dxbcGpuMadPatch},
            {"operations", std::to_string(session.madOperations ? session.madOperations() - madStart : 0)},
            {"failures", std::to_string(madFailures)},
            {"scope", "DX12 non-precise DXBC MAD; no fusion of separate MUL/ADD instructions"}};
        result["dxbc_gpu_dot"] = {
            {"patch_version", session.dxbcGpuDotPatch},
            {"operations", std::to_string(session.dotOperations ? session.dotOperations() - dotStart : 0)},
            {"failures", std::to_string(failures)},
            {"scope", "DX12 non-precise DXBC DP2/DP3/DP4"}};
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
        throw std::runtime_error("Select an explicit resource; no matching target");
    if (action == "buffer") {
        const auto range = rdcBufferRange(job);
        const auto buffers = controller.GetBuffers();
        const auto found = std::find_if(buffers.begin(), buffers.end(),
                                       [&](const auto &b) { return b.resourceId == resource; });
        if (found == buffers.end()) throw std::runtime_error("Selected resource is not a buffer");
        if (range.offset > found->length || range.length > found->length - range.offset)
            throw std::runtime_error("Selected buffer range exceeds resource length");
        const auto data = controller.GetBufferData(resource, range.offset, range.length);
        checked(controller.GetFatalErrorStatus());
        if (data.size() != range.length) throw std::runtime_error("Buffer readback returned an incomplete range");
        const auto digest = QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())), QCryptographicHash::Sha256);
        QSaveFile file(QString::fromStdWString((path(job.at("out")) / L"buffer.bin").wstring()));
        if (!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char *>(data.data()), qint64(data.size())) !=
            qint64(data.size()) || !file.commit()) throw std::runtime_error("Cannot save raw replay buffer");
        result.update({{"eid", eid}, {"gpa_event", gpa}, {"resource", resourceName(resource)},
            {"resource_object", gpaResource}, {"resource_creation", job.value("resource_creation", Json())},
            {"offset", std::to_string(range.offset)}, {"byte_length", std::to_string(range.length)},
            {"buffer_length", std::to_string(found->length)}, {"sha256", digest.toHex().toStdString()},
            {"readback_scope", "Exact requested byte range after the selected replay event; no format conversion. A successful read is not proof of replay fidelity."}});
        return complete();
    }
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
        const auto historyTextures = controller.GetTextures();
        size_t capturedRanges{}, rangeGaps{};
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
            const auto actionDetail = events.actionDetails.find(usage.eventId);
            if (dx12 && actionDetail != events.actionDetails.end() &&
                (actionDetail->second->flags & (ActionFlags::Clear | ActionFlags::Copy | ActionFlags::Resolve))) {
                const auto chunk = events.chunks.find(usage.eventId);
                row["operation_ranges"] = chunk == events.chunks.end()
                    ? Json{{"status", "unavailable"}, {"reason", "Resource operation arguments are unavailable"}}
                    : rdcOperationRanges(*chunk->second, resource, {historyTextures.data(), historyTextures.size()},
                                         {resources.data(), resources.size()});
                if (row["operation_ranges"].at("status") == "captured") {
                    row["subresource_status"] = "see_captured_operation_ranges"; ++capturedRanges;
                } else ++rangeGaps;
            }
            if (actionDetail != events.actionDetails.end() &&
                (actionDetail->second->flags & (ActionFlags::Drawcall | ActionFlags::Dispatch | ActionFlags::MeshDispatch)))
                row["indexed_action"] = flora::rdcExecutedAction(*actionDetail->second);
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
               {"captured_operation_ranges", capturedRanges},
               {"operation_range_gaps", rangeGaps},
               {"selected_event_replayed", !indexedResourceHistory},
               {"query_source", indexedResourceHistory ? "capture_usage_index_with_explicit_resource"
                                                        : "capture_usage_index_with_replayed_selection"},
               {"capture_loading_may_execute_gpu_work", true},
               {"limitations",
                {"Bindings indicate possible use, not observed reads or writes.",
                 "Operation ranges are reconstructed from captured arguments where available; binding ranges and fence dependencies remain incomplete.",
                 "Replay initialization is separate from original execution markers.",
                 frameEndHistory ? "Only indexed uses in the verified full-stream capture are covered; empty usage is not proof of no access."
                                 : "Only the captured replay prefix is covered; empty usage is not proof of no access."}}}}});
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
    // Observe the selected post-event texel before PixelHistory's internal
    // replays, and again after the existing selection restoration below. This
    // does not add another prefix replay or prove stability of other pixels.
    // Typeless R16 defaults to half-float in RenderDoc's picker; explicitly
    // select depth interpretation for depth targets (including D16).
    const auto consistencyCast = depthTarget ? CompType::Depth : CompType::Typeless;
    Json consistency{{"status", "unavailable"}, {"event_id", eid},
        {"resource", resourceName(resource)}, {"mip", sub.mip}, {"layer", sub.slice},
        {"sample", sub.sample}, {"x", x}, {"y", y},
        {"type_cast", depthTarget ? "depth" : "typeless_default"},
        {"comparison", "PickPixel component bits at the same post-event boundary"},
        {"scope", "Two observations of the selected texel only. Equal values do not prove stable intermediate fragments, other texels, or complete history."}};
    try {
        consistency["before_history"] = pixel(controller.PickPixel(resource, x, y, sub, consistencyCast));
    } catch (const std::exception &e) {
        consistency["before_error"] = e.what();
    }
    const auto modifications = controller.PixelHistory(resource, x, y, sub, CompType::Typeless);
    Json excludedOperations = Json::array(), operationCoverageGaps = Json::array();
    Json operationValueGaps = Json::array();
    std::set<uint32_t> unavailableOperationValues;
    for (size_t i = 0; i < modifications.size(); ++i) {
        const auto &v = modifications[i];
        auto r = record(v);
        r.update(rdcProvenance(v.eventId, mapping["reverse"]));
        r["record_kind"] = "backend_modification";
        r["color_interpretation"] = kind;
        r["pixel_value_kind"] = depthTarget ? "depth_stencil" : "color";
        r["stencil_present"] = stencilPresent;
        const auto actionIt = events.actionDetails.find(v.eventId);
        if (dx12 && actionIt != events.actionDetails.end() &&
            (actionIt->second->flags & (ActionFlags::Clear | ActionFlags::Copy | ActionFlags::Resolve))) {
            r["record_kind"] = "resource_operation";
            r["fragIndex"] = nullptr; r["primitiveID"] = nullptr; r["shaderOut"] = nullptr;
            const bool clear = bool(actionIt->second->flags & ActionFlags::Clear);
            r["operation"] = clear ? "clear" : bool(actionIt->second->flags & ActionFlags::Resolve) ? "resolve" : "copy";
            r["value_observation_scope"] = "Backend before/after values at the selected subresource; equality does not imply no write.";
            r["pixel_value_changed"] = depthTarget
                ? Json(r["preMod"]["depth"] != r["postMod"]["depth"] ||
                    (stencilPresent && r["preMod"]["stencil"] != r["postMod"]["stencil"]))
                : Json(r["preMod"]["col"]["uintValue"] != r["postMod"]["col"]["uintValue"]);
            const auto chunk = events.chunks.find(v.eventId);
            const auto coverage = chunk == events.chunks.end()
                ? Json{{"status", "unavailable"}, {"reason", "Resource operation structured arguments are unavailable"}}
                : clear ? flora::rdcClearCoverage(*chunk->second, tex, sub, x, y)
                        : flora::rdcTransferCoverage(*chunk->second, tex, {textures.data(), textures.size()}, sub, x, y);
            r["coverage"] = coverage;
            if (coverage.at("status") == "outside") {
                auto excluded = rdcProvenance(v.eventId, mapping["reverse"]);
                excluded.update({{"eventId", v.eventId}, {"operation", r["operation"]}, {"coverage", coverage}});
                excludedOperations.push_back(std::move(excluded));
                continue;
            }
            if (coverage.at("status") == "unavailable")
                operationCoverageGaps.push_back({{"eventId", v.eventId}, {"coverage", coverage}});
            if (!session.resolveHistoryPatch && chunk != events.chunks.end() &&
                text(chunk->second->name).ends_with("::ResolveSubresourceRegion")) {
                r["value_status"] = "unavailable_resolve_callback";
                r["value_observation_scope"] = "ResolveSubresourceRegion history requires the Flora Resolve callback patch.";
                r["preMod"] = nullptr; r["postMod"] = nullptr; r["pixel_value_changed"] = nullptr;
                operationValueGaps.push_back({{"eventId", v.eventId}, {"reason", r["value_observation_scope"]}});
                unavailableOperationValues.insert(v.eventId);
            }
        }
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
    try {
        consistency["after_restoration"] = pixel(controller.PickPixel(resource, x, y, sub, consistencyCast));
        if (consistency.contains("before_history")) {
            const auto &before = consistency.at("before_history").at("uintValue");
            const auto &after = consistency.at("after_restoration").at("uintValue");
            consistency["status"] = before == after ? "selected_texel_unchanged" : "selected_texel_changed";
        }
    } catch (const std::exception &e) {
        consistency["after_error"] = e.what();
    }
    Json boundary{{"status", "not_compared"},
        {"scope", "Final backend modification at the selected event versus the selected texel before and after history. Equal color components do not validate intermediate fragments or other events."}};
    if (unavailableOperationValues.contains(eid))
        boundary["reason"] = "Selected resource operation values are unavailable in this analysis runtime";
    else if (modifications.empty() || modifications.back().eventId != eid || !existing.contains(eid))
        boundary["reason"] = "No final backend modification at the selected event";
    else if (depthTarget || tex.format.type != ResourceFormatType::Regular ||
             !tex.format.compCount || tex.format.compCount > 4)
        boundary["reason"] = "Selected-event comparison supports regular color formats only";
    else if (!consistency.contains("before_history") || !consistency.contains("after_restoration"))
        boundary["reason"] = "Selected texel observation is unavailable";
    else {
        const auto post = pixel(modifications.back().postMod.col);
        auto matches = [&](const Json &observation) {
            for (uint32_t component = 0; component < tex.format.compCount; ++component)
                if (post.at("uintValue")[component] != observation.at("uintValue")[component]) return false;
            return true;
        };
        const bool before = matches(consistency.at("before_history"));
        const bool after = matches(consistency.at("after_restoration"));
        boundary.update({{"status", before && after ? "matches_selected_texel" : "differs_from_selected_texel"},
            {"event_id", eid}, {"component_count", tex.format.compCount}, {"post_modification", post},
            {"matches_before_history", before}, {"matches_after_restoration", after},
            {"comparison", "Exact backend component bits for the stored color channels; conversion differences are not treated as verified equality"}});
    }
    consistency["history_boundary"] = boundary;
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
           {"replay_consistency", consistency},
           {"unmapped_records", unmapped},
           {"cpu_write_snapshots", added},
           {"cpu_write_gaps", gaps},
           {"excluded_resource_operations", excludedOperations},
           {"resource_operation_coverage_gaps", operationCoverageGaps},
           {"resource_operation_value_gaps", operationValueGaps},
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
    if (dx12) {
        result["history_scope"]["intermediate_color_storage"] = session.halfColorHistoryPatch >= 2
            ? "RGBA16_FLOAT single-sample 2D targets use original-format GPU scratch storage at the selected mip/layer after each fragment; shader output remains float32. Other formats retain backend reconstruction."
            : session.halfColorHistoryPatch
            ? "RGBA16_FLOAT single-mip/single-layer/single-sample 2D targets use original-format GPU scratch storage after each fragment; shader output remains float32. Other formats retain backend reconstruction."
            : "Backend reconstruction: intermediate colors can be blended at a different precision from the original target, then converted on the CPU. Final event values do not validate intermediate fragments.";
        if (session.halfColorHistoryPatch >= 2)
            result["history_scope"]["subresource_filter"] =
                "Draws outside all known 2D RTV/DSV mip/layer ranges are omitted. Unknown view layouts and resource operations remain candidates; this does not validate per-primitive array-layer routing.";
        result["history_scope"]["default_view_filter"] = session.defaultViewHistoryPatch
            ? "Default typed 2D attachments select mip zero and all array layers; other mip draws are excluded. See array_layer_filter for Draw routing support."
            : "Default attachment ranges are not filtered by this runtime; history at other mips can contain unrelated Draw fragments.";
        result["history_scope"]["array_layer_filter"] = session.routedLayerHistoryPatch
            ? "Known 2D array attachment ranges are preserved in GPU scratch views. A selected-layer stencil mask filters Draw coverage; fragment counts and colors use the selected physical layer. Mesh/view-instancing remains unverified; array_layer_test_reason_status describes test-query support."
            : session.defaultViewHistoryPatch >= 2
            ? "Without a render-target array index output in the last VS/DS/GS stage, Draws only affect the view's first layer. Dynamic layer outputs, mesh shaders and view instancing remain unverified."
            : "This runtime does not exclude other array layers when the last vertex-processing stage has no layer-index output.";
    }
    if (dx12 && tex.arraysize > 1) {
        result["partial"] = true;
        uint32_t color{}, discard{}, failures{};
        if (session.layerTestStats) session.layerTestStats(&color, &discard, &failures);
        const bool probed = session.layerTestHistoryPatch && tex.dimension == 2 && tex.msSamp <= 1 && !failures && (color || discard);
        result["history_scope"]["array_layer_test_reason_status"] = failures ? "probe_failed"
            : probed ? "backend_selected_layer_probes" : "unverified";
        result["history_scope"]["array_layer_test_probes"] = {
            {"fixed_shader_queries", color}, {"original_shader_discard_queries", discard}, {"failures", failures}};
        result["history_scope"]["limitations"].push_back(probed
            ? "Single-sample 2D arrays use selected-layer GPU markers for fixed-shader tests and a stencil mask for original-PS discard queries. Per-fragment failure attribution, early depth/stencil semantics and general depth reconstruction remain backend limitations."
            : "Array-layer coverage and color reconstruction do not validate failure reasons: backend test queries can aggregate primitives from other layers. Those reasons remain unverified.");
    }
    if (session.depthRoundtripCopies)
        result["history_scope"]["limitations"].push_back(
            "Experimental depth refresh applies only to eligible isolated replays of the selected event. "
            "It does not normalize other history events or validate intermediate fragments.");
    if (unreliableD16 || reconstructedDepthValues || !operationCoverageGaps.empty() || !operationValueGaps.empty() || consistency.at("status") != "selected_texel_unchanged" ||
        boundary.at("status") == "differs_from_selected_texel")
        result["partial"] = true;
    for (const auto &r : resources)
        if (r.resourceId == resource)
            result["resource_name"] = text(r.name);
    return complete();
}
} // namespace flora
