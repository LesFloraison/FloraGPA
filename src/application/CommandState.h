#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
#include <optional>
namespace flora {
// Captured state only. Unknown observations must not become replay defaults.
nlohmann::json inspectCommandState(const Frame &frame, Id event, bool after = false,
                                   const nlohmann::json *commands = nullptr);
bool isStateResourceField(const std::string &key);
const std::vector<std::string> &commandStateFieldNames();
std::optional<Id> stateCommandContext(const Frame &frame, const nlohmann::json &command);
// Development validation of predictions at every qualified captured snapshot.
nlohmann::json auditCommandState(const Frame &frame);
} // namespace flora
