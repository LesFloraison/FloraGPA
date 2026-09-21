#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct QuadStrip {
    std::vector<uint32_t> indices;
    nlohmann::json metadata;
};
QuadStrip expandQuadStrip(std::span<const uint32_t> values, std::optional<uint32_t> cut = {},
                          uint32_t verticesPerPrimitive = 3);
class QuadSerialState;
// Construct and submit inside the caller's bound/private-output inspection scope.
// Restores IA bindings locally; the caller restores shaders and the full pipeline.
class QuadSerial {
    std::unique_ptr<QuadSerialState> state_;

  public:
    QuadSerial(Replay &replay, const Event &event, const State &state);
    ~QuadSerial();
    uint64_t submit();
    nlohmann::json metadata() const;
};
} // namespace flora
