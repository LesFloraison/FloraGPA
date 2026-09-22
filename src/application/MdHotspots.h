#pragma once
#include "MdProfile.h"
namespace flora {
nlohmann::json normalizeMetricGroups(const nlohmann::json &groups, const nlohmann::json &events);
nlohmann::json aggregateMetricProfiles(const nlohmann::json &profile, const nlohmann::json &weights,
                                       const nlohmann::json &groups = nullptr);
nlohmann::json aggregatePublisherMetricProfiles(const nlohmann::json &profile, const nlohmann::json &weights,
                                                const nlohmann::json &publisher,
                                                const nlohmann::json &weightPublisher,
                                                const nlohmann::json &groups = nullptr);
nlohmann::json loadPublisherMetricAggregates(const QString &directory, const nlohmann::json &result);
std::string metricGroupsCsv(const nlohmann::json &result);
nlohmann::json collectMetricGroups(const Frame &frame, const QString &directory,
                                   const nlohmann::json &request, const QString &experiment = {},
                                   const QString &bridge = {}, const std::function<bool()> &cancel = {},
                                   const std::function<void(const nlohmann::json &)> &progress = {});
} // namespace flora
