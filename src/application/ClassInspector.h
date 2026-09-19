#pragma once
#include "core/ClassLinkage.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json inspectClass(const Frame &frame, Id id);
} // namespace flora
