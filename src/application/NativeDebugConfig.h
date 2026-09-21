#pragma once
#include "SourceTrace.h"
#include <filesystem>

namespace flora {
struct NativeDebugSettings {
    SourceTrace source;
    std::set<uint64_t> instructions;
    std::vector<DebugExpression> watches;
};
nlohmann::json nativeDebugIdentity(const nlohmann::json &result);
nlohmann::json exportNativeDebugConfig(const nlohmann::json &result, const NativeDebugSettings &settings);
// Build a complete replacement before the caller changes any live debugger state.
NativeDebugSettings prepareNativeDebugConfig(const nlohmann::json &result, const nlohmann::json &rows,
                                             SourceTrace::ValueLoader loader, const nlohmann::json &config);
nlohmann::json readNativeDebugConfig(const std::filesystem::path &path);
void writeNativeDebugConfig(const std::filesystem::path &path, const nlohmann::json &config);
} // namespace flora
