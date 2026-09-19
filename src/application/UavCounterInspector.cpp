#include "UavCounterInspector.h"
#include "core/UavCounters.h"
namespace flora {
nlohmann::json inspectUavCounters(const Frame &frame, Replay &replay, Id id, Id resource) {
    using Json = nlohmann::json;
    auto result = Json::array();
    if (!id)
        return result;
    auto draw = isDraw(frame.entry(id).type);
    std::vector<UavCounter> counters;
    if (draw) {
        auto event = frame.event(id);
        counters = boundCounters(frame, event, frame.state(event.state), resource);
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
                  {"value", replay.readCounter(info.view)}};
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
