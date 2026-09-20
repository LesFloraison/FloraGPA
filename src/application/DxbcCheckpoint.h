#pragma once
#include "DxbcOutputLog.h"
#include <optional>

namespace flora {
struct CheckpointOptions {
    std::string stage = "gs";
    uint32_t slot = 7;
    uint32_t capacity = 1024;
    // Absent token selects the complete original invocation trace.
    std::optional<uint32_t> token;
    std::optional<uint32_t> hullPhase;
    nlohmann::json inputSelector = nullptr;
};
OutputLogShader instrumentCheckpoint(Bytes original, const CheckpointOptions &options);
} // namespace flora
