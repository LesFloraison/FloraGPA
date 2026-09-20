#include "OutputEdits.h"
#include "ApiCommands.h"
#include "core/Contexts.h"
#include <limits>
namespace flora {
namespace {
using Json = nlohmann::json;
template <class T> Json optionalJson(const std::optional<std::vector<T>> &values) {
    return values ? Json(*values) : Json(nullptr);
}
uint64_t integer(const Json &value, uint64_t maximum) {
    if ((!value.is_number_unsigned() && (!value.is_number_integer() || value.get<int64_t>() < 0)) ||
        value.get<uint64_t>() > maximum)
        throw std::runtime_error("Setter integer is outside its unsigned range");
    return value.get<uint64_t>();
}
template <class T> void append(std::vector<uint8_t> &out, T value) {
    const auto *first = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), first, first + sizeof value);
}
template <class T> void array(std::vector<uint8_t> &out, const Json &values, size_t limit) {
    if (!values.is_null() && !values.is_array())
        throw std::runtime_error("Setter array requires a list or null");
    append(out, uint8_t(!values.is_null()));
    if (values.is_null())
        return;
    if (values.size() > limit)
        throw std::runtime_error("Setter array exceeds slot limit");
    for (const auto &value : values)
        append(out, T(integer(value, std::numeric_limits<T>::max())));
}
const Entry &entry(const Frame &frame, Id event) {
    const auto &en = frame.entry(event);
    if (en.category != 7 || !isSrvOutputCommand(en.type))
        throw std::runtime_error("Select an output or stream-output setter");
    if (inspectCommand(frame, event).at("status") != "decoded")
        throw std::runtime_error("Setter requires a complete recovered command layout");
    Reader r(frame.payload(event));
    r.skip(8);
    requireImmediateContext(frame, r.read<Id>());
    return en;
}
} // namespace
nlohmann::json capturedOutputSetter(const Frame &frame, Id event) {
    const auto &en = entry(frame, event);
    if (isStreamOutputTargets(en.type)) {
        auto value = readStreamOutputTargets(frame.payload(event));
        return {{"count", value.count},
                {"buffers", optionalJson(value.buffers)},
                {"offsets", optionalJson(value.offsets)}};
    }
    const auto value = readOutputCommand(en.type, frame.payload(event));
    Json result = Json::object();
    if (en.type == 0x34ff || en.type == 0x3500)
        result.update(
            {{"rtv_count", value.rtvCount}, {"rtvs", optionalJson(value.rtvs)}, {"dsv", value.dsv}});
    if (en.type != 0x34ff)
        result.update({{"start_slot", value.start},
                       {"uav_count", value.uavCount},
                       {"uavs", optionalJson(value.uavs)},
                       {"initial_counts", optionalJson(value.initialCounts)}});
    return result;
}
std::vector<uint8_t> validateOutputSetter(const Frame &frame, Id event, const Json &values) {
    const auto captured = capturedOutputSetter(frame, event);
    if (!values.is_object() || values.size() != captured.size())
        throw std::runtime_error("Provide all and only the selected setter arguments");
    for (auto &[key, value] : captured.items())
        if (!values.contains(key))
            throw std::runtime_error("Missing setter argument: " + key);
    const auto type = frame.entry(event).type;
    auto raw = frame.payload(event).first(16);
    std::vector<uint8_t> encoded(raw.begin(), raw.end());
    if (isStreamOutputTargets(type)) {
        auto count = uint32_t(integer(values.at("count"), 4));
        for (auto key : {"buffers", "offsets"}) {
            const auto &v = values.at(key);
            if ((!v.is_null() && (!v.is_array() || v.size() != count)) ||
                (count && v.is_null() && std::string(key) == "buffers"))
                throw std::runtime_error("SO array must match count; nonzero count requires buffers");
        }
        append(encoded, count);
        array<Id>(encoded, values.at("buffers"), 4);
        array<uint32_t>(encoded, values.at("offsets"), 4);
        auto c = readStreamOutputTargets(encoded);
        const auto ids = c.buffers.value_or(std::vector<Id>{});
        std::set<Id> unique;
        for (auto id : ids)
            if (id && !unique.insert(id).second)
                throw std::runtime_error("A buffer cannot occupy multiple SO target slots");
        const auto offsets = c.offsets.value_or(std::vector<uint32_t>(count, keepOutput));
        validateStreamOutputBindings(frame, ids, offsets);
        return encoded;
    }
    if (type == 0x34ff || type == 0x3500) {
        const auto count = uint32_t(integer(values.at("rtv_count"), keepOutput));
        const auto &ids = values.at("rtvs");
        if ((count == keepOutput && !ids.is_null()) || (count != keepOutput && count != ids.size()))
            throw std::runtime_error("RTV array must match count; KEEP requires null");
        append(encoded, count);
        array<Id>(encoded, values.at("rtvs"), 8);
        append(encoded, integer(values.at("dsv"), UINT64_MAX));
    }
    if (type != 0x34ff) {
        append(encoded, uint32_t(integer(values.at("start_slot"), keepOutput)));
        const auto count = uint32_t(integer(values.at("uav_count"), keepOutput));
        const auto &ids = values.at("uavs"), &initial = values.at("initial_counts");
        if ((count == keepOutput && (!ids.is_null() || !initial.is_null())) ||
            (count != keepOutput && (count != ids.size() || (!initial.is_null() && count != initial.size()))))
            throw std::runtime_error("UAV arrays must match count; KEEP requires null");
        append(encoded, count);
        array<Id>(encoded, values.at("uavs"), 64);
        array<uint32_t>(encoded, values.at("initial_counts"), 64);
    }
    OutputBindingModel::validateArguments(frame, type, encoded);
    return encoded;
}
} // namespace flora
