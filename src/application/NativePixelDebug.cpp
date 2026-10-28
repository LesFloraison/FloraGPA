#include "NativePixelDebug.h"
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
namespace flora {
using Json = nlohmann::json;
Json debugNativePixel(Replay &replay, Id event, const NativePixelDebugSelection &selection,
                      const QString &tracePath, const QString &shaderPath, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (!selection.maxInputBytes || !selection.maxTraceBytes || !selection.maxSteps || tracePath.isEmpty() ||
        shaderPath.isEmpty() || QFileInfo::exists(tracePath) || QFileInfo::exists(shaderPath) ||
        QFileInfo(tracePath).absoluteFilePath().compare(QFileInfo(shaderPath).absoluteFilePath(),
                                                        Qt::CaseInsensitive) == 0)
        throw std::invalid_argument("Invalid private pixel debug limits or artifact paths");
    NativePixelDrawHistoryLimits limits;
    limits.maxBytes = limits.maxShaderBytes = selection.maxInputBytes;
    limits.maxShaderSteps = selection.maxSteps;
    limits.evaluateShaderOutputs = true;
    Json trace, invocation;
    std::vector<uint8_t> bytecode;
    Id shader = 0;
    std::string frameHash;
    auto history = collectNativePixelDrawHistory(
        replay, event, selection.pixel, limits, cancelled,
        [&](uint64_t ordinal, const dxbc_exec::InputSnapshot &source,
            const dxbc_exec::PixelTraceOptions &inputs, const std::vector<uint32_t> &samples) {
            if (ordinal != selection.primitive ||
                std::find(samples.begin(), samples.end(), selection.pixel.sample) == samples.end())
                return;
            if (!trace.is_null())
                throw std::runtime_error("Private pixel debug selection has multiple quad invocations");
            auto snapshot = source;
            uint64_t retained = source.bytecode.size();
            auto account = [&](uint64_t size) {
                if (retained > selection.maxInputBytes || size > selection.maxInputBytes - retained)
                    throw std::runtime_error("Private pixel debug snapshot copy budget exceeded");
                retained += size;
            };
            account(0);
            for (const auto &[slot, values] : snapshot.inputs.constantBuffers)
                account(values.size() * sizeof(dxbc_exec::Value));
            if (snapshot.inputs.sharedMemory)
                throw std::runtime_error("Private pixel debug cannot consume compute shared memory");
            if (snapshot.inputs.memory) {
                for (const auto &[id, buffer] : snapshot.inputs.memory->buffers()) {
                    account(buffer.bytes.size());
                    account(buffer.valid.size());
                }
                snapshot.inputs.memory = std::make_shared<dxbc_exec::Memory>(*snapshot.inputs.memory);
            }
            if (snapshot.inputs.textures) {
                for (const auto &[id, texture] : snapshot.inputs.textures->resources) {
                    account(texture.storage.bytes.size());
                    account(texture.storage.valid.size());
                }
                snapshot.inputs.textures =
                    std::make_shared<dxbc_exec::TextureInputs>(*snapshot.inputs.textures);
            }
            auto options = inputs;
            options.maxSteps = selection.maxSteps;
            options.maxBytes = selection.maxTraceBytes;
            options.inputProvenance["resource_boundary"] = "before_private_primitive";
            options.inputProvenance["completed_primitives"] = ordinal;
            trace = dxbc_exec::exportPixelExecutionTrace(tracePath, snapshot, options, cancelled);
            invocation = {{"status", "explicit_private_primitive"},
                          {"gpa_event", event},
                          {"gpa_shader", source.shader},
                          {"primitive_ordinal", ordinal},
                          {"original_primitive_id", inputs.inputProvenance.at("original_primitive_id")},
                          {"selected_lane", inputs.selectedLane},
                          {"active_mask", inputs.activeMask},
                          {"candidate_raster_samples", samples},
                          {"resource_boundary", "before_private_primitive"},
                          {"quad_input_provenance", options.inputProvenance},
                          {"original_fragment_eligibility", nullptr},
                          {"instance", nullptr}};
            bytecode = source.bytecode;
            shader = source.shader;
            frameHash = source.frameSha256;
        });
    if (trace.is_null())
        throw std::runtime_error("Selected private primitive/sample has no observed PS invocation");
    invocation["selection"] = history.at("selection");
    checkCancellation(cancelled);
    QSaveFile file(shaderPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(bytecode.data()), qint64(bytecode.size())) !=
            qint64(bytecode.size()) ||
        !file.commit())
        throw std::runtime_error("Cannot publish private pixel debug shader");
    return {{"format", "flora.native-pixel-debug"},
            {"version", 1},
            {"invocation", invocation},
            {"private_history", std::move(history)},
            {"trace", trace},
            {"trace_format", "flora.dxbc-execution-trace/1"},
            {"gpa_shader", shader},
            {"shader_sha256", sha256(bytecode)},
            {"effective_frame_sha256", frameHash},
            {"execution_model", dxbc_exec::PixelQuad::executionModel},
            {"captured_gpu_schedule", false}};
}
} // namespace flora
