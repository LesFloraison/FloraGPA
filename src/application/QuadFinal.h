#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
class QuadFinalState;
// Caller owns the event's bound input/private-output scope and restores the
// complete pipeline after helper submission. Capture uses private output copies.
class QuadFinal {
    std::unique_ptr<QuadFinalState> state_;

  public:
    QuadFinal(Replay &replay, const Event &event, const State &state);
    ~QuadFinal();
    uint64_t submit();
    nlohmann::json metadata() const;
    const nlohmann::json &outputSignatures() const;
};
} // namespace flora
