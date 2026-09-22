#pragma once
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
// Validate saved scheduled artifacts without opening a frame, device or driver.
// Returns the checked publisher data. The supplied profile is never modified.
nlohmann::json loadScheduledMetricResult(const QString &folder, const nlohmann::json &profile);
} // namespace flora
