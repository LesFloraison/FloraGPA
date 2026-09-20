#pragma once
#include "Frame.h"
#include <optional>
namespace flora {
inline constexpr std::array<uint16_t, 6> samplerSetterTypes{0x34f8, 0x351b, 0x351f, 0x34fe, 0x34e8, 0x3524};
inline constexpr std::array<const char *, 6> samplerStageNames{"vs", "hs", "ds", "gs", "ps", "cs"};
std::optional<unsigned> samplerSetterStage(uint16_t type);
struct SamplerBinding {
    uint32_t start{};
    std::vector<Id> resources;
};
struct SamplerCommand {
    Id context{};
    SamplerBinding binding;
};
SamplerCommand readSamplerCommand(Bytes bytes);
void validateSamplerResource(const Frame &frame, Id resource);
class SamplerBindings {
    std::array<std::array<std::optional<Id>, 16>, 6> known_{};
    std::array<std::map<unsigned, Id>, 6> active_;

  public:
    void clear();
    void observe(const State &state);
    void apply(State &state) const;
    void transition(unsigned stage, const SamplerBinding &original, const SamplerBinding *edited);
    void observeCommand(unsigned stage, const SamplerBinding &original);
    bool active(unsigned stage) const { return !active_.at(stage).empty(); }
    void observePrefix(const Frame &frame, Id event);
};
State effectiveSamplerBindings(const Frame &frame, Id event, State state,
                               const std::map<Id, SamplerBinding> &edits);
} // namespace flora
