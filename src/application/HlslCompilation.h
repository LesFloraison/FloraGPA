#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
struct HlslCompilation {
    std::vector<uint8_t> bytecode;
    std::string diagnostics;
    nlohmann::json options;
};
nlohmann::json hlslCompilationOptions(const std::string &source, const std::string &optimization = "auto");
HlslCompilation compileHlsl(const std::string &source, const std::string &profile,
                            const std::string &entry = "main", const std::string &name = "edited.hlsl",
                            const std::string &optimization = "auto");
} // namespace flora
