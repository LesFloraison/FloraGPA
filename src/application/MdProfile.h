#pragma once
#include "core/Frame.h"
#include <QString>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json uniformMetricRequest(const nlohmann::json &request);
nlohmann::json collectUniformMetrics(const Frame &frame, const QString &directory,
                                     const nlohmann::json &request, const QString &experiment = {},
                                     const QString &bridge = {}, const std::function<bool()> &cancel = {},
                                     const std::function<void(const nlohmann::json &)> &progress = {});
} // namespace flora
