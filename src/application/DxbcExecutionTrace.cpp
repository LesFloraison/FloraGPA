#include "DxbcExecutionTrace.h"
#include "DxbcGpuOperations.h"
#include "ShaderSourceLines.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <bit>

namespace flora::dxbc_exec {
namespace {
using Json = nlohmann::json;
Json reg(const Register &r) { return {{"dxbc_operand_kind", r.kind}, {"indices", r.indices}}; }
Json value(const Value &v) {
    if (v.valid > 15)
        throw std::runtime_error("Invalid trace register validity mask");
    return {{"bits", v.bits}, {"valid_components", v.valid}};
}
std::string hex(Bytes bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return result;
}
void validity(Bytes bytes) {
    if (std::any_of(bytes.begin(), bytes.end(), [](auto v) { return v > 1; }))
        throw std::runtime_error("Invalid trace byte validity");
}
Json counter(const Counter &c) {
    if (c.kind != CounterKind::Ordered && c.kind != CounterKind::Append)
        throw std::runtime_error("Invalid trace counter kind");
    return {{"kind", c.kind == CounterKind::Ordered ? "ordered" : "append"},
            {"value", c.value ? Json(*c.value) : Json(nullptr)}};
}
Json changes(const std::vector<MemoryChange> &items) {
    Json result = Json::array();
    for (const auto &c : items) {
        if (c.before.size() != c.after.size() || c.before.size() != c.beforeValid.size() ||
            c.before.size() != c.afterValid.size() || c.before.size() > UINT64_MAX - c.offset)
            throw std::runtime_error("Invalid trace memory change extent");
        validity(c.beforeValid);
        validity(c.afterValid);
        result.push_back({{"resource", c.resource},
                          {"offset", c.offset},
                          {"before_hex", hex(c.before)},
                          {"after_hex", hex(c.after)},
                          {"before_valid_hex", hex(c.beforeValid)},
                          {"after_valid_hex", hex(c.afterValid)}});
    }
    return result;
}
class Journal {
    QSaveFile file;
    const CancelCheck &cancelled;
    uint64_t limit;

  public:
    uint64_t bytes = 0, records = 0;
    Journal(const QString &path, uint64_t maxBytes, const CancelCheck &cancel)
        : file(path), cancelled(cancel), limit(maxBytes) {
        if (!limit || QFileInfo::exists(path))
            throw std::runtime_error("Execution trace requires a new path and a nonzero byte budget");
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot create execution trace journal");
    }
    void write(const Json &row) {
        checkCancellation(cancelled);
        const auto text = row.dump();
        if (text.size() >= limit - bytes)
            throw std::runtime_error("Execution trace byte budget exceeded");
        if (file.write(text.data(), qint64(text.size())) != qint64(text.size()) || file.write("\n", 1) != 1)
            throw std::runtime_error("Cannot write execution trace journal");
        bytes += text.size() + 1;
        ++records;
    }
    void buffer(const char *space, uint64_t id, const Buffer &b) {
        if (b.bytes.size() != b.valid.size())
            throw std::runtime_error("Invalid initial trace storage extent");
        write({{"record", "storage"}, {"space", space}, {"resource", id}, {"bytes", b.bytes.size()}});
        for (size_t offset = 0; offset < b.bytes.size(); offset += 4096) {
            checkCancellation(cancelled);
            const auto size = std::min(size_t(4096), b.bytes.size() - offset);
            const Bytes valid(b.valid.data() + offset, size);
            validity(valid);
            write({{"record", "storage_bytes"},
                   {"space", space},
                   {"resource", id},
                   {"offset", offset},
                   {"bits_hex", hex(Bytes(b.bytes.data() + offset, size))},
                   {"valid_hex", hex(valid)}});
        }
    }
    void commit() {
        checkCancellation(cancelled);
        if (!file.commit())
            throw std::runtime_error("Cannot publish execution trace journal");
    }
};
} // namespace
Json executionTraceStep(const GroupStep &group) {
    const auto &s = group.step;
    Json registers = Json::array(), counters = Json::array();
    for (const auto &c : s.changes)
        registers.push_back(
            {{"register", reg(c.destination)}, {"before", value(c.before)}, {"after", value(c.after)}});
    for (const auto &c : s.counterChanges)
        counters.push_back({{"view", c.view}, {"before", counter(c.before)}, {"after", counter(c.after)}});
    return {{"record", "step"},
            {"sequence", group.sequence},
            {"lane", group.lane},
            {"lane_sequence", s.sequence},
            {"barrier_epoch", group.barrierEpoch},
            {"released_barrier", group.releasedBarrier},
            {"instruction", s.instruction},
            {"word_offset", s.wordOffset},
            {"next_instruction", s.nextInstruction ? Json(*s.nextInstruction) : Json(nullptr)},
            {"return_instruction_stack", s.callstack},
            {"register_changes", registers},
            {"discarded", s.discarded},
            {"buffer_changes", changes(s.memoryChanges)},
            {"shared_changes", changes(s.sharedMemoryChanges)},
            {"texture_changes", changes(s.textureChanges)},
            {"counter_changes", counters},
            {"memory_gpu_assisted", s.memoryGpuAssisted},
            {"instruction_gpu_assisted", s.instructionGpuAssisted},
            {"sync_flags", s.syncFlags},
            {"sync_loop_iterations", s.syncLoopIterations},
            {"atomic", s.atomic}};
}
static Json exportExecutionTrace(const QString &path, const InputSnapshot &snapshot,
                                 const GroupTraceOptions &options, bool vertex,
                                 const VertexInputSelection &selection, const CancelCheck &cancelled,
                                 const PixelTraceOptions *pixel = nullptr) {
    checkCancellation(cancelled);
    if (snapshot.stage != (pixel    ? 4u
                           : vertex ? 0u
                                    : 5u) ||
        !snapshot.event || !snapshot.shader || !options.maxSteps || !options.maxBytes)
        throw std::runtime_error("Invalid native execution trace request");
    const Program program(snapshot.bytecode);
    if (program.stage() != (pixel    ? 0u
                            : vertex ? 1u
                                     : 5u) ||
        (vertex && !program.sharedDeclarations().empty()))
        throw std::runtime_error("Execution trace shader stage mismatch");
    const auto size = pixel    ? std::array<uint32_t, 3>{4, 1, 1}
                      : vertex ? std::array<uint32_t, 3>{1, 1, 1}
                               : program.threadGroupSize();
    for (size_t i = 0; i < 3; ++i)
        if (options.thread[i] >= size[i])
            throw std::runtime_error("Execution trace thread is outside the shader thread group");
    std::unique_ptr<Group> group;
    std::unique_ptr<Lane> vertexLane;
    std::unique_ptr<PixelQuad> quad;
    if (pixel) {
        if (pixel->selectedLane >= 4 || pixel->activeMask > 15 || !pixel->inputProvenance.is_object())
            throw std::runtime_error("Invalid pixel trace lane/mask/provenance");
        std::array<Inputs, 4> inputs;
        for (unsigned i = 0; i < 4; ++i) {
            inputs[i] = snapshot.inputs;
            for (const auto &[r, v] : pixel->registers[i]) {
                if (r.kind != 1 && r.kind != 35)
                    throw std::runtime_error("Pixel trace input is not an input register");
                if (!inputs[i].registers.emplace(r, v).second)
                    throw std::runtime_error("Pixel trace input register is duplicated");
            }
        }
        quad = std::make_unique<PixelQuad>(program, std::move(inputs), pixel->activeMask, options.maxSteps,
                                           cancelled);
    } else if (vertex)
        vertexLane = std::make_unique<Lane>(program, snapshot.inputs, options.maxSteps);
    else
        group = std::make_unique<Group>(program, snapshot.inputs, options.group, options.maxSteps, cancelled);
    const uint32_t selected =
        pixel ? pixel->selectedLane
              : options.thread[0] + size[0] * (options.thread[1] + size[1] * options.thread[2]);
    const auto shaderHash =
        QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(snapshot.bytecode.data()),
                                                snapshot.bytecode.size()),
                                 QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    Journal journal(path, options.maxBytes, cancelled);
    Json constantLengths = Json::array();
    for (const auto &[slot, values] : snapshot.inputs.constantBuffers)
        constantLengths.push_back({{"slot", slot}, {"elements", values.size()}});
    Json header{{"record", "header"},
                {"format", "flora.dxbc-execution-trace"},
                {"version", 1},
                {"stage", "cs"},
                {"scope", "one_cooperative_group"},
                {"invocation_eligibility", "not_established_by_trace_writer"},
                {"execution_model", Group::executionModel},
                {"captured_gpu_schedule", false},
                {"input_source", InputSnapshot::source},
                {"class_bindings", snapshot.classBindings},
                {"resource_observation", snapshot.resourceObservation},
                {"frame_sha256", snapshot.frameSha256},
                {"shader_sha256", shaderHash},
                {"gpa_event", snapshot.event},
                {"gpa_shader", snapshot.shader},
                {"replay_generation", snapshot.replayGeneration},
                {"adapter",
                 {{"luid", snapshot.adapterLuid},
                  {"vendor", snapshot.adapterVendor},
                  {"device", snapshot.adapterDevice}}},
                {"group", options.group},
                {"thread", options.thread},
                {"selected_lane", selected},
                {"group_size", size},
                {"max_steps", options.maxSteps},
                {"constant_buffer_lengths", constantLengths},
                {"max_bytes", options.maxBytes}};
    if (vertex) {
        for (const auto *key : {"group", "thread", "group_size"})
            header.erase(key);
        header.update(
            {{"stage", "vs"},
             {"scope", "one_vertex"},
             {"execution_model", vertexExecutionModel},
             {"input_reference", selection.reference},
             {"instance", selection.instance},
             {"index_override", selection.actualIndex ? Json(*selection.actualIndex) : Json(nullptr)}});
    }
    if (pixel) {
        for (const auto *key : {"group", "thread", "group_size"})
            header.erase(key);
        header.update({{"stage", "ps"},
                       {"scope", "one_pixel_quad"},
                       {"execution_model", PixelQuad::executionModel},
                       {"initial_active_mask", pixel->activeMask},
                       {"quad_input_provenance", pixel->inputProvenance}});
    }
    journal.write(header);
    const auto source = shaderSourceLines(snapshot.bytecode);
    journal.write({{"record", "source"}, {"data", source}});
    const auto locations = shaderLinesByOffset(source);
    for (size_t i = 0; i < program.instructions().size(); ++i) {
        const auto &inst = program.instructions()[i];
        const auto key = std::to_string(uint64_t(inst.wordOffset) * 4);
        journal.write({{"record", "instruction"},
                       {"index", i},
                       {"word_offset", inst.wordOffset},
                       {"dxbc_opcode", inst.opcode},
                       {"declaration", inst.declaration},
                       {"words", inst.words},
                       {"source", locations.value(key, Json(nullptr))}});
    }
    for (const auto &[binding, b] : snapshot.bindings)
        journal.write({{"record", "binding"},
                       {"register", reg(binding)},
                       {"resource", b.resource},
                       {"view", b.view},
                       {"offset", b.offset},
                       {"size", b.size},
                       {"stride", b.stride},
                       {"dxgi_format", b.format},
                       {"experimental_view", b.experimentalView}});
    for (const auto &[binding, reason] : snapshot.unavailable)
        journal.write({{"record", "unavailable_binding"}, {"register", reg(binding)}, {"reason", reason}});
    for (const auto &[view, reason] : snapshot.unavailableCounters)
        journal.write({{"record", "unavailable_counter"}, {"view", view}, {"reason", reason}});
    for (const auto &[slot, values] : snapshot.inputs.constantBuffers)
        for (size_t i = 0; i < values.size(); ++i)
            journal.write(
                {{"record", "constant"}, {"slot", slot}, {"element", i}, {"value", value(values[i])}});
    for (uint32_t lane = 0; lane < size[0] * size[1] * size[2]; ++lane)
        for (const auto &[r, v] : (pixel    ? quad->lane(lane).registers()
                                   : vertex ? vertexLane->registers()
                                            : group->lane(lane).registers()))
            journal.write(
                {{"record", "register"}, {"lane", lane}, {"register", reg(r)}, {"value", value(v)}});
    if (snapshot.inputs.memory) {
        for (const auto &[id, b] : snapshot.inputs.memory->buffers())
            journal.buffer("buffer", id, b);
        for (const auto &[id, c] : snapshot.inputs.memory->counters())
            journal.write({{"record", "counter"}, {"view", id}, {"value", counter(c)}});
    }
    if (group)
        for (const auto &[id, b] : group->sharedMemory().buffers())
            journal.buffer("shared", id, b);
    if (snapshot.inputs.textures) {
        const auto &textures = *snapshot.inputs.textures;
        for (const auto &[id, metadata] : textures.metadata)
            journal.write({{"record", "texture_metadata"},
                           {"resource", id},
                           {"gpa_resource_type", metadata.resource.type},
                           {"descriptor", metadata.resource.desc},
                           {"min_lod_bits", std::bit_cast<uint32_t>(metadata.minLod)}});
        for (const auto &[r, sampler] : textures.samplers)
            journal.write(
                {{"record", "sampler"},
                 {"register", reg(r)},
                 {"object", sampler.object},
                 {"experimental", sampler.experimental},
                 {"d3d11_descriptor_bits", sampler.descriptor ? Json(*sampler.descriptor) : Json(nullptr)}});
        const auto queries = [&](const Json &binding, const SampleQueryInput &q) {
            journal.write({{"record", "sample_queries"},
                           {"register", binding},
                           {"observed", q.observed},
                           {"count", q.count},
                           {"resource_samples", q.resourceSamples},
                           {"quality", q.quality},
                           {"forced_samples", q.forcedSamples},
                           {"multisample_enabled", q.multisampleEnabled},
                           {"position_bits", q.positions}});
        };
        if (textures.rasterizerQueries)
            queries(reg({14, {}}), *textures.rasterizerQueries);
        for (const auto &[r, view] : textures.views) {
            journal.write({{"record", "texture_view"},
                           {"register", reg(r)},
                           {"resource", view.resource ? Json(*view.resource) : Json(nullptr)},
                           {"dxgi_format", view.format},
                           {"d3d11_dimension", view.dimension},
                           {"first_mip", view.firstMip},
                           {"mip_count", view.mipCount},
                           {"first_layer", view.firstLayer},
                           {"layer_count", view.layerCount},
                           {"depth_slices", view.depthSlices ? Json(*view.depthSlices) : Json(nullptr)},
                           {"plane_slice", view.planeSlice ? Json(*view.planeSlice) : Json(nullptr)},
                           {"d3d11_descriptor", view.nativeDescriptor}});
            if (view.queries)
                queries(reg(r), *view.queries);
            if (view.samples) {
                const auto &s = *view.samples;
                journal.write({{"record", "multisample_observation"},
                               {"register", reg(r)},
                               {"dxgi_format", s.format},
                               {"dxbc_return_type", s.returnType},
                               {"d3d11_dimension", s.dimension},
                               {"descriptor", s.resource.desc},
                               {"first_layer", s.firstLayer},
                               {"layer_count", s.layerCount},
                               {"values", s.values.size()},
                               {"last_value", "spatial_out_of_bounds"}});
                for (size_t i = 0; i < s.values.size(); i += 256) {
                    Json values = Json::array();
                    for (size_t j = i; j < std::min(i + 256, s.values.size()); ++j)
                        values.push_back(s.values[j]);
                    journal.write({{"record", "multisample_values"},
                                   {"register", reg(r)},
                                   {"first", i},
                                   {"bits", values}});
                }
            }
        }
        for (const auto &[id, t] : snapshot.inputs.textures->resources) {
            journal.buffer("texture", id, t.storage);
            for (const auto &s : t.subresources)
                journal.write({{"record", "texture_subresource"},
                               {"resource", id},
                               {"index", s.index},
                               {"mip", s.mip},
                               {"layer", s.layer},
                               {"width", s.width},
                               {"height", s.height},
                               {"depth", s.depth},
                               {"row_pitch", s.rowPitch},
                               {"slice_pitch", s.slicePitch},
                               {"offset", s.offset},
                               {"size", s.size}});
        }
    }
    uint64_t steps = 0, selectedSteps = 0;
    const auto gpuStart = snapshot.inputs.gpu ? snapshot.inputs.gpu->operations() : 0;
    while (pixel ? !quad->finished() : vertex ? !vertexLane->finished() : !group->finished()) {
        checkCancellation(cancelled);
        Json row;
        if (pixel) {
            auto step = quad->step();
            row = executionTraceStep(GroupStep{step.sequence, 0, step.lane, std::move(step.step), false});
            row["helper_before"] = step.helper;
            row["active_mask_after"] = step.activeMask;
        } else {
            const auto step = vertex ? GroupStep{steps, 0, 0, vertexLane->step(), false} : group->step();
            row = executionTraceStep(step);
        }
        journal.write(row);
        ++steps;
        selectedSteps += row.at("lane") == selected;
    }
    Json summary{{"record", "complete"},
                 {"steps", steps},
                 {"selected_steps", selectedSteps},
                 {"sampling_batches", pixel   ? quad->samplingBatches()
                                      : group ? group->samplingBatches()
                                              : 0},
                 {"completed", true},
                 {"gpu_operations", snapshot.inputs.gpu ? snapshot.inputs.gpu->operations() - gpuStart : 0},
                 {"gpu", snapshot.inputs.gpu ? snapshot.inputs.gpu->identity() : Json(nullptr)}};
    if (pixel) {
        summary["derivative_batches"] = quad->derivativeBatches();
        summary["final_active_mask"] = quad->activeMask();
    }
    journal.write(summary);
    journal.commit();
    summary["bytes"] = journal.bytes;
    summary["records"] = journal.records;
    return summary;
}
Json exportGroupExecutionTrace(const QString &path, const InputSnapshot &snapshot,
                               const GroupTraceOptions &options, const CancelCheck &cancelled) {
    return exportExecutionTrace(path, snapshot, options, false, {}, cancelled);
}
Json exportVertexExecutionTrace(const QString &path, const InputSnapshot &snapshot,
                                const VertexTraceOptions &options, const CancelCheck &cancelled) {
    GroupTraceOptions common;
    common.maxSteps = options.maxSteps;
    common.maxBytes = options.maxBytes;
    return exportExecutionTrace(path, snapshot, common, true, options.selection, cancelled);
}
Json exportPixelExecutionTrace(const QString &path, const InputSnapshot &snapshot,
                               const PixelTraceOptions &options, const CancelCheck &cancelled) {
    GroupTraceOptions common;
    common.maxSteps = options.maxSteps;
    common.maxBytes = options.maxBytes;
    return exportExecutionTrace(path, snapshot, common, false, {}, cancelled, &options);
}
} // namespace flora::dxbc_exec
