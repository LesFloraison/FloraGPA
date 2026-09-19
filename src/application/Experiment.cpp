#include "Experiment.h"
#include "ShaderInspector.h"
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
    options.disabled.clear();
    options.shaders.clear();
    options.textures.clear();
    for (size_t i = 0; i < revision(); ++i) {
        auto &operations = project_.at("history").at(i).at("operations");
        if (!operations.is_array())
            throw std::runtime_error("Invalid experiment operation list");
        for (auto &op : operations) {
            auto kind = op.at("kind").get<std::string>();
            if (kind == "enabled") {
                auto id = identifier(op.at("event"));
                auto &e = frame.entry(id);
                static const std::set<uint16_t> writable{0x31, 0x32, 0x33,  0x34,  0x3e, 0x3f,
                                                         0x40, 0x42, 0x245, 0x246, 0x247};
                if (e.category != 7 || (!isDraw(e.type) && !writable.contains(e.type)) ||
                    !op.at("value").is_boolean())
                    throw std::runtime_error("Unsupported event enable operation");
                if (op.at("value").get<bool>())
                    options.disabled.erase(id);
                else
                    options.disabled.insert(id);
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
    if (!isDraw(frame.entry(event).type))
        throw std::runtime_error("Select a draw or dispatch event");
    auto history = project_["history"];
    history.erase(history.begin() + ptrdiff_t(revision()), history.end());
    history.push_back(
        {{"label", std::string(enabled ? "Enable event " : "Disable event ") + std::to_string(event)},
         {"operations", Json::array({{{"kind", "enabled"}, {"event", event}, {"value", enabled}}})}});
    project_["history"] = std::move(history);
    project_["cursor"] = project_["history"].size();
}
bool Experiment::canUndo() const { return revision() > 0; }
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
    auto resource = frame.resource(id);
    static const std::map<int, std::string> stages{{0x90, "vs"}, {0x91, "gs"}, {0x92, "ps"},
                                                   {0x93, "cs"}, {0x94, "ds"}, {0x95, "hs"}};
    auto metadata = inspectShader(bytecode);
    auto found = stages.find(resource.type);
    if (found == stages.end() || metadata["stage"] != found->second)
        throw std::runtime_error("Replacement shader stage mismatch");
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
