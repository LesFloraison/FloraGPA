#pragma once
#include "core/Frame.h"
#include <QString>
#include <nlohmann/json.hpp>

namespace flora {
// These identifiers and result schemas belong to FloraGPA, independent of external replay ABIs.
inline constexpr unsigned nativeAnalysisSchema = 1;
inline constexpr const char *nativeAnalysisBackend = "dx11-native/17";
struct NativeAnalysisRequest {
    std::string action;
    nlohmann::json options;
};
NativeAnalysisRequest nativeAnalysisRequest(const std::string &action, const nlohmann::json &options);
std::string analysisFileDigest(const QString &path, const CancelCheck &cancelled = {});
std::string analysisIdentity(const nlohmann::json &context);
// Publishes result.json last. Partial exports are never a completed analysis result.
nlohmann::json executeNativeAnalysis(const nlohmann::json &job, const CancelCheck &cancelled = {});
void validateNativeAnalysisResult(const nlohmann::json &job, const nlohmann::json &result);
} // namespace flora
