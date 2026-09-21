#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
std::string shaderProjectPathKey(const nlohmann::json &name);
nlohmann::json validateShaderProject(nlohmann::json project);
std::string shaderProjectDigest(const nlohmann::json &project);
nlohmann::json parseShaderProjectDefines(const std::string &text);
nlohmann::json shaderProjectFromSources(const nlohmann::json &report, const std::string &profile);
struct ShaderProjectCompilation {
    std::vector<uint8_t> bytecode;
    nlohmann::json report;
};
ShaderProjectCompilation compileShaderProject(const nlohmann::json &project);
nlohmann::json verifyShaderProject(Bytes bytecode, const nlohmann::json &project);
} // namespace flora
