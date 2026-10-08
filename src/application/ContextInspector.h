#pragma once
#include "core/Contexts.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json contextJson(const ContextDescription &context, const CancelCheck &cancelled = {});
nlohmann::json inspectContexts(const Frame &frame, const CancelCheck &cancelled = {});
nlohmann::json inspectCommandList(const Frame &frame, Id id, const CancelCheck &cancelled = {});
nlohmann::json inspectCommandLists(const Frame &frame, const CancelCheck &cancelled = {});
} // namespace flora
