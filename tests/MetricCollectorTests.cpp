#include "application/MdSamplePool.h"
#include "application/MdScheduledPool.h"
#include "application/MetricCollector.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
struct FakeTransport final : MetricSampleTransport, MetricCounterTransport {
    struct Counter {
        uint64_t id{}, uses{}, polls{};
        std::string state;
    };
    std::vector<Counter> cached;
    std::map<uint64_t, Counter> owned;
    std::optional<Counter> single;
    uint64_t created{}, reused{}, serial{};
    uint64_t identityOffset{};
    bool never{}, supported = true;
    bool samplesSupported = true, drainSupported = true, allocate{};
    unsigned readyAfter{};
    std::string error;
    Json trace = Json::array();
    bool supportsSamples() const override { return samplesSupported; }
    bool supportsReuse() const override { return supported; }
    bool supportsDrain() const override { return drainSupported; }
    void fail(const char *operation) {
        if (("," + error + ",").find("," + std::string(operation) + ",") != std::string::npos)
            throw std::runtime_error(operation);
    }
    void begin() override {
        trace.push_back({"counter_begin"});
        fail("begin");
        if (single)
            throw std::runtime_error("Counter exists");
        single = Counter{++serial, 0, 0, "begun"};
    }
    void submit() override {
        trace.push_back({"counter_submit"});
        fail("submit");
        if (!single || single->state != "begun")
            throw std::runtime_error("Invalid counter submit");
        single->state = "ended";
    }
    std::optional<MetricResult> poll(bool flush) override {
        trace.push_back({"counter_poll", flush});
        fail("poll");
        if (!single || single->state == "begun")
            throw std::runtime_error("Invalid counter poll");
        ++single->polls;
        if (!never && (flush || (readyAfter && single->polls >= readyAfter)))
            single->state = "ready";
        if (single->state != "ready")
            return {};
        return MetricResult{{{"available", single->id % 2 == 0}}, {uint8_t(single->id % 256)}};
    }
    void discard() override {
        trace.push_back({"discard"});
        fail("discard");
        single.reset();
    }
    void sampleReserve(unsigned count) override {
        trace.push_back({"reserve", count});
        fail("reserve");
        if (!owned.empty() || !cached.empty())
            throw std::runtime_error("Invalid reserve");
        for (unsigned i = 0; i < count; ++i)
            cached.push_back({++created, 0, 0, "cached"});
    }
    uint64_t sampleBegin() override {
        trace.push_back({"begin"});
        fail("begin");
        if (cached.empty() && allocate)
            cached.push_back({++created, 0, 0, "cached"});
        if (cached.empty())
            throw std::runtime_error("Empty native cache");
        auto item = cached.back();
        cached.pop_back();
        reused += item.uses != 0;
        ++item.uses;
        item.polls = 0;
        item.state = "begun";
        owned[++serial] = item;
        return serial;
    }
    uint64_t sampleInfo(uint64_t token) override {
        trace.push_back({"info", token});
        fail("info");
        return owned.at(token).id + identityOffset;
    }
    void sampleSubmit(uint64_t token) override {
        trace.push_back({"submit", token});
        fail("submit");
        auto &item = owned.at(token);
        if (item.state != "begun")
            throw std::runtime_error("Invalid submit");
        item.state = "ended";
    }
    std::optional<MetricResult> samplePoll(uint64_t token, bool flush) override {
        trace.push_back({"poll", token, flush});
        fail("poll");
        auto &item = owned.at(token);
        if (item.state == "begun")
            throw std::runtime_error("Invalid poll");
        ++item.polls;
        if (!never && (flush || (readyAfter && item.polls >= readyAfter)))
            item.state = "ready";
        if (item.state != "ready")
            return {};
        return MetricResult{{{"available", token % 2 == 0}}, {uint8_t(token % 256)}};
    }
    void sampleRecycle(uint64_t token) override {
        trace.push_back({"recycle", token});
        fail("recycle");
        auto item = owned.at(token);
        if (item.state != "ready")
            throw std::runtime_error("Invalid recycle");
        owned.erase(token);
        cached.push_back(item);
    }
    void sampleRelease(uint64_t token) override {
        trace.push_back({"release", token});
        fail("release");
        if (!owned.erase(token))
            throw std::runtime_error("Unknown release token");
    }
    void sampleClearCache() override {
        trace.push_back({"clear"});
        fail("clear");
        if (!owned.empty())
            throw std::runtime_error("Live native counters");
        cached.clear();
    }
    Json stats() const {
        return {{"created", created}, {"reused", reused}, {"cached", cached.size()}, {"owned", owned.size()}};
    }
    Json sampleStats() override {
        trace.push_back({"stats"});
        fail("stats");
        return stats();
    }
};
struct Publisher final : MetricPublisherObserver {
    FakeTransport &transport;
    Json records = Json::array(), refreshes = Json::array();
    explicit Publisher(FakeTransport &value) : transport(value) {}
    void update(bool force) override {
        transport.trace.push_back({"clock", force});
        transport.fail("clock");
        if (transport.single && transport.single->state == "begun")
            throw std::runtime_error("Clock during begun sample");
        for (const auto &[id, item] : transport.owned)
            if (item.state == "begun")
                throw std::runtime_error("Clock during begun sample");
        refreshes.push_back(records.size());
    }
    size_t recordCount() const override { return records.size(); }
    size_t refreshCount() const override { return refreshes.size(); }
};
template <class Pool> Json sampled(const Json &job) {
    FakeTransport transport;
    transport.supported = job.value("supported", true);
    transport.samplesSupported = job.value("samples_supported", true);
    transport.allocate = job["op"] == "sample";
    transport.readyAfter = job.value("ready_after", 0u);
    Publisher publisher(transport);
    Pool pool(transport, publisher, job.value("capacity", 3u), job.value("timeout", 10000u));
    Json rows = Json::array();
    bool consumerError = false;
    for (const auto &step : job.at("steps")) {
        Json row;
        try {
            const auto op = step.at("op").get<std::string>();
            if (op == "fault")
                transport.error = step.at("value").get<std::string>();
            else if (op == "info_offset")
                transport.identityOffset = step.at("value").get<uint64_t>();
            else if (op == "never")
                transport.never = step.at("value").get<bool>();
            else if (op == "consumer_error")
                consumerError = step.at("value").get<bool>();
            else if (op == "begin")
                pool.begin(step.value("consume", true) ? MdScheduledPool::Consumer([&](MetricResult &value) {
                    transport.trace.push_back({"consume", value.raw.at(0)});
                    if (consumerError)
                        throw std::runtime_error("consumer");
                    if (job.value("mutate", false))
                        value.values["consumed"] = value.raw.at(0);
                    publisher.records.push_back({value.values.at("available"), value.raw.at(0)});
                })
                                                       : MdScheduledPool::Consumer{});
            else if (op == "submit")
                pool.submit();
            else if (op == "drain")
                pool.drain(step.value("wait", false));
            else if (op == "finish")
                pool.finish();
            else if (op == "close")
                pool.close();
            else if (op == "end") {
                auto result = pool.end();
                row["return"] = {{"values", result.values}, {"raw", result.raw}};
            }
            if (!row.contains("return"))
                row["return"] = nullptr;
        } catch (const std::exception &e) {
            row["error"] = e.what();
        }
        row["report"] = pool.report();
        row["owned"] = pool.ownedCount();
        row["active"] = pool.active();
        row["stats"] = transport.stats();
        row["records"] = publisher.records;
        row["refreshes"] = publisher.refreshes;
        row["trace"] = transport.trace;
        transport.trace.clear();
        rows.push_back(row);
    }
    return rows;
}
Json counter(const Json &job) {
    FakeTransport transport;
    transport.drainSupported = job.value("supported", true);
    transport.readyAfter = job.value("ready_after", 0u);
    Publisher publisher(transport);
    MdCounter pool(transport, publisher, job.value("timeout", 10000u));
    Json rows = Json::array();
    for (const auto &step : job.at("steps")) {
        Json row;
        try {
            const auto op = step.at("op").get<std::string>();
            if (op == "fault")
                transport.error = step.at("value").get<std::string>();
            else if (op == "begin")
                pool.begin();
            else if (op == "end") {
                const auto result = pool.end();
                row["return"] = {{"values", result.values}, {"raw", result.raw}};
                // The synchronous caller publishes after End returns.
                publisher.records.push_back({result.values.at("available"), result.raw.at(0)});
            }
            if (!row.contains("return"))
                row["return"] = nullptr;
        } catch (const std::exception &e) {
            row["error"] = e.what();
        }
        row["audit"] = pool.audit();
        row["active"] = pool.active();
        row["single"] = transport.single ? Json{{"id", transport.single->id},
                                                {"polls", transport.single->polls},
                                                {"state", transport.single->state}}
                                         : Json();
        row["records"] = publisher.records;
        row["refreshes"] = publisher.refreshes;
        row["trace"] = transport.trace;
        transport.trace.clear();
        rows.push_back(std::move(row));
    }
    return rows;
}
struct ProbeQuery final : MetricQuery {
    std::string id;
    int32_t kind;
    Json cfg;
    Json &trace;
    size_t attempt{};
    ProbeQuery(std::string name, int32_t category, Json value, Json &out)
        : id(std::move(name)), kind(category), cfg(std::move(value)), trace(out) {}
    uint64_t category() const override { return uint64_t(int64_t(kind)); }
    void begin(void *context) override {
        trace.push_back({"begin", id, uint64_t(reinterpret_cast<uintptr_t>(context))});
        attempt = 0;
    }
    void end(void *context) override {
        trace.push_back({"end", id, uint64_t(reinterpret_cast<uintptr_t>(context))});
    }
    bool ready(bool flush) override {
        trace.push_back({"ready", id, flush});
        const auto values = cfg.value("ready", Json::array({true}));
        return values.at(std::min(attempt++, values.size() - 1)).get<bool>();
    }
    bool valid() override {
        trace.push_back({"valid", id});
        return cfg.value("valid", true);
    }
    bool writeMetric(MetricQuerySink &metric) override;
};
struct ProbeProvider final : MetricQueryProvider {
    std::string id;
    int32_t kind;
    Json cfg;
    Json &trace;
    size_t count{};
    ProbeProvider(Json value, Json &out)
        : id(value.at("id").get<std::string>()), kind(value.at("category").get<int32_t>()),
          cfg(std::move(value)), trace(out) {}
    int32_t category() const override { return kind; }
    std::shared_ptr<MetricQuery> create() override {
        ++count;
        trace.push_back({"create", id, count});
        if (cfg.value("throw_at", size_t(0)) == count)
            throw std::runtime_error("create " + id);
        const auto nullEvery = cfg.value("null_every", size_t(0));
        if (nullEvery && count % nullEvery == 0)
            return {};
        return std::make_shared<ProbeQuery>(id + "#" + std::to_string(count), kind, cfg, trace);
    }
};
struct ProbeMetric final : MetricProviderBinding {
    std::string id;
    Json &trace;
    ProbeMetric(std::string name, std::vector<MetricProviderPtr> providers, size_t selected,
                std::optional<std::set<uint32_t>> compatible, Json &out)
        : MetricProviderBinding(std::move(providers), selected, std::move(compatible)), id(std::move(name)),
          trace(out) {}
    void setMode(bool value) override {
        trace.push_back({"mode", id, value});
        MetricProviderBinding::setMode(value);
    }
    MetricProviderPtr bind(int32_t kind) override {
        trace.push_back({"bind", id, kind});
        return MetricProviderBinding::bind(kind);
    }
    void setKey(uint64_t key) override { trace.push_back({"key", id, key}); }
    void complete(uint64_t key, uint32_t tag) override { trace.push_back({"complete", id, key, tag}); }
    void flush() override { trace.push_back({"flush", id}); }
};
bool ProbeQuery::writeMetric(MetricQuerySink &metric) {
    trace.push_back({"write", id, static_cast<ProbeMetric &>(metric).id});
    return cfg.value("write", true);
}
Json describe(const MetricBatchPtr &batch) {
    if (!batch)
        return nullptr;
    Json ids = Json::array();
    for (const auto &query : batch->queries())
        ids.push_back(static_cast<ProbeQuery &>(*query).id);
    return {{"queries", ids},
            {"state", batch->state},
            {"key0", batch->key0},
            {"key1", batch->key1},
            {"tag", batch->tag}};
}
Json describe(const MetricBatchSlots &bySlot) {
    Json out = Json::object();
    for (const auto &[id, list] : bySlot) {
        Json rows = Json::array();
        for (const auto &b : list)
            rows.push_back(describe(b));
        out[std::to_string(id)] = rows;
    }
    return out;
}
Json ids(const std::vector<MetricProviderPtr> &providers) {
    Json out = Json::array();
    for (const auto &p : providers)
        out.push_back(static_cast<ProbeProvider &>(*p).id);
    return out;
}
Json ids(const std::vector<std::shared_ptr<MetricProviderBinding>> &metrics) {
    Json out = Json::array();
    for (const auto &m : metrics)
        out.push_back(static_cast<ProbeMetric &>(*m).id);
    return out;
}
Json run(const Json &job) {
    if (job["op"] == "scheduled")
        return sampled<MdScheduledPool>(job);
    if (job["op"] == "sample")
        return sampled<MdSamplePool>(job);
    if (job["op"] == "reuse")
        return sampled<MdReusingPool>(job);
    if (job["op"] == "counter")
        return counter(job);
    Json trace = Json::array(), rows = Json::array();
    std::map<std::string, MetricProviderPtr> providers;
    std::vector<MetricProviderPtr> ordered;
    for (const auto &cfg : job.at("providers")) {
        if (cfg.is_null()) {
            ordered.push_back(nullptr);
            continue;
        }
        auto p = std::make_shared<ProbeProvider>(cfg, trace);
        providers[p->id] = p;
        ordered.push_back(p);
    }
    const auto counts = [&] {
        Json out = Json::object();
        for (const auto &[id, p] : providers)
            out[id] = static_cast<ProbeProvider &>(*p).count;
        return out;
    };
    if (job["op"] == "order")
        return ids(metricPoolProviderOrder(ordered));
    if (job["op"] == "pool") {
        MetricQueryPool pool(ordered, job.value("capacity", uint64_t(0)));
        std::map<std::string, MetricBatchPtr> handles;
        for (const auto &step : job.at("steps")) {
            Json row;
            try {
                const auto slot = step.value("slot", uint64_t(0));
                const auto op = step.at("op").get<std::string>();
                if (op == "empty")
                    row["return"] = pool.empty(slot);
                else if (op == "capacity") {
                    pool.capacity = step.at("value").get<uint64_t>();
                    row["return"] = nullptr;
                } else if (op == "append") {
                    pool.append(slot, step.at("handle").is_null()
                                          ? nullptr
                                          : handles.at(step["handle"].get<std::string>()));
                    row["return"] = nullptr;
                } else {
                    auto b = op == "take"    ? pool.take(slot, step.value("replenish", false))
                             : op == "first" ? pool.takeFirst(slot)
                                             : pool.peek(slot);
                    if (step.contains("save"))
                        handles[step["save"].get<std::string>()] = b;
                    row["return"] = describe(b);
                }
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            row["slots"] = describe(pool.bySlot);
            row["counts"] = counts();
            row["trace"] = trace;
            trace.clear();
            rows.push_back(row);
        }
        return rows;
    }
    auto contexts = std::make_shared<MetricContextSlots>();
    contexts->contexts = job.value("contexts", std::vector<uint64_t>{});
    contexts->contextType = [&](uint64_t value) {
        trace.push_back({"context_type", value});
        return job.at("context_types").at(std::to_string(value)).get<int32_t>();
    };
    if (job["op"] == "contexts") {
        for (const auto &value : job.at("values")) {
            auto index = contexts->slot(value.get<uint64_t>());
            rows.push_back({{"return", index}, {"contexts", contexts->contexts}, {"trace", trace}});
            trace.clear();
        }
        return rows;
    }
    std::map<std::string, std::shared_ptr<ProbeMetric>> metrics;
    for (const auto &cfg : job.at("metrics")) {
        std::vector<MetricProviderPtr> list;
        for (const auto &id : cfg.at("providers"))
            list.push_back(providers.at(id.get<std::string>()));
        auto p = std::make_shared<ProbeMetric>(
            cfg.at("id").get<std::string>(), std::move(list), cfg.value("selected", size_t(0)),
            cfg.contains("compatible") ? std::optional(cfg["compatible"].get<std::set<uint32_t>>())
                                       : std::nullopt,
            trace);
        metrics[p->id] = p;
    }
    auto preferred =
        job.contains("preferred") ? std::optional(job["preferred"].get<uint32_t>()) : std::nullopt;
    MetricCollector collector(
        contexts, [&](bool force) { trace.push_back({"clock", force}); }, job.value("capacity", uint64_t(3)),
        job.value("mode", false), preferred, job.value("defer_reads", false));
    for (const auto &step : job.at("steps")) {
        Json row;
        try {
            const auto op = step.at("op").get<std::string>();
            if (op == "subscribe")
                collector.subscribe(metrics.at(step["id"].get<std::string>()));
            else if (op == "unsubscribe")
                collector.unsubscribe(metrics.at(step["id"].get<std::string>()));
            else if (op == "capacity")
                collector.setCapacity(step["value"].get<uint64_t>());
            else if (op == "begin")
                row["return"] = describe(
                    collector.begin(step.value("key0", uint64_t(11)), step.value("key1", uint64_t(22)),
                                    step.value("tag", 6u), step.value("context", uint64_t(0))));
            else if (op == "end")
                collector.end(step.value("context", uint64_t(0)));
            else if (op == "drain")
                collector.drain.drain(step.value("wait", false));
            else if (op == "notify")
                collector.notifications.notify(step.value("context", uint64_t(0)),
                                               step.at("key").get<uint64_t>(),
                                               step.at("kind").get<int32_t>());
            else if (op == "defer")
                collector.deferReads = step.at("value").get<bool>();
            else if (op == "rebuild")
                collector.rebuild();
            if (!row.contains("return"))
                row["return"] = nullptr;
        } catch (const std::exception &e) {
            row["error"] = e.what();
        }
        row["generation"] = collector.generation;
        row["capacity"] = collector.capacity;
        row["contexts"] = contexts->contexts;
        row["slot_count"] = collector.drain.slotCount;
        row["providers"] = ids(collector.subscriptions.providers);
        row["metrics"] = ids(collector.subscriptions.metrics);
        row["compatible"] = collector.subscriptions.compatible;
        row["selection"] = Json::object();
        for (const auto &[id, m] : metrics)
            row["selection"][id] = {{"provider", static_cast<ProbeProvider &>(*m->provider()).id},
                                    {"selected", m->selectedIndex()},
                                    {"mode", m->mode}};
        row["recycled"] = collector.recycled ? describe(collector.recycled->bySlot) : Json();
        row["recording"] = collector.recording ? describe(collector.recording->bySlot) : Json();
        row["pending"] = collector.pending ? Json{{"slots", describe(collector.pending->bySlot)},
                                                  {"failures", collector.pending->failures},
                                                  {"failure_limit", collector.pending->failureLimit}}
                                           : Json();
        row["deferred"] = Json::object();
        for (const auto &[id, d] : collector.drain.deferred) {
            Json batches = Json::array();
            for (const auto &b : d->batches)
                batches.push_back(describe(b));
            row["deferred"][std::to_string(id)] = {
                {"batches", batches}, {"dirty", d->dirty}, {"source_slot", d->sourceSlot}};
        }
        row["counts"] = counts();
        row["trace"] = trace;
        trace.clear();
        rows.push_back(row);
    }
    return rows;
}
} // namespace
class MetricCollectorTests : public QObject {
    Q_OBJECT
  private slots:
    void adapterTimeoutCleanup() {
        for (bool reuse : {false, true}) {
            FakeTransport metrics;
            metrics.allocate = !reuse;
            Publisher publisher(metrics);
            std::unique_ptr<MdSamplePool> pool;
            if (reuse)
                pool = std::make_unique<MdReusingPool>(metrics, publisher, 2, 1);
            else
                pool = std::make_unique<MdSamplePool>(metrics, publisher, 2, 1);
            pool->begin();
            pool->submit();
            metrics.never = true;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pool->finish());
            QVERIFY(metrics.owned.empty() && metrics.cached.empty());
            QVERIFY(!pool->active() && pool->ownedCount() == 0);
            metrics.never = false;
            pool->begin();
            QCOMPARE(pool->end().raw.size(), size_t(1));
            pool->close();
        }
        FakeTransport metrics;
        Publisher publisher(metrics);
        MdCounter counter(metrics, publisher, 1);
        counter.begin();
        metrics.never = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, counter.end());
        QVERIFY(!counter.active() && !metrics.single);
        metrics.never = false;
        counter.begin();
        QCOMPARE(counter.end().raw.size(), size_t(1));
    }
    void timeoutCleanup() {
        FakeTransport metrics;
        Publisher publisher(metrics);
        MdScheduledPool pool(metrics, publisher, 3, 1);
        for (unsigned i = 0; i < 3; ++i) {
            pool.begin();
            pool.submit();
        }
        metrics.never = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pool.finish());
        QVERIFY(metrics.owned.empty() && metrics.cached.empty());
        QCOMPARE(pool.ownedCount(), size_t(0));
        metrics.never = false;
        pool.begin();
        auto result = pool.end();
        pool.close();
        QCOMPARE(result.raw.size(), size_t(1));
        QVERIFY(metrics.owned.empty() && metrics.cached.empty());
    }
    void emptyBatchesRetainIdentity() {
        MetricQueryPool pool({}, 3);
        auto third = pool.take(0, true);
        auto second = pool.takeFirst(0);
        auto first = pool.peek(0);
        QVERIFY(third && second && first && third != second && second != first && first != third);
        pool.append(0, third);
        QCOMPARE(pool.take(0), third);
        QCOMPARE(pool.take(0), first);
        QVERIFY(pool.empty(0));
        pool.append(42, {});
        QVERIFY(!pool.bySlot.contains(42));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        Json result = Json::array();
        for (const auto &job : Json::parse(input.readAll().toStdString()))
            try {
                result.push_back(run(job));
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    MetricCollectorTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricCollectorTests.moc"
