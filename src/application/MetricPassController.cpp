#include "MetricPassController.h"
#include "MetricAnalysis.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }
bool natural(const Json &v, uint64_t maximum = UINT64_MAX) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0) &&
           v.get<uint64_t>() <= maximum;
}
uint32_t u32(const Json &v) {
    if (!natural(v, UINT32_MAX))
        fail("Expected uint32");
    return v.get<uint32_t>();
}
void indices(const Json &values, size_t count, const char *message) {
    for (const auto &v : values)
        if (!natural(v) || v.get<uint64_t>() >= count)
            fail(message);
}
} // namespace
MetricConsumer::MetricConsumer(const Json &handles, Json timingHandle)
    : timingHandle_(std::move(timingHandle)) {
    reset(handles);
}
void MetricConsumer::reset(const Json &handles) {
    std::vector<uint64_t> values;
    for (const auto &h : handles) {
        if (!natural(h) || h == 0)
            fail("Invalid metric handle");
        values.push_back(h.get<uint64_t>());
    }
    if (std::set<uint64_t>(values.begin(), values.end()).size() != values.size())
        fail("Duplicate metric handle");
    handles_ = std::move(values);
    counts_.assign(handles_.size(), 0);
    rows_.clear();
    timings_.clear();
}
size_t MetricConsumer::slot(uint64_t handle) const {
    const auto found = std::find(handles_.begin(), handles_.end(), handle);
    if (found == handles_.end())
        fail("Unexpected metric handle");
    return size_t(found - handles_.begin());
}
void MetricConsumer::receive(uint64_t handle, uint64_t payloadType, std::optional<Bytes> payload,
                             bool present) {
    if (!present)
        return;
    const auto index = slot(handle);
    if (payload && payload->size() < 4)
        fail("Truncated metric payload");
    std::optional<uint32_t> header;
    if (payload)
        header = Reader(*payload).read<uint32_t>() & 0x7fe00000;
    if (metricIdentityEqual(handle, timingHandle_)) {
        if (header != 0x18c00000)
            return;
        if (payload->size() < 8)
            fail("Truncated timing payload");
        Reader reader(*payload);
        reader.skip(4);
        const auto count = reader.read<uint32_t>() / 52;
        if (payload->size() < 8 + uint64_t(count) * 52)
            fail("Truncated timing records");
        for (uint32_t i = 0; i < count; ++i) {
            Reader record(payload->subspan(28 + size_t(i) * 52));
            const auto start = record.read<double>(), end = record.read<double>();
            if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end < 0 ||
                start >= std::ldexp(1., 64) || end >= std::ldexp(1., 64))
                fail("Timing value outside uint64 domain");
            const auto a = uint64_t(start), b = uint64_t(end);
            timings_.push_back({a, b - a});
        }
        return;
    }
    if (payloadType != 0xa0 && payloadType != 0xa3)
        fail("Unexpected metric payload type");
    double value = -1;
    if (header == (payloadType == 0xa0 ? 0x14000000u : 0x14600000u)) {
        if (payload->size() < 20)
            fail("Truncated numeric payload");
        Reader reader(*payload);
        reader.skip(12);
        value = payloadType == 0xa0 ? double(reader.read<uint64_t>()) : reader.read<double>();
    }
    const auto row = counts_[index];
    if (row == rows_.size())
        rows_.emplace_back(handles_.size(), 0.);
    rows_[size_t(row)][index] = value;
    ++counts_[index];
}
Json MetricConsumer::snapshot() const {
    return {{"handles", handles_}, {"counts", counts_}, {"rows", rows_}, {"timings", timings_}};
}
MetricProbeFanout::MetricProbeFanout(const Json &handles, MetricProbeBackend &backend) : backend_(backend) {
    for (const auto &h : handles) {
        if (!natural(h))
            fail("Invalid probe handle");
        handles_.push_back(h.get<uint64_t>());
    }
}
bool MetricProbeFanout::begin(uint64_t key0, uint64_t key1, uint32_t tag, uint64_t context) {
    for (auto h : handles_)
        if (h && backend_.beginProbe(h, key0, key1, tag, context))
            return false;
    return true;
}
bool MetricProbeFanout::end(uint64_t key, uint32_t tag, uint64_t context) {
    for (auto h : handles_)
        if (h && backend_.endProbe(h, key, tag, context))
            return false;
    return true;
}
MetricPassController::MetricPassController(const Json &handles, const Json &requests, const Json &passes,
                                           MetricPassPublisher &publisher, MetricPriorityLock &primary,
                                           MetricPriorityLock *secondary, const Json &options,
                                           MetricConsumer *consumer)
    : handles_(handles), requests_(requests), passes_(passes),
      timingHandle_(options.value("timing_handle", Json(0))), publisher_(publisher), primary_(primary),
      secondary_(secondary) {
    for (const auto &h : handles_)
        if (!natural(h) || h == 0)
            fail("Invalid metric handle");
    indices(requests_, handles_.size(), "Invalid metric index");
    for (const auto &group : passes_)
        indices(group, handles_.size(), "Invalid metric index");
    std::set<uint64_t> requested;
    for (const auto &i : requests_)
        requested.insert(handles_[i.get<size_t>()].get<uint64_t>());
    if (requested.size() != requests_.size())
        fail("Duplicate requested handle");
    const auto ch = options.value("consumer_handle", Json(1));
    if (!natural(ch) || ch == 0)
        fail("Invalid consumer handle");
    consumerHandle_ = ch.get<uint64_t>();
    vendor_ = u32(options.value("vendor", Json(0x8086)));
    probeDeviceKey_ = u32(options.value("probe_mode", Json(0)));
    poolSize_ = u32(options.value("pool_size", Json(256)));
    if (vendor_ != 0x8086 && !secondary_)
        fail("Non-Intel pass requires secondary priority lock");
    if (!consumer)
        ownedConsumer_ = std::make_unique<MetricConsumer>(Json::array(), timingHandle_);
    consumer_ = consumer ? consumer : ownedConsumer_.get();
}
void MetricPassController::setProbeDeviceKey(const Json &value) { probeDeviceKey_ = u32(value); }
Json MetricPassController::prepare(const Json &requests, const Json &compatibility, const Json &deviceKey,
                                   const ConfigurationProvider &provider) {
    const auto key = u32(deviceKey);
    indices(requests, handles_.size(), "Metric request outside descriptor table");
    if (requests.empty()) {
        reportedPassCount = 1;
        return {{"passes", Json::array()}, {"flag", false}};
    }
    if (compatibility.size() != handles_.size())
        fail("Compatibility table size mismatch");
    Json choices = Json::array();
    for (const auto &i : requests)
        choices.push_back(compatibility[i.get<size_t>()]);
    const auto grouped = groupMetricChoices(choices);
    reportedPassCount = 1;
    requests_ = requests;
    passes_ = Json::array();
    for (const auto &group : grouped) {
        Json pass = Json::array();
        for (const auto &i : group.at("metrics"))
            pass.push_back(requests[i.get<size_t>()]);
        passes_.push_back(std::move(pass));
    }
    configure(key, provider(key));
    reportedPassCount = uint32_t(passes_.size());
    return {{"passes", passes_}, {"flag", false}};
}
void MetricPassController::select(const Json &value) {
    const auto index = u32(value);
    if (index == currentPass)
        return;
    if (!requests_.empty() && index >= passes_.size())
        fail("Pass outside plan");
    if (vendor_ != 0x8086) {
        secondary_->setPriority(8);
        secondary_->acquire();
    }
    primary_.setPriority(7);
    primary_.acquire();
    currentPass = index;
    if (requests_.empty())
        return;
    completedProbes = 0;
    if (!index) {
        Json handles = Json::array();
        for (const auto &i : requests_)
            handles.push_back(handles_[i.get<size_t>()]);
        consumer_->reset(handles);
    }
    const auto &group = passes_[index];
    const bool timing = std::any_of(group.begin(), group.end(), [&](const Json &i) {
        return metricIdentityEqual(handles_[i.get<size_t>()], timingHandle_);
    });
    publisher_.setPool(timing ? poolSize_ : std::min(poolSize_, 256u));
    for (const auto &i : group)
        if (publisher_.subscribe(consumerHandle_, handles_[i.get<size_t>()].get<uint64_t>()))
            throw std::runtime_error("GM metric subscription failed");
}
void MetricPassController::begin(const Json &deviceKey) {
    const auto key = u32(deviceKey);
    if (!requests_.empty() && key == probeDeviceKey_ && !publisher_.begin(0, 0, 6, 0))
        throw std::runtime_error("GM BeginProbe failed");
}
bool MetricPassController::configure(const Json &deviceKey, Bytes configuration) {
    const auto key = u32(deviceKey);
    if (!publisher_.configure(0, configuration))
        return false;
    probeDeviceKey_ = key;
    return true;
}
void MetricPassController::end() {
    if (requests_.empty())
        return;
    if (!publisher_.end(0, 6, 0))
        throw std::runtime_error("GM EndProbe failed");
    ++completedProbes;
}
void MetricPassController::flush() { publisher_.flush(consumerHandle_); }
void MetricPassController::finish(const Json &unsubscribe) {
    if (!unsubscribe.is_boolean())
        fail("Unsubscribe must be bool");
    if (requests_.empty())
        return;
    flush();
    if (unsubscribe.get<bool>()) {
        for (const auto &group : passes_)
            for (const auto &i : group)
                publisher_.unsubscribe(consumerHandle_, handles_[i.get<size_t>()].get<uint64_t>());
        currentPass = UINT32_MAX;
    }
    if (vendor_ != 0x8086) {
        secondary_->setPriority(UINT32_MAX);
        secondary_->release();
    }
    primary_.setPriority(UINT32_MAX);
    primary_.release();
}
Json MetricPassController::snapshot() const {
    return {{"handles", handles_},
            {"requests", requests_},
            {"passes", passes_},
            {"current_pass", currentPass},
            {"completed_probes", completedProbes},
            {"reported_pass_count", reportedPassCount},
            {"probe_mode", probeDeviceKey_},
            {"consumer", consumer_->snapshot()}};
}
} // namespace flora
