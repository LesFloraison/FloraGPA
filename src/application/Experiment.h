#pragma once
#include "core/Frame.h"
#include "replay/Replay.h"
#include <QString>
#include <nlohmann/json.hpp>

namespace flora {
class Experiment {
    nlohmann::json project_;

  public:
    explicit Experiment(const Frame &frame);
    void load(const QString &path, const Frame &frame);
    void save(const QString &path) const;
    void apply(const Frame &frame, ReplayOptions &options) const;
    void setEnabled(const Frame &frame, Id event, bool enabled);
    void setClear(const Frame &frame, Id event, const nlohmann::json &values);
    nlohmann::json clear(const Frame &frame, Id event) const;
    void setUpdateSource(const Frame &frame, Id event, Bytes data);
    void setBuffer(const Frame &frame, Id event, Id resource, uint64_t offset, Bytes data);
    void setShader(const Frame &frame, Id id, Bytes bytecode, const std::string &source,
                   const std::string &entry);
    std::vector<uint8_t> shaderBytes(const Frame &frame, Id id) const;
    nlohmann::json shaderSource(Id id) const;
    bool enabled(Id event) const;
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    size_t revision() const;
    const nlohmann::json &document() const { return project_; }
};
} // namespace flora
