#include "UavCounterInspector.h"
#include "core/UavCounters.h"
namespace flora {
nlohmann::json inspectUavCounters(const Frame &, Replay &replay, Id id, Id resource) {
    const auto &frame = replay.frame();
    using Json = nlohmann::json;
    auto result = Json::array();
    if (!id)
        return result;
    auto draw = isDraw(frame.entry(id).type);
    std::vector<UavCounter> counters;
    if (draw) {
        auto event = frame.event(id);
        counters = boundCounters(
            frame, event, effectiveBindings(frame, id, frame.state(event.state), replay.options()), resource);
    } else
        counters = referencedCounters(frame, id, resource);
    for (const auto &info : counters) {
        Json item{{"view", info.view},
                  {"resource", info.resource},
                  {"first_element", info.firstElement},
                  {"num_elements", info.numElements},
                  {"stride", info.stride},
                  {"flags", info.flags},
                  {"kind", info.flags == 2 ? "append_consume" : "counter"},
                  {"bindings", Json::array()},
                  {"value", nullptr}};
        try {
            item["value"] = replay.readCounter(info.view);
        } catch (const CounterValueUnavailable &error) {
            item["status"] = "counter_value_unavailable";
            item["reason"] = error.what();
        }
        for (const auto &binding : info.bindings)
            item["bindings"].push_back({{"stage", binding.stage}, {"slot", binding.slot}});
        if (!draw) {
            item["scope"] = "command_reference";
            item["reference_fields"] = info.references;
        }
        result.push_back(std::move(item));
    }
    return result;
}
} // namespace flora
