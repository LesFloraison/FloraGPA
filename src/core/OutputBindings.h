#pragma once
#include "SrvBindings.h"
#include "StreamOutput.h"
namespace flora {
inline constexpr uint32_t keepOutput = UINT32_MAX;
struct OutputCommand {
    Id context{}, dsv{};
    uint32_t rtvCount{}, start{}, uavCount{};
    std::optional<std::vector<Id>> rtvs, uavs;
    std::optional<std::vector<uint32_t>> initialCounts;
};
bool isOutputCommand(uint16_t type);
OutputCommand readOutputCommand(uint16_t type, Bytes bytes);
// Keys use the same RTV / OM UAV / CS UAV / SO / DSV ordering as SrvOutputs.
std::map<unsigned, Id> outputChanges(uint16_t type, const OutputCommand &command);
void validateOutputRange(uint16_t type, const OutputCommand &command, uint32_t limit = 64);
void validateOutputTargets(const Frame &frame, const SrvOutputs &outputs);

class MissingOutputResource : public std::runtime_error {
  public:
    std::set<Id> resources;
    explicit MissingOutputResource(std::set<Id> ids)
        : std::runtime_error("Captured output binding references missing view records"),
          resources(std::move(ids)) {}
};
using BindingValues = std::map<std::string, std::optional<Id>>;
struct BindingGap {
    std::set<std::string> affected, edited;
};
// Original and edited histories stay separate: an original null snapshot must
// not erase an input that only the original output command would have evicted.
class OutputBindingModel {
    const Frame &frame_;
    Id context_;
    SrvHazards hazards_;
    BindingValues original_, changed_;
    std::set<std::string> protected_, dirty_, uncertain_;
    bool possible(std::optional<Id> id, std::optional<Id> output, const std::string &role,
                  const std::string &key);
    std::optional<Id> iaEffective(Id id, const SrvOutputs &outputs);
    void inputs(BindingValues &values);
    void output(BindingValues &values, const BindingValues &changes);
    std::set<std::string> apply(BindingValues &values, uint16_t type, Bytes bytes);
    void differences();

  public:
    OutputBindingModel(const Frame &frame, Id context);
    static bool models(uint16_t type);
    static BindingValues snapshot(const State &state);
    static State overlay(const BindingValues &values, State state);
    static void validateArguments(const Frame &frame, uint16_t type, Bytes bytes);
    const BindingValues &original() const { return original_; }
    const BindingValues &changed() const { return changed_; }
    const auto &dirty() const { return dirty_; }
    void step(const Entry &entry, std::optional<Bytes> replacement = {});
    BindingGap gap(const Entry &entry);
    void anchor(const State &state);
    State overlay(State state) const;
};
struct OutputHistoryGap {
    std::set<Id> missingViews;
    BindingGap fields;
};
struct OutputBindingState {
    State state;
    std::array<bool, 4> retainedSo{};
};
// Immutable replacement payloads and one forward traversal; earlier requests
// read cached differences without moving or replaying the live model backwards.
class OutputBindingHistory {
    const Frame &frame_;
    std::map<Id, std::vector<uint8_t>> replacements_;
    std::map<Id, Entry>::const_iterator next_;
    std::map<Id, std::unique_ptr<OutputBindingModel>> models_;
    std::map<Id, BindingValues> deltas_;
    std::map<Id, OutputHistoryGap> gaps_;

  public:
    OutputBindingHistory(const Frame &frame, std::map<Id, std::vector<uint8_t>> replacements);
    void advance(Id event);
    const BindingValues &delta(Id event);
    OutputBindingState state(Id event, State captured);
    const auto &gaps() const { return gaps_; }
};
} // namespace flora
