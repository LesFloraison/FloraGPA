#include "application/Experiment.h"
#include "application/MdFrameRanges.h"
#include "application/MdIterationTransport.h"
#include "application/MetricsDiscovery.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read fixture");
    return Json::parse(file.readAll().toStdString());
}
struct Counter {
    Json trace = Json::array();
    std::string fault;
    bool active{};
    uint64_t serial{};
    MetricCommandCounterClient::Consumer current;
    std::vector<std::pair<uint64_t, MetricCommandCounterClient::Consumer>> pending;
    void fail(const char *name) {
        if (fault == name)
            throw std::runtime_error(std::string("Injected ") + name);
    }
    MetricResult value(uint64_t id) { return {{{"token", id}}, {uint8_t(id)}}; }
    MetricCommandCounterClient client() {
        return {[&](auto consume) {
                    trace.push_back({"begin", bool(consume)});
                    fail("begin");
                    if (active)
                        throw std::runtime_error("Already active");
                    active = true;
                    ++serial;
                    current = std::move(consume);
                },
                [&] {
                    trace.push_back({"submit", serial});
                    fail("submit");
                    pending.emplace_back(serial, std::move(current));
                    active = false;
                },
                [&] {
                    trace.push_back({"end", serial});
                    fail("end");
                    auto r = value(serial);
                    active = false;
                    if (current)
                        current(r);
                    current = {};
                    return r;
                }};
    }
    void flush() {
        auto jobs = std::move(pending);
        pending.clear();
        for (auto &[id, consume] : jobs) {
            trace.push_back({"deliver", id});
            auto r = value(id);
            if (consume)
                consume(r);
        }
    }
};
template <class C> Json exercise(C &counter, Counter &native, const Json &steps) {
    Json output = Json::array();
    for (const auto &step : steps) {
        Json row;
        try {
            const auto op = step.at("op");
            if (op == "command")
                counter.scope(step.at("event").get<Id>(), [&] {
                    native.trace.push_back({"command", step.at("event")});
                    if (step.value("fail", false))
                        throw std::runtime_error("Injected command");
                });
            else if (op == "fault")
                native.fault = step.at("value").get<std::string>();
            else if (op == "flush")
                native.flush();
            else if (op == "verify")
                row["return"] = counter.verify();
            if (!row.contains("return"))
                row["return"] = nullptr;
        } catch (const std::exception &e) {
            row["error"] = e.what();
        }
        row["state"] = counter.snapshot();
        row["native_active"] = native.active;
        row["pending"] = native.pending.size();
        row["trace"] = native.trace;
        native.trace.clear();
        output.push_back(row);
    }
    return output;
}
Json run(const Json &job) {
    const auto op = job.at("op");
    if (op == "interval_selection" || op == "frame_selection") {
        static std::map<std::string, std::unique_ptr<Frame>> fixtures;
        const auto path = job.at("capture").get<std::string>();
        auto &stored = fixtures[path];
        if (!stored)
            stored = std::make_unique<Frame>(QString::fromStdString(path).toStdWString());
        const auto &frame = *stored;
        Experiment edits(frame);
        for (const auto &id : job.value("disabled", Json::array()))
            edits.setEnabled(frame, id.get<Id>(), false);
        if (job.contains("experiment"))
            edits.load(QString::fromStdString(job.at("experiment").get<std::string>()), frame);
        ReplayOptions options;
        edits.apply(frame, options);
        const auto &effective = options.viewFrame ? *options.viewFrame : frame;
        if (op == "interval_selection")
            return selectMetricInterval(effective, &edits, job.value("start", Json()),
                                        job.value("end", Json()));
        return selectFrameMetricRanges(effective, job.value("indices", Json()), &edits);
    }
    Counter native;
    if (op == "interval") {
        MetricIntervalCounter counter(
            native.client(), job.at("interval"),
            [&](MetricResult &r) {
                native.trace.push_back({"consume", r.values, r.raw});
                native.fail("consume");
            },
            job.value("callbacks", false));
        return exercise(counter, native, job.at("steps"));
    }
    FrameRangeCounter counter(
        native.client(), job.at("ranges"),
        [&](const Json &range, MetricResult &r) {
            native.trace.push_back({"consume", range.at("range_index"), r.values, r.raw});
            native.fail("consume");
        },
        job.value("callbacks", false));
    return exercise(counter, native, job.at("steps"));
}
} // namespace
class MdFrameRangeTests : public QObject {
    Q_OBJECT
  private slots:
    void deferredCallbacksOwnRangeIdentity() {
        Counter native;
        Json received = Json::array();
        {
            FrameRangeCounter ranges(
                native.client(),
                {{{"range_index", 9},
                  {"start_event", 10},
                  {"end_event", 12},
                  {"commands", {{{"event", 10}}, {{"event", 12}}}}}},
                [&](const Json &row, MetricResult &) { received.push_back(row["range_index"]); }, true);
            ranges.scope(10, [] {});
            ranges.scope(12, [] {});
            QCOMPARE(ranges.verify().size(), size_t(1));
            QVERIFY(received.empty());
        }
        native.flush();
        QCOMPARE(received, Json::array({9}));
    }
    void realFrameScopes() {
        std::string phase = "initialization";
        try {
            const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
            if (root.isEmpty() || qEnvironmentVariableIntValue("FLORA_TEST_INTEL_METRICS") != 1)
                QSKIP("Set reference root and Intel metric opt-in");
            for (const auto filename :
                 {"GF2_Exilium_2026_03_03__00_19_35.gpa_frame", "bf1_2026_01_21__16_53_05.gpa_frame"}) {
                Frame frame((root + '/' + filename).toStdWString());
                phase = std::string(filename) + ": baseline";
                ReplayOptions options;
                options.vendor = 0x8086;
                Replay replay(frame, options);
                replay.run();
                const auto baseline = replay.output();
                const auto all = selectFrameMetricRanges(frame);
                for (const Json invalid : {Json(0), Json(false), Json("all"), Json::object()})
                    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, selectFrameMetricRanges(frame, invalid));
                const auto count = all["ranges"].size();
                QVERIFY(count > 3);
                const auto selection = selectFrameMetricRanges(frame, {count - 1, 0, count / 2});
                const auto ranges = selection.at("ranges");
                Json mapped = Json::array();
                for (const auto &r : ranges)
                    mapped.push_back({r["start_event"], r["end_event"]});
                MetricsDiscovery md(replay.nativeDevice());
                QTemporaryDir dir;
                QVERIFY(dir.isValid());
                MetricPriorityOptions priority;
                priority.path = dir.filePath("frame-priority.table");
                priority.mutexName = "FloraGPA_FrameRange_" + QUuid::createUuid().toString();
                Json records = Json::array(), boundary;
                MdIterationTransport session(
                    md, replay.nativeDevice(), {"CsThreads"},
                    [&](MdIterationTransport &transport, uint32_t pass, const Json &requested) {
                        if (requested != mapped)
                            throw std::runtime_error("Range mapping changed");
                        MetricCommandCounterClient client{
                            [&](auto consume) { transport.begin(std::move(consume)); },
                            [&] { transport.submit(); },
                            {}};
                        FrameRangeCounter counter(
                            client, ranges,
                            [&, pass](const Json &info, MetricResult &r) {
                                const auto &set = md.selected();
                                const auto n = set.at("metrics").size();
                                Json values = Json::array(), information = Json::array();
                                for (size_t i = 0; i < r.values.at("values").size(); ++i)
                                    (i < n ? values : information).push_back(r.values["values"][i]);
                                records.push_back(info["range_index"]);
                                transport.deliver(set,
                                                  {{"set", set["name"]},
                                                   {"event", nullptr},
                                                   {"start_event", info["start_event"]},
                                                   {"end_event", info["end_event"]},
                                                   {"pass_index", pass},
                                                   {"sample_index", 0},
                                                   {"raw_report", "range.bin"},
                                                   {"raw_sha256", sha256(r.raw)},
                                                   {"available", r.values["available"]},
                                                   {"unavailable_reasons", r.values["unavailable_reasons"]},
                                                   {"values", values},
                                                   {"information", information}});
                            },
                            true);
                        replay.run({}, {}, {}, [&](Id id, const auto &body) { counter.scope(id, body); });
                        boundary = counter.verify();
                        const auto image = replay.output();
                        if (image.width != baseline.width || image.height != baseline.height ||
                            image.rgba != baseline.rgba)
                            throw std::runtime_error("Complete scoped replay changed presentation");
                    },
                    [&](const Json &catalog) { return metricDeviceMutex(catalog, priority); });
                session.prepare(session.requestedIds());
                phase = std::string(filename) + ": scheduled frame ranges";
                const auto measured = session.replay(0, mapped, false);
                QCOMPARE(measured["metrics"][0]["values"].size(), size_t(3));
                QCOMPARE(records, Json::array({0, count / 2, count - 1}));
                QCOMPARE(boundary.size(), size_t(3));
                session.close();
                QCOMPARE(md.sampleCount(), 0u);
                QCOMPARE(md.sampleStats()["cached"], Json(0));
                QVERIFY(!QFile::exists(*priority.path));
                auto lock = metricDeviceMutex(md.catalog(), priority);
                lock->acquire();
                try {
                    unsigned reports{};
                    auto consume = [&](MetricResult &result) {
                        if (result.raw.empty())
                            throw std::runtime_error("Empty synchronous MD report");
                        ++reports;
                    };
                    MetricCommandCounterClient direct{[&](auto) { md.begin(); }, [&] { md.submit(); },
                                                      [&] { return md.end(); }};
                    FrameRangeCounter synchronous(direct, ranges,
                                                  [&](const Json &, MetricResult &r) { consume(r); });
                    phase = std::string(filename) + ": synchronous frame ranges";
                    replay.run({}, {}, {}, [&](Id id, const auto &body) { synchronous.scope(id, body); });
                    QCOMPARE(synchronous.verify().size(), size_t(3));
                    QCOMPARE(reports, 3u);
                    QCOMPARE(replay.output().rgba, baseline.rgba);
                    const auto interval = selectMetricInterval(frame, nullptr, ranges.front()["start_event"],
                                                               ranges.back()["end_event"]);
                    MetricIntervalCounter continuous(direct, interval, consume);
                    phase = std::string(filename) + ": synchronous interval";
                    replay.run({}, {}, {}, [&](Id id, const auto &body) { continuous.scope(id, body); });
                    QCOMPARE(continuous.verify()["commands_seen"].size(), interval["commands"].size());
                    QCOMPARE(reports, 4u);
                    QCOMPARE(replay.output().rgba, baseline.rgba);
                    MetricPublisherValues values(md);
                    MdScheduledPool pool(md, values, 2);
                    MetricCommandCounterClient scheduled{
                        [&](auto callback) { pool.begin(std::move(callback)); }, [&] { pool.submit(); },
                        [&] { return pool.end(); }};
                    MetricIntervalCounter callbackInterval(scheduled, interval, consume, true);
                    phase = std::string(filename) + ": scheduled interval";
                    replay.run({}, {}, {},
                               [&](Id id, const auto &body) { callbackInterval.scope(id, body); });
                    QCOMPARE(callbackInterval.verify()["reports"], Json(1));
                    QCOMPARE(reports, 5u);
                    QCOMPARE(replay.output().rgba, baseline.rgba);
                    pool.close();
                } catch (...) {
                    md.close();
                    lock->close();
                    throw;
                }
                lock->close();
                QCOMPARE(md.sampleCount(), 0u);
                QCOMPARE(md.sampleStats()["cached"], Json(0));
                QVERIFY(!QFile::exists(*priority.path));
                // Verify a command exception crosses the scope without publishing a partial report.
                Counter fake;
                FrameRangeCounter failed(fake.client(), ranges, [](const Json &, MetricResult &) {}, true);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         replay.run({}, {}, {}, [&](Id id, const auto &body) {
                                             failed.scope(id, [&] {
                                                 if (id == ranges[0]["start_event"].get<Id>())
                                                     throw std::runtime_error("Injected replay command");
                                                 body();
                                             });
                                         }));
                QVERIFY(failed.snapshot()["failed"] == true);
                QVERIFY_THROWS_EXCEPTION(std::invalid_argument, failed.verify());
                QCOMPARE(fake.pending.size(), size_t(0));
            }
        } catch (const std::exception &error) {
            QFAIL((phase + ": " + error.what()).c_str());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        Json result = Json::array();
        for (const auto &job : read(args[2])) {
            try {
                result.push_back(run(job));
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        }
        QFile output(args[3]);
        if (!output.open(QIODevice::WriteOnly))
            return 2;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    MdFrameRangeTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MdFrameRangeTests.moc"
