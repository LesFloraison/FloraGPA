#pragma once
#include "Frame.h"
#include <optional>
#include <set>
namespace flora {
inline constexpr std::array<uint16_t, 6> srvSetterTypes{0x34f7, 0x3519, 0x351d, 0x34fd, 0x34e6, 0x3521};
std::optional<unsigned> srvSetterStage(uint16_t type);
struct SrvBinding {
    uint32_t start{};
    std::vector<Id> views;
};
struct SrvCommand {
    Id context{};
    SrvBinding binding;
};
SrvCommand readSrvCommand(Bytes bytes);
void validateSrvResource(const Frame &frame, Id view);
bool isSrvOutputCommand(uint16_t type);
// RTV[8], OM UAV[64], CS UAV[64], SO[4], DSV. nullopt is unobserved, not null binding.
using SrvOutputs = std::array<std::optional<Id>, 141>;
SrvOutputs srvOutputs(const State &state);
class SrvHazards {
    struct Span {
        Id resource{};
        std::optional<std::set<std::pair<uint32_t, uint32_t>>> subresources;
        bool ambiguous{};
    };
    const Frame &frame_;
    std::map<Id, Span> spans_;
    std::optional<Span> span(Id id);

  public:
    explicit SrvHazards(const Frame &frame) : frame_(frame) {}
    // General output/output overlap keeps the captured state's conservative rules.
    std::optional<bool> overlap(std::optional<Id> left, std::optional<Id> right, bool shaderResource = false);
    std::optional<Id> effective(Id view, const SrvOutputs &outputs, bool unknown = false);
};
struct SrvObservation {
    SrvOutputs outputs{};
    std::array<std::array<std::optional<Id>, 128>, 6> srvs{};
};
// Incremental original-capture history; independent of experiment overrides.
class SrvHistory {
    const Frame &frame_;
    Id context_;
    std::map<Id, Entry>::const_iterator next_;
    SrvHazards hazards_;
    SrvObservation state_;
    void apply(const Entry &entry);

  public:
    SrvHistory(const Frame &frame, Id context);
    const SrvObservation &advance(Id event, bool after = false);
};
class SrvBindings {
    std::array<std::map<unsigned, Id>, 6> active_;

  public:
    void clear();
    bool active() const;
    bool active(unsigned stage) const { return !active_.at(stage).empty(); }
    const auto &overrides(unsigned stage) const { return active_.at(stage); }
    void transition(unsigned stage, const SrvBinding &original, const SrvBinding *edited,
                    const SrvObservation &previous);
    void hazards(SrvHazards &checker, const SrvOutputs &outputs);
    void apply(State &state) const;
    void unbind(unsigned stage, unsigned slot);
};
State effectiveSrvBindings(const Frame &frame, Id event, State state, const std::map<Id, SrvBinding> &edits);
} // namespace flora
