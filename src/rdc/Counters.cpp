#include "Counters.h"
#include <cmath>
#include <stdexcept>
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
    return {{"counter", uint32_t(d.counter)},
            {"name", text(d.name)},
            {"category", text(d.category)},
            {"description", text(d.description)},
            {"resultType", uint32_t(d.resultType)},
            {"resultByteWidth", d.resultByteWidth},
            {"unit", uint32_t(d.unit)},
            {"uuid", {{"words", {d.uuid.words[0], d.uuid.words[1], d.uuid.words[2], d.uuid.words[3]}}}}};
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
