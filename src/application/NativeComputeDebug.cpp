#include "NativeComputeDebug.h"
#include "replay/Replay.h"
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>

namespace flora {
using Json = nlohmann::json;
Json debugNativeCompute(Replay &replay, const QString &tracePath,
                        const dxbc_exec::GroupTraceOptions &selection, uint64_t maxInputBytes,
                        const CancelCheck &cancelled, const QString &shaderPath) {
    checkCancellation(cancelled);
    if (!shaderPath.isEmpty() &&
        (QFileInfo::exists(shaderPath) ||
         QFileInfo(shaderPath)
                 .absoluteFilePath()
                 .compare(QFileInfo(tracePath).absoluteFilePath(), Qt::CaseInsensitive) == 0))
        throw std::runtime_error("Native debug shader export requires a distinct new path");
    const auto event = replay.frame().event(replay.options().until);
    if (event.type != 0x35 && event.type != 0x36)
        throw std::runtime_error("Native thread debugging requires a GPA Dispatch event");
    if (replay.options().disabled.contains(event.id))
        throw std::runtime_error("Selected dispatch is disabled by the experiment");
    Json invocation;
    const auto snapshot = dxbc_exec::snapshotResourceInputs(replay, 5, maxInputBytes, cancelled, [&] {
        std::array<uint32_t, 3> groups{};
        if (event.type == 0x35) {
            if (event.args.size() != 3)
                throw std::runtime_error("Invalid direct dispatch arguments");
            std::copy(event.args.begin(), event.args.end(), groups.begin());
        } else {
            if (event.args.size() != 1 || event.args[0] % 4)
                throw std::runtime_error("Invalid indirect dispatch argument offset");
            const auto descriptor = replay.frame().resource(event.argumentBuffer);
            if (descriptor.type != 0x83 || descriptor.desc.size() != 6 ||
                !(descriptor.desc[4] & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) ||
                descriptor.desc[0] > maxInputBytes || uint64_t(event.args[0]) + 12 > descriptor.desc[0])
                throw std::runtime_error(
                    "Indirect dispatch argument buffer is invalid or exceeds the input budget");
            const auto bytes = replay.readBuffer(event.argumentBuffer);
            Reader reader(Bytes(bytes).subspan(event.args[0], 12));
            groups = reader.array<uint32_t, 3>();
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            if (groups[axis] > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
                throw std::runtime_error("Dispatch dimensions exceed the D3D11 limit");
            if (selection.group[axis] >= groups[axis])
                throw std::runtime_error("Selected compute group is outside the effective dispatch");
        }
        const auto predicate = replay.observeBoundPredicate(cancelled);
        if (!predicate.allowsExecution)
            throw std::runtime_error("Compute invocation eligibility is unknown: " + predicate.source);
        if (!*predicate.allowsExecution)
            throw std::runtime_error("Selected dispatch is skipped by the observed replay predicate");
        invocation = {{"status", "eligible_in_observed_replay"},
                      {"gpa_event", event.id},
                      {"dispatch_kind", event.type == 0x35 ? "direct" : "indirect"},
                      {"dispatch_groups", groups},
                      {"argument_buffer", event.argumentBuffer},
                      {"argument_offset", event.type == 0x36 ? Json(event.args[0]) : Json(nullptr)},
                      {"group", selection.group},
                      {"thread", selection.thread},
                      {"predicate",
                       {{"resource", predicate.resource},
                        {"comparison", predicate.comparison},
                        {"flags", predicate.flags},
                        {"source", predicate.source},
                        {"query_value", predicate.queryValue ? Json(*predicate.queryValue) : Json(nullptr)},
                        {"allows_execution", true},
                        {"original_query_recovered", false}}}};
    });
    auto trace = dxbc_exec::exportGroupExecutionTrace(tracePath, snapshot, selection, cancelled);
    if (!shaderPath.isEmpty()) {
        checkCancellation(cancelled);
        if (QFileInfo::exists(shaderPath))
            throw std::runtime_error("Native debug shader export requires a new path");
        QSaveFile file(shaderPath);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(snapshot.bytecode.data()),
                       qint64(snapshot.bytecode.size())) != qint64(snapshot.bytecode.size()))
            throw std::runtime_error("Cannot write the effective native debug shader");
        checkCancellation(cancelled);
        if (!file.commit())
            throw std::runtime_error("Cannot publish the effective native debug shader");
    }
    const dxbc_exec::Program program(snapshot.bytecode);
    invocation["group_size"] = program.threadGroupSize();
    return {{"format", "flora.native-compute-debug"},
            {"version", 1},
            {"invocation", invocation},
            {"trace", trace},
            {"trace_format", "flora.dxbc-execution-trace/1"},
            {"gpa_shader", snapshot.shader},
            {"shader_sha256", sha256(snapshot.bytecode)},
            {"effective_frame_sha256", snapshot.frameSha256},
            {"execution_model", dxbc_exec::Group::executionModel},
            {"captured_gpu_schedule", false}};
}
} // namespace flora
