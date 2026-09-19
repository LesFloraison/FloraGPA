#pragma once
#include "replay/Replay.h"
#include <QString>
#include <nlohmann/json.hpp>

namespace flora {
nlohmann::json vertexValue(Bytes data, uint64_t offset, uint32_t format);
nlohmann::json meshPrimitives(const std::vector<int64_t> &stream, uint32_t topology);
// Replay must be positioned immediately before the selected draw.
nlohmann::json inspectGeometry(const Frame &frame, Replay &replay, Id event);
void exportGeometry(const nlohmann::json &geometry, const QString &directory);
} // namespace flora
