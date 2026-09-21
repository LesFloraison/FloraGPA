#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct QuadOptions {
    std::string depthMode = "prepared", target = "auto";
    std::optional<uint32_t> layer;
};
struct QuadCounterOptions {
    bool depthTest = true, verifyStorage = true, serial = false, prepareDepth = false;
    std::string target = "auto";
    std::optional<uint32_t> layer;
};
struct QuadResult {
    nlohmann::json report, counterReport;
    // Little-endian uint32 storage: locks, counts, live, histogram, reference.
    std::array<std::vector<uint8_t>, 5> storage;
    Image preview;
};
// Caller owns the prepared event scope; diagnostic work preserves original
// resources. This lower-level API also retains the original nonserial mode.
QuadResult diagnoseQuadBound(Replay &replay, const Event &event, const State &state,
                             const QuadCounterOptions &options = {});
// Owns an inclusive prefix replay. The normal command submits the original
// event exactly once after isolated diagnostics, including experiment edits.
QuadResult captureQuad(Replay &replay, Id event, const QuadOptions &options = {});
void exportQuad(const QuadResult &result, const std::filesystem::path &directory);
} // namespace flora
