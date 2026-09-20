#include "Experiment.h"
#include "CommandEdits.h"
#include "DepthStencilEdits.h"
#include "SetterEdits.h"
#include "ShaderInspector.h"
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
    options.depthStencilEdits.clear();
    std::map<Id, Json> pipeline;
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
                auto values = normalizeDepthStencil(op.at("values"));
                mergeDepthStencil(pipeline[event], values);
                options.depthStencilEdits[event] = depthStencilEdit(frame, event, pipeline[event]);
            } else if (kind == "setter") {
                const auto id = identifier(op.at("event"));
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
                mergeDepthStencil(result, normalizeDepthStencil(op.at("values")));
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
Json Experiment::setter(const Frame &frame, Id event) const {
    auto result = capturedSetter(frame, event);
    for (size_t i = 0; i < revision(); ++i)
        for (const auto &op : project_["history"][i]["operations"])
            if (op.at("kind") == "setter" && identifier(op.at("event")) == event)
                result = op.at("values");
    return result;
}
void Experiment::setSetter(const Frame &frame, Id event, const Json &values) {
    auto binding = validatePredicateSetter(frame, event, values);
    Json normalized{{"predicate", binding.resource}, {"predicate_value", binding.value}};
    auto history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", "Setter event " + std::to_string(event)},
         {"operations", Json::array({{{"kind", "setter"}, {"event", event}, {"values", normalized}}})}});
    project_["history"] = std::move(history);
    project_["cursor"] = project_["history"].size();
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
