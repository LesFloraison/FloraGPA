#pragma once
#include "core/Frame.h"
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
// Offline only. No D3D device or replay is created. Cancellation is checked between records.
nlohmann::json validateFrame(const std::filesystem::path &path, const std::function<bool()> &cancelled = {});
} // namespace flora
