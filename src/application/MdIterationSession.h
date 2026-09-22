#pragma once
#include "Experiment.h"
#include <QStringList>
namespace flora {
nlohmann::json scheduledUiSettings(const nlohmann::json &ui);
nlohmann::json scheduledUiDocument(nlohmann::json ui, const nlohmann::json &settings);
nlohmann::json prepareScheduledRequest(const Frame &frame, const Experiment &experiment,
                                       const nlohmann::json &catalog, const nlohmann::json &request);
QStringList scheduledWorkerArguments(const QString &capture, const QString &jobDirectory,
                                     const QString &bridge, const nlohmann::json &prepared);
nlohmann::json acceptScheduledResult(const QString &directory, const nlohmann::json &prepared,
                                     const QString &experimentFile, const QString &experimentKey);
void exportScheduledResult(const QString &directory, const QString &destination);
} // namespace flora
