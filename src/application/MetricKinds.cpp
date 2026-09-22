#include "MetricsDiscovery.h"
#include <cmath>
namespace flora {
using Json = nlohmann::json;
namespace {
bool integer(const Json &value, uint64_t maximum) {
    return value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0) &&
           value.get<uint64_t>() <= maximum;
}
constexpr const char *names[]{"duration", "event", "event_with_range", "throughput", "timestamp", "flag",
                              "ratio",    "raw"};
} // namespace
unsigned metricDescriptorKind(const Json &type, const Json &unit, const Json &symbol) {
    if (!integer(type, 7))
        throw std::runtime_error("Unsupported MD metric type");
    if (!unit.is_string() || !symbol.is_string())
        throw std::runtime_error("Metric unit and symbol must be strings");
    constexpr unsigned kinds[]{1, 0, 0, 0, 2, 5, 3, 5};
    auto kind = kinds[type.get<unsigned>()];
    if (unit == "percent" && symbol != "GpuTime" && symbol != "VulkanGpuTime" && symbol != "D3D12GpuTime")
        kind = 4;
    if (symbol == "AvgGpuCoreFrequencyMHz")
        kind = 3;
    return kind;
}
Json annotateMetricCatalog(Json catalog) {
    for (auto &group : catalog.at("sets"))
        for (auto &definition : group.at("metrics")) {
            if (!definition.contains("metric_type"))
                continue;
            const auto type = definition.at("metric_type");
            const auto kind = metricDescriptorKind(type, definition.at("unit"), definition.at("name"));
            definition.update(
                {{"metric_type_name", names[type.get<unsigned>()]},
                 {"gpa_kind", kind},
                 {"gpa_combination", kind == 0 || kind == 1 || kind == 2 || kind == 5 ? "sum" : "weighted"},
                 {"gpa_kind_source", "MD publisher 0x10d10 + 0x13880"}});
        }
    return catalog;
}
Json validateMetricResult(const Json &metadata, Json result, Bytes raw) {
    const auto &metrics = metadata.at("metrics");
    if (!integer(result.value("reports", Json()), 1) || result.at("reports") != 1 ||
        raw.size() != metadata.at("report_size").get<uint64_t>() || !result.contains("values") ||
        !result["values"].is_array() ||
        result["values"].size() != metrics.size() + metadata.at("information_count").get<size_t>())
        throw std::runtime_error("Invalid calculated hardware counter report shape");
    for (size_t index = 0; index < result["values"].size(); ++index) {
        const auto &value = result["values"][index];
        const auto kind = value.value("type", Json());
        const auto number = value.value("value", Json());
        if (!integer(kind, 3))
            throw std::runtime_error("Unsupported hardware counter value type");
        const auto type = kind.get<unsigned>();
        if (!number.is_null()) {
            const bool valid = type < 2    ? integer(number, type == 0 ? UINT32_MAX : UINT64_MAX)
                               : type == 2 ? number.is_number() && std::isfinite(number.get<double>())
                                           : number.is_boolean();
            if (!valid)
                throw std::runtime_error("Invalid hardware counter value");
        }
        if (index < metrics.size()) {
            constexpr unsigned storage[]{0, 1, 3, 2};
            const auto expected = metrics[index].at("result_type");
            if (!integer(expected, 3) || type != storage[expected.get<unsigned>()])
                throw std::runtime_error("Hardware counter type disagrees with driver metadata");
        }
    }
    Json info = Json::object(), reasons = Json::array();
    const auto &definitions = metadata.at("information");
    for (size_t i = 0; i < definitions.size() && i + metrics.size() < result["values"].size(); ++i)
        info[definitions[i].at("name").get<std::string>()] = result["values"][i + metrics.size()].at("value");
    for (const auto *flag : {"OverrunOccured", "ReportError", "ReportLost", "ReportInconsistent",
                             "ReportCtxSwitchLost", "ReportWithoutWorkload", "ReportContextMismatch"})
        if (info.contains(flag) && !(info[flag].is_boolean() && !info[flag].get<bool>()))
            reasons.push_back(flag);
    result["available"] = reasons.empty();
    result["unavailable_reasons"] = reasons;
    return result;
}
} // namespace flora
