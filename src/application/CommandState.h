#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
// Captured state only. Unknown observations must not become replay defaults.
nlohmann::json inspectCommandState(const Frame &frame, Id event, bool after = false,
                                   const nlohmann::json *commands = nullptr);
bool isStateResourceField(const std::string &key);
// Development validation of predictions at every qualified captured snapshot.
nlohmann::json auditCommandState(const Frame &frame);
} // namespace flora
