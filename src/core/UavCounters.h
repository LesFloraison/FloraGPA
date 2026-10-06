#pragma once
#include "Frame.h"
#include <optional>

namespace flora {
class CounterValueUnavailable : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
struct CounterBinding {
    std::string stage;
    uint32_t slot;
};
struct UavCounter {
    Id view, resource;
    uint32_t firstElement, numElements, stride, flags;
    std::vector<CounterBinding> bindings;
    std::vector<std::string> references;
};
std::optional<UavCounter> describeCounter(const Frame &frame, Id view);
std::vector<UavCounter> boundCounters(const Frame &frame, const Event &event, const State &state,
                                      Id resource = 0);
std::vector<UavCounter> referencedCounters(const Frame &frame, Id event, Id resource);
void validateCounterEdit(const Frame &frame, Id view, std::optional<Id> event = {},
                         const State *effective = nullptr);
} // namespace flora
