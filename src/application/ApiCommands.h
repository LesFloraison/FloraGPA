#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
#include <optional>

namespace flora {
// Read-only wire inspection. A decoded record does not imply replay support.
nlohmann::json inspectCommand(const Frame &frame, Id id);
nlohmann::json inspectCommands(const Frame &frame, const CancelCheck &cancelled = {});
bool commandMatches(const nlohmann::json &command, const std::string &text = "",
                    std::optional<Id> resource = {});
nlohmann::json commandResourceSelection(const Frame &frame, const nlohmann::json &reference,
                                        const nlohmann::json &command = nullptr);
void exportCommands(const Frame &frame, const std::filesystem::path &directory, const std::string &text = "",
                    std::optional<Id> resource = {}, bool gpuOnly = false, const CancelCheck &cancelled = {});
} // namespace flora
