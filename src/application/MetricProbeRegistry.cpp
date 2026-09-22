#include "MetricProbeRegistry.h"
#include "MetricAnalysis.h"
#include <algorithm>
namespace flora {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }
bool natural(const Json &v, uint64_t maximum = UINT64_MAX) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0) &&
           v.get<uint64_t>() <= maximum;
}
uint64_t handle(const Json &v) {
    if (!natural(v))
        fail("Expected uint64 handle");
    return v.get<uint64_t>();
}
} // namespace
bool MetricProbeRegistry::registerType(const Json &path) {
    if (!path.is_string() || path.get_ref<const std::string &>().empty() ||
        path.get_ref<const std::string &>().find('\0') != std::string::npos)
        fail("Invalid probe type path");
    const auto publisher = backend_.lookup(0, "publishers\\GfxInProcPublisher");
    if (publisher.status)
        return false;
    const auto found = backend_.lookup(handle(publisher.handle), path.get<std::string>());
    const auto kind = handle(found.handle);
    if (!kind)
        return false;
    groups_[0].push_back(kind);
    return true;
}
bool MetricProbeRegistry::configure(const Json &group, Bytes configuration) {
    for (const auto kind : groups_[handle(group)]) {
        auto &probe = probes_[kind];
        if (!probe) {
            const auto created = backend_.createProbe(kind, configuration);
            probe = handle(created.handle);
            if (created.status)
                return false;
        }
    }
    return true;
}
std::vector<uint64_t> MetricProbeRegistry::handles() const {
    std::vector<uint64_t> result;
    for (const auto &[key, value] : probes_)
        result.push_back(value);
    return result;
}
bool MetricProbeRegistry::begin(uint64_t key0, uint64_t key1, uint32_t tag, uint64_t context) {
    return MetricProbeFanout(handles(), backend_).begin(key0, key1, tag, context);
}
bool MetricProbeRegistry::end(uint64_t key, uint32_t tag, uint64_t context) {
    return MetricProbeFanout(handles(), backend_).end(key, tag, context);
}
void MetricProbeRegistry::release() {
    groups_.clear();
    for (auto &[key, value] : probes_) {
        if (!value)
            continue;
        if (backend_.destroyProbe(value))
            return;
        value = 0;
    }
}
Json MetricProbeRegistry::snapshot() const {
    Json groups = Json::object(), probes = Json::object();
    for (const auto &[key, value] : groups_)
        groups[std::to_string(key)] = value;
    for (const auto &[key, value] : probes_)
        probes[std::to_string(key)] = value;
    return {{"groups", groups}, {"probes", probes}, {"handles", handles()}};
}
std::vector<uint8_t> dx11MetricProbeConfiguration(const Json &devicePointer) {
    if (!natural(devicePointer))
        fail("Expected uint64 device pointer");
    const auto pointer = devicePointer.get<uint64_t>();
    std::vector<uint8_t> result(12);
    const uint32_t kind = pointer ? 3 : 0;
    std::memcpy(result.data(), &kind, 4);
    std::memcpy(result.data() + 4, &pointer, 8);
    return result;
}
std::vector<uint8_t> dx11MetricConfigurationForKey(const Json &devices, const Json &key) {
    if (!natural(key, UINT32_MAX))
        fail("Expected uint32 device/context key");
    std::map<uint32_t, Json> ordered;
    for (const auto &record : devices) {
        if (!natural(record.at(0), UINT32_MAX))
            fail("Invalid device key");
        ordered[record[0].get<uint32_t>()] = record;
    }
    const auto found = ordered.find(key.get<uint32_t>());
    if (found != ordered.end())
        return dx11MetricProbeConfiguration(found->second.at(1));
    for (const auto &[id, record] : ordered)
        for (const auto &context : record.at(2))
            if (metricIdentityEqual(context, key))
                return dx11MetricProbeConfiguration(record.at(1));
    fail("Invalid reference to device");
}
Json receiveDx11MetricResult(const Json &ids, const Json &completionTokens, const Json &rows,
                             const Json &timings, Json initial) {
    auto result = initial.is_null() ? Json{{"metrics", Json::array()}, {"timings", Json::array()}} : initial;
    for (const auto &id : ids)
        if (!natural(id, UINT32_MAX))
            fail("Expected uint32 metric IDs");
    const bool valid = !rows.empty() && rows.at(0).size() == ids.size();
    const auto finish = [&](int status) {
        auto out = result;
        out["status"] = status;
        return out;
    };
    if (completionTokens.empty() || (!valid && timings.empty()))
        return finish(1);
    if (!timings.empty()) {
        for (const auto &pair : timings)
            if (pair.size() != 2 || !natural(pair[0]) || !natural(pair[1]))
                fail("Expected uint64 timing pairs");
        result["timings"] = timings;
    }
    if (!valid)
        return finish(0);
    for (const auto &row : rows)
        if (row.size() != ids.size())
            fail("Callback matrix is not rectangular");
    for (const auto &row : rows)
        for (const auto &v : row)
            if (!v.is_number())
                fail("Invalid callback number");
    Json metrics = Json::array();
    for (size_t i = 0; i < ids.size(); ++i) {
        Json values = Json::array();
        for (const auto &row : rows)
            values.push_back(row[i].get<double>());
        metrics.push_back(
            {{"metric", ids[i]}, {"values", values}, {"aux", std::vector<uint64_t>(rows.size())}});
    }
    result["metrics"] = metrics;
    return finish(0);
}
} // namespace flora
