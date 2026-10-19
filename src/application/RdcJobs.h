#pragma once
#include <QString>
#include <cstdint>
#include <nlohmann/json.hpp>
namespace flora {
struct RdcBufferRange { uint64_t offset{}, length{}; };
RdcBufferRange rdcBufferRange(const nlohmann::json &job);
nlohmann::json prepareRdcJob(const QString &capture, const std::string &action, const QString &output,
                             const QString &library,
                             const nlohmann::json &options = nlohmann::json::object());
QString runRdcAnalysis(const nlohmann::json &job, const QString &worker, double timeoutSeconds);
} // namespace flora
