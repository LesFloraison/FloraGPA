#include "NativeVertexDebug.h"
#include "replay/Replay.h"
#include <QFileInfo>
#include <QSaveFile>

namespace flora {
nlohmann::json debugNativeVertex(Replay &replay, const QString &tracePath,
                                 const dxbc_exec::VertexTraceOptions &selection, uint64_t maxInputBytes,
                                 const CancelCheck &cancelled, const QString &shaderPath,
                                 std::optional<uint32_t> actualIndex) {
    checkCancellation(cancelled);
    if (!shaderPath.isEmpty() &&
        (QFileInfo::exists(shaderPath) ||
         QFileInfo(shaderPath)
                 .absoluteFilePath()
                 .compare(QFileInfo(tracePath).absoluteFilePath(), Qt::CaseInsensitive) == 0))
        throw std::runtime_error("Native debug shader export requires a distinct new path");
    auto options = selection;
    if (actualIndex) {
        if (options.selection.actualIndex && options.selection.actualIndex != actualIndex)
            throw std::runtime_error("Conflicting native vertex index overrides");
        options.selection.actualIndex = actualIndex;
    }
    auto snapshot = dxbc_exec::snapshotVertexInputs(replay, options.selection, maxInputBytes, cancelled);
    auto &invocation = snapshot.invocation;
    invocation["status"] =
        options.selection.actualIndex ? "explicit_vertex_index_override" : "eligible_in_observed_replay";
    auto trace = dxbc_exec::exportVertexExecutionTrace(tracePath, snapshot.resources, options, cancelled);
    const auto &resources = snapshot.resources;
    if (!shaderPath.isEmpty()) {
        checkCancellation(cancelled);
        if (QFileInfo::exists(shaderPath))
            throw std::runtime_error("Native debug shader export requires a new path");
        QSaveFile file(shaderPath);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(resources.bytecode.data()),
                       qint64(resources.bytecode.size())) != qint64(resources.bytecode.size()))
            throw std::runtime_error("Cannot write the effective native debug shader");
        checkCancellation(cancelled);
        if (!file.commit())
            throw std::runtime_error("Cannot publish the effective native debug shader");
    }
    return {{"format", "flora.native-vertex-debug"},
            {"version", 2},
            {"invocation", invocation},
            {"trace", trace},
            {"trace_format", "flora.dxbc-execution-trace/1"},
            {"gpa_shader", resources.shader},
            {"shader_sha256", sha256(resources.bytecode)},
            {"effective_frame_sha256", resources.frameSha256},
            {"execution_model", dxbc_exec::vertexExecutionModel},
            {"captured_gpu_schedule", false}};
}
} // namespace flora
