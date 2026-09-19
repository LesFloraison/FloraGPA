#pragma once
#include "core/Contexts.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json contextJson(const ContextDescription &context);
nlohmann::json inspectContexts(const Frame &frame);
nlohmann::json inspectCommandList(const Frame &frame, Id id);
nlohmann::json inspectCommandLists(const Frame &frame);
} // namespace flora
