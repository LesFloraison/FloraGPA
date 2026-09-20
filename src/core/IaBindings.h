#pragma once
#include "SrvBindings.h"
namespace flora {
bool isIaSetter(uint16_t type);
struct IaBinding {
    uint16_t type{};
    Id context{}, resource{};
    uint32_t format{}, offset{}, start{};
    std::vector<Id> buffers;
    std::vector<uint32_t> strides, offsets;
};
IaBinding readIaSetter(uint16_t type, Bytes bytes);
std::vector<uint8_t> encodeIaSetter(const IaBinding &binding, Bytes prefix);
void validateIaBinding(const Frame &frame, const IaBinding &binding);
std::optional<Id> effectiveIaBuffer(const Frame &frame, Id resource, const SrvOutputs &outputs,
                                    bool unknown = false);
using IaTuple = std::array<Id, 3>; // resource, stride/format, offset
struct IaObservation {
    SrvOutputs outputs;
    std::array<std::array<std::optional<Id>, 3>, 32> vertices{};
};
class IaHistory {
    const Frame &frame_;
    Id context_;
    std::map<Id, Entry>::const_iterator next_;
    SrvHistory outputs_;
    IaObservation state_;

  public:
    IaHistory(const Frame &frame, Id context);
    const IaObservation &advance(Id event, bool after = false);
};
class IaBindings {
  public:
    std::optional<Id> layout;
    std::optional<IaTuple> index;
    std::map<unsigned, IaTuple> vertices;
    void clear();
    bool active() const { return layout || index || !vertices.empty(); }
    void transition(const IaBinding &original, const IaBinding *edited, const IaObservation &previous);
    void hazards(const Frame &frame, const SrvOutputs &outputs, bool after = false);
    void apply(State &state, bool includeBuffers = true) const;
};
State effectiveIaBindings(const Frame &frame, Id event, State state, const std::map<Id, IaBinding> &edits,
                          bool includeBuffers = true);
} // namespace flora
