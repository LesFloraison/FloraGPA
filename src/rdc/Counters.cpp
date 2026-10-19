#include "Counters.h"
#include <cmath>
#include <stdexcept>
#include <set>
namespace flora {
using Json = nlohmann::json;
std::string rdcCounterUnit(CounterUnit unit) {
    static constexpr const char *names[] = {"Absolute", "Seconds", "Percentage", "Ratio",  "Bytes",
                                            "Cycles",   "Hertz",   "Volt",       "Celsius"};
    const auto n = uint32_t(unit);
    if (n >= std::size(names))
        throw std::runtime_error("Unknown counter unit");
    return std::string("CounterUnit.") + names[n];
}
Json rdcCounterDescription(const CounterDescription &d) {
    auto text = [](const rdcstr &s) { return std::string(s.c_str(), s.size()); };
    const bool measurable = (d.resultByteWidth == 4 || d.resultByteWidth == 8) &&
        (d.resultType == CompType::Float || d.resultType == CompType::UInt || d.resultType == CompType::SInt);
    return {{"counter", uint32_t(d.counter)},
            {"name", text(d.name)},
            {"category", text(d.category)},
            {"description", text(d.description)},
            {"resultType", uint32_t(d.resultType)},
            {"resultByteWidth", d.resultByteWidth},
            {"unit", uint32_t(d.unit)},
            {"unit_name", rdcCounterUnit(d.unit)},
            {"source", IsNvidiaCounter(d.counter) ? "renderdoc_nvidia" :
                       IsAMDCounter(d.counter) ? "renderdoc_amd" :
                       IsIntelCounter(d.counter) ? "renderdoc_intel" :
                       uint32_t(d.counter) < uint32_t(GPUCounter::FirstAMD) ? "renderdoc_builtin" : "renderdoc_other"},
            {"measurable", measurable},
            {"status", measurable ? "available_not_sampled" : "diagnostic_or_unsupported_format"},
            {"uuid", {{"words", {d.uuid.words[0], d.uuid.words[1], d.uuid.words[2], d.uuid.words[3]}}}}};
}
rdcarray<GPUCounter> selectRdcCounters(const std::map<GPUCounter, CounterDescription> &available,
                                     const Json &requested) {
    rdcarray<GPUCounter> selected;
    if (requested.is_null()) {
        for (const auto &[counter, description] : available)
            if (uint32_t(counter) < uint32_t(GPUCounter::FirstAMD) &&
                rdcCounterDescription(description).at("measurable") == true)
                selected.push_back(counter);
    } else {
        if (!requested.is_array() || requested.empty() || requested.size() > 4096)
            throw std::runtime_error("Counter selection requires 1 to 4096 unique counter IDs");
        std::set<uint32_t> seen;
        for (const auto &value : requested) {
            if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() <= 0) ||
                value.get<uint64_t>() == 0 || value.get<uint64_t>() > UINT32_MAX ||
                !seen.insert(value.get<uint32_t>()).second)
                throw std::runtime_error("Counter IDs must be unique nonzero uint32 integers");
            const auto counter = GPUCounter(value.get<uint32_t>());
            const auto it = available.find(counter);
            if (it == available.end())
                throw std::runtime_error("Requested counter is not enumerated by this replay device: " + value.dump());
            if (rdcCounterDescription(it->second).at("measurable") != true)
                throw std::runtime_error("Requested counter is a diagnostic or unsupported format: " +
                                         std::string(it->second.name.c_str()));
            selected.push_back(counter);
        }
    }
    if (selected.empty()) throw std::runtime_error("No measurable counters selected");
    return selected;
}
Json rdcCounterValue(const CounterDescription &d, const CounterResult &r) {
    if (d.counter != r.counter)
        throw std::runtime_error("Counter result has no matching description");
    if (d.resultByteWidth != 4 && d.resultByteWidth != 8)
        throw std::runtime_error("Unsupported counter value width");
    if (d.resultType == CompType::Float) {
        const double value = d.resultByteWidth == 8 ? r.value.d : double(r.value.f);
        if (std::isfinite(value))
            return value;
        return std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
    }
    // Match the original adapter: the API union exposes unsigned storage for
    // every non-floating counter, including descriptions marked SInt.
    return d.resultByteWidth == 8 ? Json(r.value.u64) : Json(r.value.u32);
}
} // namespace flora
