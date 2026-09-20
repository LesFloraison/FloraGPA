#pragma once
#include "FrameOutput.h"
namespace flora {
struct ReplayUiState {
    std::string target = "auto", channel = "rgba", low = "0", high = "1";
    std::optional<uint32_t> layer, sample;
    bool warp = false;
    Id event = 0;
    int boundary = 0;
    std::string geometryStage = "final", geometryTable = "expanded_vertices", geometryInstance;
    uint32_t geometryStream = 0;
};
ReplayUiState replayUiState(const Frame &frame, const nlohmann::json &ui, Id currentEvent = 0);
nlohmann::json replayUiDocument(const Frame &frame, const ReplayUiState &state,
                                const nlohmann::json &previous = nlohmann::json::object());
} // namespace flora
