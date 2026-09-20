#include "Experiment.h"
#include "BlendEdits.h"
#include "CommandEdits.h"
#include "DepthStencilEdits.h"
#include "RasterizerEdits.h"
#include "SamplerEdits.h"
#include "SetterEdits.h"
#include "ShaderInspector.h"
#include "SrvEdits.h"
#include "core/BufferBindings.h"
#include "core/UavCounters.h"
#include <QFile>
#include <QSaveFile>

namespace flora {
using Json = nlohmann::json;
namespace {
Id identifier(const Json &value) {
    if (value.is_number_unsigned())
        return value.get<Id>();
    if (value.is_number_integer() && value.get<int64_t>() >= 0)
        return Id(value.get<int64_t>());
    throw std::runtime_error("Experiment identifier must be an unsigned integer");
}
std::vector<uint8_t> asset(const Json &value) {
    auto text = value.at("data").get<std::string>();
    auto decoded = QByteArray::fromBase64Encoding(QByteArray(text.data(), qsizetype(text.size())),
                                                  QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        throw std::runtime_error("Invalid experiment asset base64");
    std::vector<uint8_t> bytes(decoded.decoded.begin(), decoded.decoded.end());
    if (sha256(bytes) != value.at("sha256").get<std::string>())
        throw std::runtime_error("Experiment asset checksum mismatch");
    return bytes;
}
} // namespace
Experiment::Experiment(const Frame &frame) {
    project_ = {{"format", "FloraGPA experiment 1"},
                {"frame_sha256", frame.sha256()},
                {"frame_name", QString::fromStdWString(frame.path().filename().wstring()).toStdString()},
                {"cursor", 0},
                {"history", Json::array()}};
}
size_t Experiment::revision() const { return size_t(identifier(project_.at("cursor"))); }
void Experiment::load(const QString &path, const Frame &frame) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open experiment");
    auto bytes = file.readAll();
    auto candidate = Json::parse(bytes.begin(), bytes.end());
    if (candidate.at("format") != "FloraGPA experiment 1" || candidate.at("frame_sha256") != frame.sha256())
        throw std::runtime_error("Experiment format or capture hash mismatch");
    if (!candidate.at("history").is_array() ||
        identifier(candidate.at("cursor")) > candidate.at("history").size())
        throw std::runtime_error("Invalid experiment cursor");
    auto old = project_;
    project_ = std::move(candidate);
    try {
        ReplayOptions check;
        apply(frame, check);
    } catch (...) {
        project_ = std::move(old);
        throw;
    }
}
void Experiment::save(const QString &path) const {
    auto text = project_.dump(2) + "\n";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()) || !file.commit())
        throw std::runtime_error("Cannot save experiment atomically");
}
void Experiment::apply(const Frame &frame, ReplayOptions &options) const {
    options.editedEvents = false;
    options.disabled.clear();
    options.shaders.clear();
    options.textures.clear();
    options.commandPayloads.clear();
    options.updateSources.clear();
    options.buffers.clear();
    options.initialUavCounters.clear();
    options.uavCounters.clear();
    options.predicateSetters.clear();
    options.samplerSetters.clear();
    options.samplerEdits.clear();
    options.srvEdits.clear();
    options.depthStencilEdits.clear();
    options.rasterizerEdits.clear();
    options.blendEdits.clear();
    std::map<Id, Json> pipeline;
    std::map<Id, std::map<std::pair<unsigned, unsigned>, Json>> samplers;
    std::map<Id, std::map<std::pair<unsigned, unsigned>, Json>> srvs;
    for (size_t i = 0; i < revision(); ++i) {
        auto &operations = project_.at("history").at(i).at("operations");
        if (!operations.is_array())
            throw std::runtime_error("Invalid experiment operation list");
        for (auto &op : operations) {
            // Re-enabling a draw is still an explicit experiment with edited SO history.
            options.editedEvents |= op.contains("event");
            auto kind = op.at("kind").get<std::string>();
            if (kind == "enabled") {
                auto id = identifier(op.at("event"));
                auto &e = frame.entry(id);
                if (e.category != 7 || (!isDraw(e.type) && !isWritableCommand(e.type)) ||
                    !op.at("value").is_boolean())
                    throw std::runtime_error("Unsupported event enable operation");
                if (!isDraw(e.type))
                    validateWritableCommand(frame, id);
                if (op.at("value").get<bool>())
                    options.disabled.erase(id);
                else
                    options.disabled.insert(id);
            } else if (kind == "pipeline") {
                auto event = identifier(op.at("event"));
                auto values = normalizePipeline(op.at("values"));
                mergePipeline(pipeline[event], values);
                const auto depth = depthPipelineFields(pipeline[event]);
                if (!depth.empty())
                    options.depthStencilEdits[event] = depthStencilEdit(frame, event, depth);
                if (pipeline[event].contains("rasterizer") || pipeline[event].contains("viewports") ||
                    pipeline[event].contains("scissors"))
                    options.rasterizerEdits[event] = rasterizerEdit(frame, event, pipeline[event]);
                if (pipeline[event].contains("blend_state") || pipeline[event].contains("blend_factor") ||
                    pipeline[event].contains("sample_mask"))
                    options.blendEdits[event] = blendEdit(frame, event, pipeline[event]);
            } else if (kind == "srv_descriptor") {
                const auto event = identifier(op.at("event"));
                const auto stage = srvStage(op.at("stage").get<std::string>());
                const auto slot = srvSlot(op.at("slot"));
                auto &patch = srvs[event][{stage, slot}];
                patch = mergeSrv(patch, op.at("values"));
            } else if (kind == "sampler") {
                const auto event = identifier(op.at("event"));
                const auto stage = samplerStage(op.at("stage").get<std::string>());
                const auto slot = samplerSlot(op.at("slot"));
                const auto values = normalizeSampler(op.at("values"));
                auto &patch = samplers[event][{stage, slot}];
                if (patch.is_null())
                    patch = Json::object();
                patch.update(values);
            } else if (kind == "setter") {
                const auto id = identifier(op.at("event"));
                if (samplerSetterStage(frame.entry(id).type))
                    options.samplerSetters[id] = validateSamplerSetter(frame, id, op.at("values"));
                else
                    options.predicateSetters[id] = validatePredicateSetter(frame, id, op.at("values"));
            } else if (kind == "clear") {
                auto id = identifier(op.at("event"));
                validateWritableCommand(frame, id);
                options.commandPayloads[id] =
                    patchClear(frame.entry(id).type, frame.payload(id), op.at("values"));
            } else if (kind == "update_source") {
                auto id = identifier(op.at("event"));
                auto layout = updateSourceLayout(frame, id);
                auto bytes = asset(op.at("asset"));
                if (bytes.size() != layout.size)
                    throw std::runtime_error("Update source byte count mismatch: expected " +
                                             std::to_string(layout.size));
                options.updateSources[id] = std::move(bytes);
            } else if (kind == "buffer") {
                auto event = identifier(op.at("event")), resource = identifier(op.at("resource"));
                auto offset = identifier(op.at("offset"));
                auto bytes = asset(op.at("asset"));
                validateBufferPatch(frame, event, resource, offset, bytes.size());
                options.buffers[event][resource].push_back({offset, std::move(bytes)});
            } else if (kind == "uav_counter" || kind == "initial_uav_counter") {
                auto view = identifier(op.at("view")), value = identifier(op.at("value"));
                if (value > UINT32_MAX)
                    throw std::runtime_error("UAV counter must be a uint32 integer");
                std::optional<Id> event;
                if (kind == "uav_counter")
                    event = identifier(op.at("event"));
                validateCounterEdit(frame, view, event);
                if (event)
                    options.uavCounters[*event][view] = uint32_t(value);
                else
                    options.initialUavCounters[view] = uint32_t(value);
            } else if (kind == "shader") {
                auto id = identifier(op.at("resource"));
                auto r = frame.resource(id);
                if (r.type < 0x90 || r.type > 0x95)
                    throw std::runtime_error("Shader replacement target type mismatch");
                auto bytes = asset(op.at("asset"));
                if (bytes.size() < 32 || std::memcmp(bytes.data(), "DXBC", 4))
                    throw std::runtime_error("Replacement is not DXBC");
                options.shaders[id] = std::move(bytes);
            } else if (kind == "texture") {
                auto id = identifier(op.at("resource"));
                auto r = frame.resource(id);
                auto info = textureInfo(r);
                if (info.samples != 1 || !info.mips || info.mips > 32 || !info.layers || r.type == 0x87)
                    throw std::runtime_error("Texture replacement requires live non-MSAA storage");
                uint64_t size = 0;
                for (UINT layer = 0; layer < info.layers; ++layer)
                    for (UINT mip = 0; mip < info.mips; ++mip) {
                        auto [pitch, rows] = pitches(std::max(1u, info.width >> mip),
                                                     std::max(1u, info.height >> mip), info.format);
                        auto count = uint64_t(pitch) * rows * std::max(1u, info.depth >> mip);
                        if (count > SIZE_MAX - size)
                            throw std::runtime_error("Texture size overflow");
                        size += count;
                    }
                auto bytes = asset(op.at("asset"));
                if (bytes.size() != size)
                    throw std::runtime_error("Texture replacement byte count mismatch");
                options.textures[id] = std::move(bytes);
            } else
                throw std::runtime_error("Experiment operation migration pending: " + kind);
        }
    }
    // Descriptor inheritance uses final setter edits, regardless of history order.
    for (const auto &[event, bindings] : srvs)
        for (const auto &[target, patch] : bindings)
            options.srvEdits[event][target] =
                nativeSrv(effectiveSrv(frame, event, target.first, target.second, patch));
    for (const auto &[event, bindings] : samplers) {
        const auto state = samplerState(frame, event, options.samplerSetters);
        for (const auto &[target, patch] : bindings) {
            auto values = samplerDescriptor(frame, state.stages[target.first].samplers[target.second]);
            values.update(patch);
            options.samplerEdits[event][target] = nativeSampler(values);
        }
    }
}
bool Experiment::enabled(Id event) const {
    bool enabled = true;
    for (size_t i = 0; i < revision(); ++i)
        for (auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "enabled" && identifier(op.at("event")) == event)
                enabled = op.at("value").get<bool>();
    return enabled;
}
void Experiment::setEnabled(const Frame &frame, Id event, bool enabled) {
    auto &e = frame.entry(event);
    if (e.category != 7)
        throw std::runtime_error("Select an API command");
    if (!isDraw(e.type))
        validateWritableCommand(frame, event);
    auto history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", std::string(enabled ? "Enable event " : "Disable event ") + std::to_string(event)},
         {"operations", Json::array({{{"kind", "enabled"}, {"event", event}, {"value", enabled}}})}});
    project_["history"] = std::move(history);
    project_["cursor"] = project_["history"].size();
}
Json Experiment::depthStencil(const Frame &frame, Id event) const {
    auto result = capturedDepthStencil(frame, event);
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "pipeline" && identifier(op.at("event")) == event)
                mergeDepthStencil(result, depthPipelineFields(normalizePipeline(op.at("values"))));
    return result;
}
void Experiment::setDepthStencil(const Frame &frame, Id event, const Json &values) {
    const auto normalized = normalizeDepthStencil(values);
    depthStencilEdit(frame, event, normalized);
    auto history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "Depth/stencil event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "pipeline"}, {"event", event}, {"values", values}}})}});
    project_["history"] = std::move(history);
    project_["cursor"] = project_["history"].size();
}
Json Experiment::rasterizer(const Frame &frame, Id event) const {
    Json values = Json::object();
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "pipeline" && identifier(op.at("event")) == event)
                mergePipeline(values, normalizePipeline(op.at("values")));
    return effectiveRasterizer(frame, event, values);
}
void Experiment::setRasterizer(const Frame &frame, Id event, const Json &values) {
    setPipeline(frame, event, values, "Rasterizer");
}
Json Experiment::blend(const Frame &frame, Id event) const {
    Json values = Json::object();
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "pipeline" && identifier(op.at("event")) == event)
                mergePipeline(values, normalizePipeline(op.at("values")));
    return effectiveBlend(frame, event, values);
}
void Experiment::setBlend(const Frame &frame, Id event, const Json &values) {
    setPipeline(frame, event, values, "Blend");
}
void Experiment::setPipeline(const Frame &frame, Id event, const Json &values, const std::string &label) {
    normalizePipeline(values);
    auto previous = project_;
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", label + " event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "pipeline"}, {"event", event}, {"values", values}}})}});
    project_["cursor"] = history.size();
    try {
        ReplayOptions checked;
        apply(frame, checked);
    } catch (...) {
        project_ = std::move(previous);
        throw;
    }
}
Json Experiment::setter(const Frame &frame, Id event) const {
    auto result = capturedSetter(frame, event);
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "setter" && identifier(op.at("event")) == event)
                result = op.at("values");
    return result;
}
Json Experiment::sampler(const Frame &frame, Id event, const std::string &name, unsigned slot) const {
    const auto stage = samplerStage(name);
    samplerSlot(slot);
    ReplayOptions options;
    apply(frame, options);
    const auto state = samplerState(frame, event, options.samplerSetters);
    auto result = samplerDescriptor(frame, state.stages[stage].samplers[slot]);
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "sampler" && identifier(op.at("event")) == event && op.at("stage") == name &&
                samplerSlot(op.at("slot")) == slot)
                result.update(normalizeSampler(op.at("values")));
    return normalizeSampler(result);
}
void Experiment::setSampler(const Frame &frame, Id event, const std::string &stage, unsigned slot,
                            const Json &values) {
    samplerStage(stage);
    samplerSlot(slot);
    const auto normalized = normalizeSampler(values);
    auto previous = project_;
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "Sampler " + stage + "[" + std::to_string(slot) + "] event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "sampler"},
                                      {"event", event},
                                      {"stage", stage},
                                      {"slot", slot},
                                      {"values", normalized}}})}});
    project_["cursor"] = history.size();
    try {
        ReplayOptions checked;
        apply(frame, checked);
    } catch (...) {
        project_ = std::move(previous);
        throw;
    }
}
Json Experiment::srv(const Frame &frame, Id event, const std::string &name, unsigned slot) const {
    const auto stage = srvStage(name);
    srvSlot(slot);
    Json patch = Json::object();
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "srv_descriptor" && identifier(op.at("event")) == event &&
                op.at("stage") == name && srvSlot(op.at("slot")) == slot)
                patch = mergeSrv(patch, op.at("values"));
    if (patch.empty()) {
        const auto &entry = frame.entry(event);
        if (entry.category != 7 || !isDraw(entry.type))
            throw std::runtime_error("Select a draw or dispatch");
        patch = capturedSrv(frame, frame.state(frame.event(event).state).stages[stage].srv[slot]);
    }
    return effectiveSrv(frame, event, stage, slot, patch);
}
void Experiment::setSrv(const Frame &frame, Id event, const std::string &stage, unsigned slot,
                        const Json &values) {
    srvStage(stage);
    srvSlot(slot);
    const auto normalized = normalizeSrv(values);
    auto previous = project_;
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "SRV " + stage + "[" + std::to_string(slot) + "] event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "srv_descriptor"},
                                      {"event", event},
                                      {"stage", stage},
                                      {"slot", slot},
                                      {"values", normalized}}})}});
    project_["cursor"] = history.size();
    try {
        ReplayOptions checked;
        apply(frame, checked);
    } catch (...) {
        project_ = std::move(previous);
        throw;
    }
}
void Experiment::setSetter(const Frame &frame, Id event, const Json &values) {
    Json normalized;
    if (samplerSetterStage(frame.entry(event).type)) {
        auto binding = validateSamplerSetter(frame, event, values);
        normalized = {{"start_slot", binding.start}, {"samplers", binding.resources}};
    } else {
        auto binding = validatePredicateSetter(frame, event, values);
        normalized = {{"predicate", binding.resource}, {"predicate_value", binding.value}};
    }
    auto previous = project_;
    auto history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "Setter event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "setter"}, {"event", event}, {"values", normalized}}})}});
    project_["history"] = std::move(history);
    project_["cursor"] = project_["history"].size();
    try {
        ReplayOptions checked;
        apply(frame, checked);
    } catch (...) {
        project_ = std::move(previous);
        throw;
    }
}
Json Experiment::clear(const Frame &frame, Id event) const {
    validateWritableCommand(frame, event);
    for (size_t i = revision(); i > 0; --i) {
        const auto &ops = project_["history"][i - 1]["operations"];
        for (auto it = ops.rbegin(); it != ops.rend(); ++it)
            if (it->at("kind") == "clear" && identifier(it->at("event")) == event)
                return clearValues(
                    frame.entry(event).type,
                    patchClear(frame.entry(event).type, frame.payload(event), it->at("values")));
    }
    return clearValues(frame.entry(event).type, frame.payload(event));
}
void Experiment::setClear(const Frame &frame, Id event, const Json &values) {
    validateWritableCommand(frame, event);
    auto type = frame.entry(event).type;
    auto normalized = clearValues(type, patchClear(type, frame.payload(event), values));
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "Clear event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "clear"}, {"event", event}, {"values", normalized}}})}});
    project_["cursor"] = history.size();
}
void Experiment::setUpdateSource(const Frame &frame, Id event, Bytes data) {
    auto layout = updateSourceLayout(frame, event);
    if (data.size() != layout.size)
        throw std::runtime_error("Update source byte count mismatch: expected " +
                                 std::to_string(layout.size));
    Json op{{"kind", "update_source"},
            {"event", event},
            {"asset",
             {{"data", QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))
                           .toBase64()
                           .toStdString()},
              {"sha256", sha256(data)}}}};
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "Update source " + std::to_string(event)}, {"operations", Json::array({op})}});
    project_["cursor"] = history.size();
}
bool Experiment::canUndo() const { return revision() > 0; }
void Experiment::setBuffer(const Frame &frame, Id event, Id resource, uint64_t offset, Bytes data) {
    setBufferPatches(frame, event, resource, {{offset, {data.begin(), data.end()}}},
                     "Buffer " + std::to_string(resource) + " at event " + std::to_string(event));
}
void Experiment::setBufferPatches(const Frame &frame, Id event, Id resource,
                                  const std::vector<BufferPatch> &patches, const std::string &label) {
    if (patches.empty())
        return;
    Json operations = Json::array();
    for (const auto &patch : patches) {
        auto &data = patch.bytes;
        validateBufferPatch(frame, event, resource, patch.offset, data.size());
        operations.push_back(
            {{"kind", "buffer"},
             {"event", event},
             {"resource", resource},
             {"offset", patch.offset},
             {"asset",
              {{"data", QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))
                            .toBase64()
                            .toStdString()},
               {"sha256", sha256(data)}}}});
    }
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back({{"label", label}, {"operations", std::move(operations)}});
    project_["cursor"] = history.size();
}
std::optional<uint32_t> Experiment::initialUavCounter(Id view) const {
    for (size_t i = revision(); i > 0; --i)
        for (auto it = project_["history"][i - 1]["operations"].rbegin();
             it != project_["history"][i - 1]["operations"].rend(); ++it)
            if (it->at("kind") == "initial_uav_counter" && identifier(it->at("view")) == view)
                return uint32_t(identifier(it->at("value")));
    return {};
}
bool Experiment::setUavCounter(const Frame &frame, Id view, uint32_t value, std::optional<Id> event) {
    validateCounterEdit(frame, view, event);
    if (!event && initialUavCounter(view) == value)
        return false;
    Json op{{"kind", event ? "uav_counter" : "initial_uav_counter"}, {"view", view}, {"value", value}};
    if (event)
        op["event"] = *event;
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", std::string(event ? "UAV counter " : "Initial UAV counter ") + std::to_string(view)},
         {"operations", Json::array({op})}});
    project_["cursor"] = history.size();
    return true;
}
bool Experiment::canRedo() const { return revision() < project_["history"].size(); }
bool Experiment::undo() {
    if (!canUndo())
        return false;
    project_["cursor"] = revision() - 1;
    return true;
}
bool Experiment::redo() {
    if (!canRedo())
        return false;
    project_["cursor"] = revision() + 1;
    return true;
}

void Experiment::setShader(const Frame &frame, Id id, Bytes bytecode, const std::string &source,
                           const std::string &entry) {
    inspectResourceShader(frame, id, bytecode);
    Json operation{
        {"kind", "shader"},
        {"resource", id},
        {"asset",
         {{"data", QByteArray(reinterpret_cast<const char *>(bytecode.data()), qsizetype(bytecode.size()))
                       .toBase64()
                       .toStdString()},
          {"sha256", sha256(bytecode)}}},
        {"source_language", "hlsl"},
        {"source_text", source},
        {"source_entry", entry}};
    auto &history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back({{"label", "Shader " + std::to_string(id)}, {"operations", Json::array({operation})}});
    project_["cursor"] = history.size();
}
std::vector<uint8_t> Experiment::shaderBytes(const Frame &frame, Id id) const {
    for (size_t i = revision(); i > 0; --i) {
        auto &operations = project_["history"][i - 1]["operations"];
        for (auto it = operations.rbegin(); it != operations.rend(); ++it)
            if (it->at("kind") == "shader" && identifier(it->at("resource")) == id)
                return asset(it->at("asset"));
    }
    auto bytes = frame.shader(frame.resource(id).data);
    return {bytes.begin(), bytes.end()};
}
Json Experiment::shaderSource(Id id) const {
    for (size_t i = revision(); i > 0; --i) {
        auto &operations = project_["history"][i - 1]["operations"];
        for (auto it = operations.rbegin(); it != operations.rend(); ++it)
            if (it->at("kind") == "shader" && identifier(it->at("resource")) == id)
                return *it;
    }
    return Json::object();
}
} // namespace flora
