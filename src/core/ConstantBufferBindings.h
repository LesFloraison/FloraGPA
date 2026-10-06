#pragma once
#include "Frame.h"
#include <set>
namespace flora {
inline constexpr std::array<uint16_t, 6> constantBufferShimTypes{0x34e5, 0x351c, 0x3520,
                                                                 0x34f4, 0x34ee, 0x3525};
std::optional<unsigned> constantBufferStage(uint16_t type);
struct ConstantBufferBinding {
    uint16_t type{};
    Id context{};
    uint32_t start{};
    std::vector<Id> buffers;
    std::optional<std::vector<uint32_t>> first, counts;
};
struct ConstantBufferRange {
    Id buffer{};
    uint32_t first{}, count{};
    bool window{};
    bool operator==(const ConstantBufferRange &) const = default;
};
ConstantBufferBinding readConstantBufferSetter(uint16_t type, Bytes bytes);
void validateConstantBufferBinding(const Frame &frame, const ConstantBufferBinding &binding);
struct UnusedConstantBufferLifetime {
    Id event{}, closingEvent{};
    unsigned stage{};
    std::vector<Id> resources;
};
UnusedConstantBufferLifetime
proveUnusedConstantBufferLifetime(const Frame &frame, Id event, const std::set<Id> &disabled = {},
                                  const std::map<Id, std::vector<uint8_t>> &payloads = {},
                                  const std::map<Id, ConstantBufferBinding> &edits = {}, Id until = 0,
                                  bool before = false, const CancelCheck &cancelled = {});
ConstantBufferRange constantBufferRow(const ConstantBufferBinding &binding, size_t row);
bool displacedConstantBuffers(const ConstantBufferBinding &original, const ConstantBufferBinding &edited);
using ConstantBufferObservations = std::array<std::array<std::optional<ConstantBufferRange>, 14>, 6>;
class ConstantBufferHistory {
    const Frame &frame_;
    std::map<Id, Entry>::const_iterator next_;
    ConstantBufferObservations known_{};

  public:
    explicit ConstantBufferHistory(const Frame &frame) : frame_(frame), next_(frame.entries().begin()) {}
    const ConstantBufferObservations &advance(Id event);
};
class ConstantBufferBindings {
    std::array<std::map<unsigned, ConstantBufferRange>, 6> active_;

  public:
    void clear();
    bool active(unsigned stage) const { return !active_.at(stage).empty(); }
    void transition(const ConstantBufferBinding &original, const ConstantBufferBinding *edited,
                    const ConstantBufferObservations &previous = {});
    void apply(State &state) const;
};
State effectiveConstantBufferBindings(const Frame &frame, Id event, State state,
                                      const std::map<Id, ConstantBufferBinding> &edits);
} // namespace flora
