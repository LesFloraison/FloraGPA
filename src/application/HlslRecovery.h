#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
// Reconstructs analysis HLSL, never claims to recover the original source.
// Unsupported declarations/instructions are errors, not omitted statements.
std::string recoverHlsl(Bytes bytecode);
struct RecoveredHlsl {
    std::string source;
    std::vector<uint8_t> recompiled;
    nlohmann::json report;
};
// Saved source is accepted only if recompilation reproduces every byte.
RecoveredHlsl reconstructHlsl(Bytes bytecode, const nlohmann::json &savedSource = nullptr);
} // namespace flora
