#include "MdIterationTransport.h"
#include "MetricAcquisitionPriority.h"
#include "MetricProbeRegistry.h"
#include "MetricsDiscovery.h"
#include <algorithm>
#include <limits>
namespace flora {
using Json = nlohmann::json;
Json mdIterationDescriptors(const Json &catalog, const Json &symbols) {
    const auto plan = planMetrics(catalog, symbols);
    auto names = symbols;
    for (const auto weight : {"GpuCoreClocks", "GpuTime"}) {
        bool found = false;
        for (const auto &group : catalog.at("sets"))
            for (const auto &metric : group.at("metrics"))
                if (metric.at("name") == weight)
                    found = true;
        if (found && std::find(names.begin(), names.end(), Json(weight)) == names.end())
            names.push_back(weight);
    }
    Json result = Json::array();
    for (const auto &symbol : names) {
        planMetrics(catalog, Json::array({symbol}));
        Json choices = Json::array(), definition;
        for (size_t i = 0; i < catalog.at("sets").size(); ++i)
            for (const auto &metric : catalog["sets"][i].at("metrics"))
                if (metric.at("name") == symbol) {
                    choices.push_back(i);
                    definition = metric;
                    break;
                }
        if (definition.is_null() || !definition.contains("gpa_kind"))
            throw std::invalid_argument("Driver descriptor lacks recovered numeric kind");
        const auto name = symbol == "GpuCoreClocks" ? "intel.gpu_core_clocks"
                          : symbol == "GpuTime"     ? "intel.duration"
                                                    : "md." + symbol.get<std::string>();
        result.push_back({{"id", result.size() + 1},
                          {"symbol", symbol},
                          {"name", name},
                          {"kind", definition["gpa_kind"]},
                          {"definition", definition},
                          {"compatible_sets", choices}});
    }
    return {{"catalog", result}, {"plan", plan}};
}
struct MdIterationTransport::State {
    MdIterationClient client;
    uint64_t devicePointer;
    Acquire acquire;
    Json catalog, plan, ids = Json::array(), handles = Json::array(), compatibility = Json::array();
    Json trace = Json::array(), poolReports = Json::array(), cleanupFailure;
    std::unique_ptr<MetricPublisherValues> values;
    std::unique_ptr<MdScheduledPool> pool;
    std::vector<uint64_t> subscriptions;
    MdScheduledPool::Consumer consume;
    uint64_t probesCreated{};
    bool closed{};
    std::unique_ptr<MetricPriorityLock> lock;
    struct Backend final : MetricProbeRegistryBackend {
        State &s;
        explicit Backend(State &state) : s(state) {}
        MetricProbeResult lookup(uint64_t, const std::string &) override { return {0, 1}; }
        MetricProbeResult createProbe(uint64_t, Bytes configuration) override {
            Reader reader(configuration);
            if (reader.read<uint32_t>() != 3 || reader.read<uint64_t>() != s.devicePointer ||
                reader.remaining())
                throw std::invalid_argument("Probe device mismatch");
            ++s.probesCreated;
            return {0, 1};
        }
        int32_t destroyProbe(uint64_t) override {
            s.closePool();
            return 0;
        }
        int32_t beginProbe(uint64_t, uint64_t, uint64_t, uint32_t, uint64_t) override {
            if (!s.pool)
                throw std::logic_error("No active MD query pool");
            s.pool->begin(s.consume);
            return 0;
        }
        int32_t endProbe(uint64_t, uint64_t, uint32_t, uint64_t) override {
            if (!s.pool)
                throw std::logic_error("No active MD query pool");
            s.pool->submit();
            return 0;
        }
    } backend;
    struct Publisher final : MetricPassPublisher {
        State &s;
        MetricProbeRegistry registry;
        explicit Publisher(State &state) : s(state), registry(state.backend) {}
        void setPool(uint32_t capacity) override {
            s.closePool();
            Json choices = Json::array();
            for (const auto &i : s.manager->passes().at(s.manager->currentPass))
                choices.push_back(s.compatibility[i.get<size_t>()]);
            const auto grouped = groupMetricChoices(choices);
            if (grouped.size() != 1 || grouped[0]["compatible_groups"].empty())
                throw std::invalid_argument("Invalid concrete MD pass");
            const auto source = grouped[0]["compatible_groups"][0].get<size_t>();
            s.client.select(s.client.catalog.at("sets").at(source).at("name").get<std::string>());
            s.pool = std::make_unique<MdScheduledPool>(s.client.samples, *s.values, capacity);
        }
        int32_t subscribe(uint64_t, uint64_t h) override {
            if (std::find(s.subscriptions.begin(), s.subscriptions.end(), h) != s.subscriptions.end())
                throw std::invalid_argument("Duplicate live subscription");
            s.subscriptions.push_back(h);
            return 0;
        }
        int32_t unsubscribe(uint64_t, uint64_t h) override {
            std::erase(s.subscriptions, h);
            return 0;
        }
        void flush(uint64_t) override {
            if (s.pool)
                s.pool->finish();
        }
        bool configure(uint64_t group, Bytes bytes) override { return registry.configure(group, bytes); }
        bool begin(uint64_t a, uint64_t b, uint32_t tag, uint64_t ctx) override {
            return registry.begin(a, b, tag, ctx);
        }
        bool end(uint64_t key, uint32_t tag, uint64_t ctx) override { return registry.end(key, tag, ctx); }
    } publisher;
    std::unique_ptr<MetricPassController> manager;
    State(MdIterationClient c, uint64_t device, const Json &symbols, Acquire run, LockFactory factory)
        : client(std::move(c)), devicePointer(device), acquire(std::move(run)), backend(*this),
          publisher(*this) {
        if (!client.samples.supportsReuse())
            throw std::invalid_argument("Reusable counter bridge required");
        const auto described = mdIterationDescriptors(client.catalog, symbols);
        catalog = described["catalog"];
        plan = described["plan"];
        for (size_t i = 0; i < catalog.size(); ++i) {
            handles.push_back(catalog[i]["id"]);
            compatibility.push_back(catalog[i]["compatible_sets"]);
            if (i < symbols.size())
                ids.push_back(catalog[i]["id"]);
        }
        values = std::make_unique<MetricPublisherValues>(client.clock);
        if (!publisher.registry.registerType("dx11"))
            throw std::invalid_argument("Cannot register local MD adapter");
        lock = factory ? factory(client.catalog) : metricDeviceMutex(client.catalog);
        if (!lock)
            throw std::invalid_argument("Missing priority lock");
        try {
            manager = std::make_unique<MetricPassController>(handles, Json::array(), Json::array(), publisher,
                                                             *lock);
        } catch (...) {
            lock->close();
            throw;
        }
    }
    size_t index(const Json &id) const {
        for (size_t i = 0; i < handles.size(); ++i)
            if (metricIdentityEqual(handles[i], id))
                return i;
        throw std::invalid_argument("tuple.index(x): x not in tuple");
    }
    void emergencyClose(const char *stage, std::exception_ptr error) {
        if (cleanupFailure.is_null()) {
            const auto failure = metricPriorityFailure(error);
            Json before;
            try {
                before["owned"] = client.sampleCount();
            } catch (...) {
                const auto e = metricPriorityFailure(std::current_exception());
                before["owned"] = {{"unavailable", e["type"]}, {"message", e["message"]}};
            }
            try {
                before["native_pool"] = client.samples.sampleStats();
            } catch (...) {
                const auto e = metricPriorityFailure(std::current_exception());
                before["native_pool"] = {{"unavailable", e["type"]}, {"message", e["message"]}};
            }
            cleanupFailure = {{"stage", stage},
                              {"type", failure["type"]},
                              {"message", failure["message"]},
                              {"before_device_close", before}};
        }
        client.close();
    }
    void closePool() {
        if (!pool)
            return;
        auto current = std::move(pool);
        std::exception_ptr failure;
        try {
            current->close();
        } catch (...) {
            failure = std::current_exception();
            try {
                emergencyClose("query_pool", failure);
            } catch (...) {
                failure = std::current_exception();
            }
        }
        poolReports.push_back(current->report());
        current.reset();
        if (failure)
            std::rethrow_exception(failure);
    }
    void close() {
        if (closed)
            return;
        std::exception_ptr failure;
        try {
            closePool();
        } catch (...) {
            failure = std::current_exception();
        }
        subscriptions.clear();
        try {
            publisher.registry.release();
        } catch (...) {
            failure = std::current_exception();
            try {
                emergencyClose("publisher", failure);
            } catch (...) {
                failure = std::current_exception();
            }
        }
        if (!cleanupFailure.is_null() && !client.closed())
            client.close();
        lock->close();
        closed = true;
        if (failure)
            std::rethrow_exception(failure);
    }
};
MdIterationTransport::MdIterationTransport(MdIterationClient client, uint64_t pointer, const Json &symbols,
                                           Acquire acquire, LockFactory factory)
    : state_(std::make_unique<State>(std::move(client), pointer, symbols, std::move(acquire),
                                     std::move(factory))) {}
MdIterationTransport::MdIterationTransport(MetricsDiscovery &md, ID3D11Device *device, const Json &symbols,
                                           Acquire acquire, LockFactory factory)
    : MdIterationTransport({md, md, md.catalog(), [&md](const std::string &name) { md.select(name); },
                            [&md] { return uint64_t(md.sampleCount()); }, [&md] { md.close(); },
                            [&md] { return md.closed(); }},
                           uint64_t(reinterpret_cast<uintptr_t>(device)), symbols, std::move(acquire),
                           std::move(factory)) {}
MdIterationTransport::~MdIterationTransport() {
    try {
        state_->close();
    } catch (...) {
        // A terminal owner-close failure must not unlock an active OA resource.
        // Retain the failed state/lock until process exit; explicit close allows retry.
        if (!state_->closed)
            (void)state_.release();
    }
}
Json MdIterationTransport::descriptions() {
    state_->trace.push_back({{"operation", "catalog"}});
    return state_->catalog;
}
Json MdIterationTransport::prepare(const Json &ids) {
    auto &s = *state_;
    Json indices = Json::array();
    for (const auto &id : ids)
        indices.push_back(s.index(id));
    const auto result = s.manager->prepare(indices, s.compatibility, 1, [&](uint32_t key) {
        return dx11MetricConfigurationForKey(Json::array({{1, s.devicePointer, Json::array()}}), key);
    });
    s.trace.push_back(
        {{"operation", "prepare"}, {"ids", ids}, {"passes", result["passes"]}, {"flag", result["flag"]}});
    return {{"groups", result["passes"]}, {"flag", result["flag"]}};
}
void MdIterationTransport::begin(MdScheduledPool::Consumer consume) {
    auto &s = *state_;
    s.consume = std::move(consume);
    try {
        s.manager->begin(1);
    } catch (...) {
        s.consume = {};
        throw;
    }
    s.consume = {};
}
void MdIterationTransport::submit() { state_->manager->end(); }
void MdIterationTransport::flush() { state_->manager->flush(); }
Json MdIterationTransport::passMetricIds(uint32_t pass) const {
    Json result = Json::array();
    for (const auto &index : state_->manager->passes().at(pass))
        result.push_back(state_->handles.at(index.get<size_t>()));
    return result;
}
void MdIterationTransport::deliver(const Json &description, const Json &row) {
    auto &s = *state_;
    s.values->append(description, row);
    std::map<std::string, Json> values;
    for (const auto &v : s.values->lastRecord().at("values"))
        values[v.at("name").get<std::string>()] = v.at("value");
    for (auto h : s.subscriptions) {
        const auto &desc = s.catalog[s.index(h)];
        const auto &converted = values.at(desc["symbol"].get<std::string>());
        const auto value =
            row.at("available").get<bool>()
                ? (converted.is_boolean() ? double(converted.get<bool>()) : converted.get<double>())
                : std::numeric_limits<double>::quiet_NaN();
        std::array<uint8_t, 20> packet{};
        const uint32_t header = 0x14600000;
        std::memcpy(packet.data(), &header, 4);
        std::memcpy(packet.data() + 12, &value, 8);
        s.manager->consumer().receive(h, 0xa3, Bytes(packet));
    }
}
Json MdIterationTransport::replay(uint32_t pass, const Json &ranges, bool flag) {
    auto &s = *state_;
    if (flag)
        throw std::invalid_argument("This MD adapter does not support nondefault replay request flags");
    if (s.manager->requests().empty())
        throw std::invalid_argument("Use the non-metric replay entry point for empty requests");
    s.trace.push_back({{"operation", "replay"}, {"pass_index", pass}, {"ranges", ranges}});
    Json requested = Json::array();
    for (const auto &i : s.manager->requests())
        requested.push_back(s.handles[i.get<size_t>()]);
    s.manager->consumer().reset(requested);
    Json result;
    std::exception_ptr failure;
    try {
        s.manager->select(pass);
        s.acquire(*this, pass, ranges);
        s.manager->finish(true);
        Json handles = Json::array(), rows = Json::array();
        std::vector<size_t> columnSlots;
        for (const auto &i : s.manager->passes().at(pass)) {
            const auto h = s.handles[i.get<size_t>()].get<uint64_t>(), slot = s.manager->consumer().slot(h);
            if (s.manager->consumer().counts()[slot] != ranges.size())
                throw std::invalid_argument("Incomplete range callbacks");
            handles.push_back(h);
            columnSlots.push_back(slot);
        }
        for (const auto &row : s.manager->consumer().rows()) {
            Json selected = Json::array();
            for (auto slot : columnSlots)
                selected.push_back(row[slot]);
            rows.push_back(selected);
        }
        const auto assembled = receiveDx11MetricResult(handles, std::vector<uint64_t>(ranges.size()), rows);
        if (assembled["status"] != 0)
            throw std::invalid_argument("DX11 metric callback rejected empty or mismatched data");
        result = {{"metrics", assembled["metrics"]}, {"parallel_ranges", Json::array()}, {"flag", false}};
    } catch (...) {
        failure = std::current_exception();
    }
    s.closePool();
    if (failure)
        std::rethrow_exception(failure);
    return result;
}
void MdIterationTransport::close() { state_->close(); }
bool MdIterationTransport::closed() const { return state_->closed; }
const Json &MdIterationTransport::catalog() const { return state_->catalog; }
const Json &MdIterationTransport::plan() const { return state_->plan; }
const Json &MdIterationTransport::requestedIds() const { return state_->ids; }
MetricPublisherValues &MdIterationTransport::publisherValues() { return *state_->values; }
Json MdIterationTransport::audit() const {
    const auto &s = *state_;
    const auto lock = s.lock->audit();
    const bool closed = s.client.closed();
    return {{"trace", s.trace},
            {"query_pools", s.poolReports},
            {"probes_created", s.probesCreated},
            {"owned", closed ? Json() : Json(s.client.sampleCount())},
            {"native_pool", closed ? Json() : s.client.samples.sampleStats()},
            {"native_device_closed", closed},
            {"cleanup_failure", s.cleanupFailure},
            {"subscriptions", s.subscriptions},
            {"local_lock_depth", lock.at("depth")},
            {"complete_original_scheduling", false},
            {"cross_process_priority_mutex", true},
            {"priority_mutex", lock},
            {"policy", "One live publisher clock/Busy state; reset callback receiver and unsubscribe/close "
                       "counters after each replay"}};
}
} // namespace flora
