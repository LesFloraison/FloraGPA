#pragma once
#include "core/Frame.h"
#include <QString>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
// Validates the Python collector's input domain and supplies its defaults.
nlohmann::json scheduledMetricRequest(const nlohmann::json &request);
nlohmann::json collectMetricCatalog(const QString &directory, const QString &bridge = {});
nlohmann::json collectScheduledMetrics(const Frame &frame, const QString &directory,
                                       const nlohmann::json &request, const QString &experiment = {},
                                       const QString &bridge = {}, const std::function<bool()> &cancel = {},
                                       const std::function<void(const nlohmann::json &)> &progress = {});
} // namespace flora
