#pragma once
#include <nlohmann/json.hpp>
#include <QFile>
#include <QSaveFile>
#include <charconv>

namespace flora {
enum class ReplayBackend { Dx11, Dx12 };
struct ReplayRequestIdentity {
    std::string session;
    ReplayBackend backend = ReplayBackend::Dx11;
    uint64_t request{}, experimentRevision{};
    bool operator==(const ReplayRequestIdentity &) const = default;
};
inline nlohmann::json replayIdentityJson(const ReplayRequestIdentity &id) {
    return {{"schema", 1}, {"backend", id.backend == ReplayBackend::Dx12 ? "dx12" : "dx11"},
        {"session", id.session}, {"request", std::to_string(id.request)},
        {"experiment_revision", std::to_string(id.experimentRevision)}};
}
inline ReplayRequestIdentity parseReplayIdentity(const nlohmann::json &json) {
    if (!json.is_object() || json.size() != 5 || json.at("schema") != 1)
        throw std::runtime_error("Unsupported replay request identity schema");
    ReplayRequestIdentity id;
    id.session = json.at("session").get<std::string>();
    if (id.session.empty() || id.session.size() > 128) throw std::runtime_error("Invalid replay session identity");
    const auto backend = json.at("backend").get<std::string>();
    if (backend != "dx11" && backend != "dx12") throw std::runtime_error("Unknown replay backend identity");
    id.backend = backend == "dx12" ? ReplayBackend::Dx12 : ReplayBackend::Dx11;
    const auto number = [&](const char *key) {
        // Numeric JSON is deliberately rejected even if currently small enough:
        // this boundary must never rely on floating-point identity conversion.
        const auto text = json.at(key).get<std::string>(); uint64_t value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || std::to_string(value) != text)
            throw std::runtime_error("Invalid decimal replay identity");
        return value;
    };
    id.request = number("request"); id.experimentRevision = number("experiment_revision");
    if (!id.request) throw std::runtime_error("Replay request identity cannot be zero");
    return id;
}
inline ReplayRequestIdentity readReplayIdentity(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > 4096)
        throw std::runtime_error("Replay identity is missing or exceeds its size bound");
    const auto bytes = file.readAll();
    return parseReplayIdentity(nlohmann::json::parse(bytes.begin(), bytes.end()));
}
inline void writeReplayIdentity(const QString &path, const ReplayRequestIdentity &id) {
    const auto json = replayIdentityJson(id); (void)parseReplayIdentity(json);
    const auto bytes = json.dump(); QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot publish replay request identity");
}
inline void requireReplayIdentity(const QString &path, const ReplayRequestIdentity &expected) {
    if (readReplayIdentity(path) != expected) throw std::runtime_error("Worker result belongs to a different replay request");
}
}
