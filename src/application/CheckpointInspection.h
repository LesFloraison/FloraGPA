#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>

namespace flora {
struct CheckpointInspectionOptions {
    std::string stage = "gs";
    // Original disassembler instruction number; absent with trace=false lists the catalog.
    std::optional<uint32_t> instruction;
    bool trace = false;
    std::optional<uint32_t> hullPhase;
    nlohmann::json inputSelector = nullptr;
    uint64_t maxBytes = 256ull * 1024 * 1024;
};
struct CheckpointInspection {
    nlohmann::json report;
    std::vector<uint8_t> shader, bytes;
    std::string assembly;
};
// Replay must be complete at the before-event boundary. Diagnostic mutations are isolated.
CheckpointInspection inspectCheckpoint(Replay &replay, Id event,
                                       const CheckpointInspectionOptions &options = {});
CheckpointInspection checkpointCatalog(Bytes shader, Id resource, const nlohmann::json &event,
                                       const CheckpointInspectionOptions &options);
nlohmann::json checkpointRegisters(Bytes data, const nlohmann::json &metadata, uint32_t record);
nlohmann::json checkpointHeaders(Bytes data, const nlohmann::json &metadata);
void completeCheckpointInspection(CheckpointInspection &inspection, nlohmann::json metadata,
                                  std::vector<uint8_t> records);
void exportCheckpoint(const CheckpointInspection &inspection, const std::filesystem::path &directory);
} // namespace flora
