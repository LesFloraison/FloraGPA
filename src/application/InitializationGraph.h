#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
// Pinned original collector metadata. Neither function initializes GPU objects.
nlohmann::json inspectInitializationNode(const Frame &frame, Id id);
nlohmann::json inspectInitializationGraph(const Frame &frame);
} // namespace flora
