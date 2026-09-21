#pragma once
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json prepareRdcJob(const QString &capture, const std::string &action, const QString &output,
                             const QString &library,
                             const nlohmann::json &options = nlohmann::json::object());
QString runRdcAnalysis(const nlohmann::json &job, const QString &worker, double timeoutSeconds);
} // namespace flora
