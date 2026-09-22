#pragma once
#include "Experiment.h"
#include <QStringList>
namespace flora {
nlohmann::json uniformUiSettings(const nlohmann::json &ui, bool requested);
nlohmann::json uniformUiDocument(nlohmann::json ui, const nlohmann::json &sets,
                                 const nlohmann::json &requested);
nlohmann::json prepareUniformRequest(const Frame &frame, const Experiment &experiment,
                                     const nlohmann::json &catalog, const nlohmann::json &settings,
                                     bool requested, Id currentEvent);
QStringList uniformWorkerArguments(const QString &capture, const QString &directory, const QString &bridge,
                                   const nlohmann::json &prepared);
nlohmann::json acceptUniformResult(const QString &directory, const nlohmann::json &prepared,
                                   const QString &experimentFile, const QString &experimentKey);
} // namespace flora
