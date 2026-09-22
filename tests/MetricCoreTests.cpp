#include "application/MetricClock.h"
#include "application/MetricQueries.h"
#include "application/MetricReport.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> bytes(const Json &value) {
    const auto data = QByteArray::fromHex(QByteArray::fromStdString(value.get<std::string>()));
    return {reinterpret_cast<const uint8_t *>(data.constData()),
            reinterpret_cast<const uint8_t *>(data.constData()) + data.size()};
}
std::string hex(Bytes data) {
    return QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))
        .toHex()
        .toStdString();
}
std::string bits(double value) { return hex(Bytes(reinterpret_cast<const uint8_t *>(&value), sizeof value)); }
MetricTypedReport records(const Json &values) {
    MetricTypedReport result;
    for (const auto &value : values) {
        const auto data = bytes(value);
        metricTypedDouble(data);
        MetricTypedValue record;
        std::copy(data.begin(), data.end(), record.begin());
        result.push_back(record);
    }
    return result;
}
struct ClockProvider final : MetricClockProvider {
    Json job, trace = Json::array();
    size_t index{}, maximumIndex{};
    explicit ClockProvider(Json value) : job(std::move(value)) {}
    int64_t maximum() override {
        trace.push_back({"maximum"});
        if (job.contains("maximums"))
            return job["maximums"].at(maximumIndex++).get<int64_t>();
        return job.at("period").get<int64_t>();
    }
    std::pair<int64_t, int64_t> sample(bool force) override {
        if (index >= job.at("samples").size())
            throw std::runtime_error("Clock fixture exhausted");
        const auto pair = job["samples"][index++];
        trace.push_back({"sample", force, pair});
        return {pair[0].get<int64_t>(), pair[1].get<int64_t>()};
    }
    bool replaceOffset() const override { return job.value("replace", false); }
};
Json state(const MetricClock &clock) {
    return Json::array({clock.reference(), clock.offset(), clock.previousOffset()});
}
struct ValueSink final : MetricValueSink {
    std::vector<uint32_t> ids;
    Json trace = Json::array();
    std::vector<uint32_t> requestedIds() override {
        trace.push_back({"requested"});
        return ids;
    }
    void writeValue(uint32_t index, double value) override { trace.push_back({"write", index, bits(value)}); }
    void setKey(uint64_t key) override { trace.push_back({"key", key}); }
};
struct QuerySink final : MetricQuerySink {
    Json cfg;
    Json &trace;
    QuerySink(Json value, Json &out) : cfg(std::move(value)), trace(out) {}
    uint64_t category() const override { return cfg.value("category", uint64_t(0)); }
    void fail(const char *method) {
        if (cfg.value("throw", "") == method)
            throw std::runtime_error(std::string("sink ") + method);
    }
    void setKey(uint64_t key) override {
        trace.push_back({cfg["id"], "key", key});
        fail("key");
    }
    void complete(uint64_t key, uint32_t tag) override {
        trace.push_back({cfg["id"], "complete", key, tag});
        fail("complete");
    }
    void flush() override {
        trace.push_back({cfg["id"], "flush"});
        fail("flush");
    }
};
struct Query final : MetricQuery {
    Json cfg;
    Json &trace;
    size_t attempts{};
    Query(Json value, Json &out) : cfg(std::move(value)), trace(out) {}
    uint64_t category() const override { return cfg.value("category", uint64_t(0)); }
    void fail(const char *method) {
        if (cfg.value("throw", "") == method)
            throw std::runtime_error(std::string("query ") + method);
    }
    void begin(void *) override {
        trace.push_back({cfg["id"], "begin"});
        fail("begin");
    }
    void end(void *) override {
        trace.push_back({cfg["id"], "end"});
        fail("end");
    }
    bool ready(bool flush) override {
        trace.push_back({cfg["id"], "ready", flush});
        fail("ready");
        const auto values = cfg.value("ready", Json::array({true}));
        return values.at(std::min(attempts++, values.size() - 1)).get<bool>();
    }
    bool valid() override {
        trace.push_back({cfg["id"], "valid"});
        fail("valid");
        return cfg.value("valid", true);
    }
    bool writeMetric(MetricQuerySink &sink) override {
        trace.push_back({cfg["id"], "write", static_cast<QuerySink &>(sink).cfg["id"]});
        fail("write");
        return cfg.value("write", true);
    }
};
std::vector<std::shared_ptr<MetricQuery>> queries(const Json &values, Json &trace) {
    std::vector<std::shared_ptr<MetricQuery>> result;
    for (const auto &value : values)
        result.push_back(value.is_null() ? nullptr : std::make_shared<Query>(value, trace));
    return result;
}
std::vector<MetricQuerySink *> sinks(const Json &values, Json &trace,
                                     std::vector<std::unique_ptr<QuerySink>> &owned) {
    std::vector<MetricQuerySink *> result;
    for (const auto &value : values) {
        if (value.is_null())
            result.push_back(nullptr);
        else {
            owned.push_back(std::make_unique<QuerySink>(value, trace));
            result.push_back(owned.back().get());
        }
    }
    return result;
}
Json slotSnapshot(const MetricBatchSlots &values) {
    Json result = Json::object();
    for (const auto &[key, list] : values) {
        Json ids = Json::array();
        for (const auto &p : list)
            ids.push_back(p ? Json(p->key0) : Json());
        result[std::to_string(key)] = ids;
    }
    return result;
}
Json run(const Json &job) {
    const auto op = job.at("op").get<std::string>();
    if (op == "ticks")
        return metricTicksToNanoseconds(job.at("ticks").get<uint64_t>(), job.at("frequency").get<uint64_t>());
    if (op == "uint_float")
        return bits(metricUintFloat32(job.at("value").get<uint64_t>()));
    if (op == "typed")
        return bits(metricTypedDouble(bytes(job.at("record"))));
    if (op == "timestamp")
        return metricTimestampInteger(bytes(job.at("record")));
    if (op == "duration")
        return metricGpuTimeMicroseconds(job.at("value").get<uint64_t>());
    if (op == "source") {
        size_t index = 0;
        Json trace = Json::array(), rows = Json::array();
        MetricsDiscoveryClockSource source(job.at("period").get<int64_t>(), [&] {
            const auto pair = job.at("pairs").at(index++);
            trace.push_back(pair);
            return MetricClockPair{pair[0].get<uint64_t>(), pair[1].get<uint64_t>(), pair[2].get<uint64_t>()};
        });
        for (const auto &force : job.at("forces")) {
            auto [offset, reference] = source.sample(force.get<bool>());
            rows.push_back(
                {{"sample", {offset, reference}}, {"success", source.lastSuccess()}, {"trace", trace}});
            trace.clear();
        }
        return rows;
    }
    if (op == "clock") {
        ClockProvider provider(job);
        MetricClock clock(provider);
        Json rows = Json::array();
        rows.push_back({{"return", nullptr}, {"state", state(clock)}, {"trace", provider.trace}});
        provider.trace.clear();
        for (const auto &step : job.at("steps")) {
            Json row;
            try {
                if (step["op"] == "cross")
                    row["return"] =
                        clock.crossed(step["earlier"].get<int64_t>(), step["later"].get<int64_t>());
                else if (step["op"] == "convert")
                    row["return"] = clock.convert(step["value"].get<int64_t>());
                else {
                    clock.update(step.value("force", false));
                    row["return"] = nullptr;
                }
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            row["state"] = state(clock);
            row["trace"] = provider.trace;
            rows.push_back(row);
            provider.trace.clear();
        }
        return rows;
    }
    if (op == "report") {
        auto value = job["records"].is_null() ? std::optional<MetricTypedReport>{}
                                              : std::optional(records(job["records"]));
        MetricReportValues report(std::move(value), job.value("key", uint64_t(0)));
        ValueSink sink;
        sink.ids = job.at("ids").get<std::vector<uint32_t>>();
        Json reads = Json::array();
        for (auto index : job.at("read")) {
            const auto v = report.read(index.get<uint32_t>());
            reads.push_back(v ? Json(bits(*v)) : Json());
        }
        auto written = report.writeMetric(sink);
        return {{"reads", reads}, {"written", written}, {"trace", sink.trace}};
    }
    if (op == "postprocess" || op == "postprocess_batches") {
        ClockProvider provider(job);
        MetricClock clock(provider);
        MetricBusyState busy{job.value("busy", uint64_t(0))};
        Json results = Json::array();
        const auto batches = op == "postprocess" ? Json::array({job.at("reports")}) : job.at("batches");
        for (const auto &batch : batches) {
            Json result;
            try {
                std::vector<MetricTypedReport> data;
                for (const auto &row : batch)
                    data.push_back(records(row));
                auto processed = postprocessMetricReports(data, job.at("metrics"), job.at("information"),
                                                          clock, busy, job.value("normalize", true));
                Json encoded = Json::array();
                for (const auto &row : processed.reports) {
                    Json fields = Json::array();
                    for (const auto &value : row)
                        fields.push_back(hex(value));
                    encoded.push_back(fields);
                }
                result = {{"reports", encoded}, {"keys", processed.keys}};
            } catch (const std::exception &e) {
                result["error"] = e.what();
            }
            result["state"] = state(clock);
            result["trace"] = provider.trace;
            result["busy"] = busy.previousKey;
            results.push_back(result);
            provider.trace.clear();
        }
        return op == "postprocess" ? results.at(0) : results;
    }
    if (op == "batch") {
        Json trace = Json::array(), rows = Json::array();
        MetricQueryBatch batch(queries(job.at("queries"), trace), job.value("key0", uint64_t(0)),
                               job.value("key1", uint64_t(0)), job.value("tag", 0u));
        batch.state = job.value("state", 0u);
        std::vector<std::unique_ptr<QuerySink>> owned;
        auto targets = sinks(job.at("metrics"), trace, owned);
        for (const auto &step : job.at("steps")) {
            Json row;
            try {
                if (step["op"] == "begin") {
                    batch.begin(nullptr);
                    row["return"] = nullptr;
                } else if (step["op"] == "end") {
                    batch.end(nullptr);
                    row["return"] = nullptr;
                } else if (step["op"] == "poll")
                    row["return"] = batch.poll(step.value("wait", false));
                else
                    row["return"] = batch.dispatch(targets, step.value("wait", false));
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            row["state"] = batch.state;
            row["trace"] = trace;
            rows.push_back(row);
            trace.clear();
        }
        return rows;
    }
    if (op == "pool") {
        PendingMetricPool pool;
        pool.failures = job.value("failures", uint8_t(0));
        pool.failureLimit = job.value("failure_limit", uint8_t(0));
        for (const auto &[slot, ids] : job.at("slots").items()) {
            pool.bySlot[std::stoull(slot)];
            for (const auto &id : ids)
                pool.bySlot[std::stoull(slot)].push_back(
                    id.is_null() ? nullptr
                                 : std::make_shared<MetricQueryBatch>(
                                       std::vector<std::shared_ptr<MetricQuery>>{}, id.get<uint64_t>()));
        }
        Json rows = Json::array();
        std::map<uint64_t, size_t> attempts;
        for (const auto &step : job.at("steps")) {
            Json row, trace = Json::array();
            try {
                const auto accepted =
                    pool.drain(step.at("slot").get<uint64_t>(), step.at("limit").get<uint64_t>(),
                               [&](const MetricBatchPtr &item) {
                                   trace.push_back(item->key0);
                                   const auto values = job.at("accept").at(std::to_string(item->key0));
                                   const auto value =
                                       values.at(std::min(attempts[item->key0]++, values.size() - 1));
                                   if (value.is_string())
                                       throw std::runtime_error(value.get<std::string>());
                                   return value.get<bool>();
                               });
                Json ids = Json::array();
                for (const auto &item : accepted)
                    ids.push_back(item->key0);
                row["return"] = ids;
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            row["slots"] = slotSnapshot(pool.bySlot);
            row["failures"] = pool.failures;
            row["trace"] = trace;
            rows.push_back(row);
        }
        return rows;
    }
    if (op == "drain") {
        Json trace = Json::array(), rows = Json::array();
        std::map<uint64_t, MetricBatchPtr> batches;
        for (const auto &cfg : job.at("batches")) {
            const auto id = cfg.at("id").get<uint64_t>();
            auto p = std::make_shared<MetricQueryBatch>(queries(cfg.at("queries"), trace), id,
                                                        cfg.value("key1", uint64_t(0)), cfg.value("tag", 0u));
            p->state = cfg.value("state", 2u);
            batches[id] = p;
        }
        auto list = [&](const Json &ids) {
            std::vector<MetricBatchPtr> out;
            for (const auto &id : ids)
                out.push_back(id.is_null() ? nullptr : batches.at(id.get<uint64_t>()));
            return out;
        };
        PendingMetricPool pool;
        pool.failures = job.value("failures", uint8_t(0));
        pool.failureLimit = job.value("failure_limit", uint8_t(0));
        MetricBatchSlots recycled;
        for (const auto &[id, ids] : job.at("slots").items())
            pool.bySlot[std::stoull(id)] = list(ids);
        MetricQueryDrain drain;
        drain.pending = job.value("pending", true) ? &pool : nullptr;
        drain.recycled = job.value("recycled", true) ? &recycled : nullptr;
        drain.slotCount = job.value("slot_count", uint64_t(0));
        drain.limit = job.value("limit", uint64_t(0));
        drain.limited = job.value("limited", false);
        if (job.value("clock", true))
            drain.updateClock = [&](bool force) {
                trace.push_back({"clock", force});
                if (job.value("clock_error", false))
                    throw std::runtime_error("clock error");
            };
        std::vector<std::unique_ptr<QuerySink>> owned;
        drain.metrics = sinks(job.at("metrics"), trace, owned);
        for (const auto &[id, cfg] : job.at("deferred").items()) {
            auto p = std::make_shared<DeferredMetricQueries>();
            p->batches = list(cfg.at("batches"));
            p->dirty = cfg.value("dirty", false);
            drain.deferred[std::stoull(id)] = p;
        }
        for (const auto &step : job.at("steps")) {
            Json row;
            try {
                if (step["op"] == "arm")
                    row["return"] = drain.deferred.at(step.at("id").get<uint64_t>())->arm();
                else {
                    drain.drain(step.value("wait", false));
                    row["return"] = nullptr;
                }
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            row["trace"] = trace;
            trace.clear();
            row["slots"] = slotSnapshot(pool.bySlot);
            row["recycled"] = slotSnapshot(recycled);
            row["failures"] = pool.failures;
            row["states"] = Json::object();
            for (const auto &[id, p] : batches)
                row["states"][std::to_string(id)] = p->state;
            row["dirty"] = Json::object();
            for (const auto &[id, p] : drain.deferred)
                row["dirty"][std::to_string(id)] = p->dirty;
            rows.push_back(row);
        }
        return rows;
    }
    throw std::runtime_error("Unknown metric core probe");
}
} // namespace
class MetricCoreTests : public QObject {
    Q_OBJECT
  private slots:
    void finiteWaitBound() {
        struct Never final : MetricQuery {
            uint64_t calls{}, flushes{};
            uint64_t category() const override { return 0; }
            void begin(void *) override {}
            void end(void *) override {}
            bool ready(bool flush) override {
                ++calls;
                flushes += flush;
                return false;
            }
            bool valid() override { throw std::runtime_error("Unready query validated"); }
            bool writeMetric(MetricQuerySink &) override { return false; }
        };
        auto query = std::make_shared<Never>();
        MetricQueryBatch batch({query});
        batch.begin(nullptr);
        batch.end(nullptr);
        QVERIFY(!batch.poll(true));
        QCOMPARE(query->calls, uint64_t(0xffffff));
        QCOMPARE(query->flushes, uint64_t(1));
        QCOMPARE(batch.state, 2u);
    }
    void integerRoundBoundary() {
        QCOMPARE(metricUintFloat32(UINT64_MAX), 18446744073709551616.0);
        QCOMPARE(metricGpuTimeMicroseconds(UINT64_MAX), uint64_t(18446744073709551));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, metricTicksToNanoseconds(1, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, metricTypedDouble(std::vector<uint8_t>(15)));
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
    MetricCoreTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricCoreTests.moc"
