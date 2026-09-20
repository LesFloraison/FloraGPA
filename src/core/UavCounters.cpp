#include "UavCounters.h"
#include <algorithm>

namespace flora {
std::optional<UavCounter> describeCounter(const Frame &frame, Id view) {
    auto it = frame.entries().find(view);
    if (it == frame.entries().end() || it->second.category != 5 || it->second.type != 0x8f)
        return {};
    Reader r(frame.payload(view));
    r.skip(16);
    auto resource = r.read<Id>();
    auto format = r.read<uint32_t>(), dimension = r.read<uint32_t>();
    auto first = r.read<uint32_t>(), count = r.read<uint32_t>(), flags = r.read<uint32_t>();
    r.end();
    if (dimension != 1 || format != 0 || (flags != 2 && flags != 4))
        return {};
    auto desc = frame.resource(resource);
    if (desc.type != 0x83 || !(desc.desc.at(4) & 64) || !desc.desc.at(5))
        return {};
    return UavCounter{view, resource, first, count, desc.desc[5], flags, {}, {}};
}
std::vector<UavCounter> boundCounters(const Frame &frame, const Event &event, const State &state,
                                      Id resource) {
    bool compute = event.type == 0x35 || event.type == 0x36;
    auto start = compute ? 0u : std::min(state.omStart, 64u);
    auto end = compute ? 64u : std::min(state.rtCount, 64u);
    std::vector<UavCounter> result;
    for (auto slot = start; slot < end; ++slot) {
        auto view = compute ? (slot < 8 ? state.csUav[slot] : state.csExtended[slot - 8])
                            : (slot < 8 ? state.rtv[slot] : state.omExtended[slot - 8]);
        auto info = describeCounter(frame, view);
        if (!info || (resource && info->resource != resource))
            continue;
        auto it = std::find_if(result.begin(), result.end(), [&](const auto &x) { return x.view == view; });
        if (it == result.end()) {
            result.push_back(*info);
            it = std::prev(result.end());
        }
        it->bindings.push_back({compute ? "cs" : "om", slot});
    }
    return result;
}
std::vector<UavCounter> referencedCounters(const Frame &frame, Id event, Id resource) {
    auto &entry = frame.entry(event);
    std::vector<UavCounter> result;
    auto reference = [&](Id view, const std::string &field) {
        auto info = describeCounter(frame, view);
        if (!info || info->resource != resource)
            return;
        auto it = std::find_if(result.begin(), result.end(), [&](const auto &x) { return x.view == view; });
        if (it == result.end()) {
            result.push_back(*info);
            it = std::prev(result.end());
        }
        it->references.push_back(field);
    };
    if (entry.category != 7)
        throw std::runtime_error("Counter inspection requires an API event");
    Reader r(frame.payload(event));
    const auto t = entry.type;
    if (t == 0x33 || t == 0x34) {
        r.skip(16);
        reference(r.read<Id>(), "view");
        if (r.flag())
            r.skip(16);
    } else if (t == 0x3f) {
        r.skip(28);
        reference(r.read<Id>(), "source_uav");
    } else if (t == 0x25e || t == 0x3522 || t == 0x3500) {
        r.skip(16);
        if (t == 0x3500) {
            auto rtCount = r.read<uint32_t>();
            if (r.flag()) {
                if (rtCount > 8)
                    throw std::runtime_error("RTV reference count invalid");
                r.skip(size_t(rtCount) * 8);
            }
            r.skip(8);
        }
        r.skip(4);
        auto count = r.read<uint32_t>();
        if (r.flag()) {
            if (count > 64)
                throw std::runtime_error("UAV reference count invalid");
            for (uint32_t i = 0; i < count; ++i)
                reference(r.read<Id>(), "uavs[" + std::to_string(i) + "]");
        }
        if (r.flag()) {
            if (count > 64)
                throw std::runtime_error("UAV initial-count array invalid");
            r.skip(size_t(count) * 4);
        }
    } else
        return result;
    r.end();
    return result;
}
void validateCounterEdit(const Frame &frame, Id view, std::optional<Id> event, const State *effective) {
    if (!describeCounter(frame, view))
        throw std::runtime_error("Select an Append/Consume/Counter UAV view");
    if (event) {
        auto command = frame.event(*event);
        auto counters = boundCounters(frame, command, effective ? *effective : frame.state(command.state));
        if (std::none_of(counters.begin(), counters.end(), [&](const auto &x) { return x.view == view; }))
            throw std::runtime_error("Counter view is not bound to this draw or dispatch");
    }
}
} // namespace flora
