#include "application/MetricIterations.h"
#include "application/data/FrameApiTypes.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
#include <cmath>
#include <cstring>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json decode(Json value) {
    if (value.is_object() && value.size() == 1 && value.contains("$f")) {
        const auto bytes = QByteArray::fromHex(QByteArray::fromStdString(value["$f"].get<std::string>()));
        if (bytes.size() != 8)
            throw std::invalid_argument("Invalid float fixture");
        double result;
        std::memcpy(&result, bytes.data(), 8);
        return result;
    }
    if (value.is_structured())
        for (auto &child : value)
            child = decode(std::move(child));
    return value;
}
Json encode(Json value) {
    if (value.is_number_float()) {
        const auto d = value.get<double>();
        return {{"$f", std::isnan(d)
                           ? "nan"
                           : QByteArray(reinterpret_cast<const char *>(&d), 8).toHex().toStdString()}};
    }
    if (value.is_structured())
        for (auto &child : value)
            child = encode(std::move(child));
    return value;
}
struct Transport : MetricIterationTransport {
    Json job, trace = Json::array(), requested = Json::array();
    size_t serial{}, prepares{}, polls{};
    explicit Transport(Json j) : job(std::move(j)) {}
    void fail(const char *op) {
        if (job.value("throw", "") == op)
            throw std::runtime_error(std::string("injected ") + op);
    }
    Json descriptions() override {
        trace.push_back({"catalog"});
        fail("catalog");
        return job.value("descriptions", Json::array());
    }
    Json queryFlagDescriptions() override {
        if (job.contains("flag_descriptions"))
            trace.push_back({"flag_catalog"});
        fail("flag_catalog");
        return job.value("flag_descriptions", Json::array());
    }
    Json prepare(const Json &ids) override {
        trace.push_back({"prepare", ids});
        fail("prepare");
        requested = ids;
        if (job.contains("plans")) {
            const auto &plans = job["plans"];
            return plans.at(std::min(prepares++, plans.size() - 1));
        }
        Json groups = Json::array();
        for (size_t i = 0; i < ids.size(); ++i)
            groups.push_back(Json::array({i}));
        return {{"groups", groups}, {"flag", job.value("prepare_flag", Json(false))}};
    }
    Json replay(uint32_t pass, const Json &ranges, bool flag) override {
        trace.push_back({"replay", pass, requested, ranges, flag});
        fail("replay");
        if (job.contains("responses")) {
            const auto &responses = job["responses"];
            return responses.at(std::min(serial++, responses.size() - 1));
        }
        ++serial;
        Json metrics = Json::array(), parallel = Json::array();
        if (!requested.empty()) {
            const auto mid = requested.at(pass);
            Json values = Json::array(), aux = Json::array();
            for (size_t r = 0; r < ranges.size(); ++r) {
                values.push_back(mid.get<double>() + double(serial) * .5 + double(r) * 3);
                aux.push_back(uint64_t(1) << ((serial + r) % 64));
            }
            metrics.push_back({{"metric", mid}, {"values", values}, {"aux", aux}});
        }
        const auto policy = job.value("policy", "normal");
        const bool weight = !requested.empty() && requested.at(pass).get<uint64_t>() < 42;
        if ((weight && policy == "weight_empty") ||
            (weight && policy == "weight_empty_first" && serial == 1) ||
            (weight && policy == "weight_empty_twice" && serial <= 2) ||
            (!weight && policy == "main_empty") || (!weight && policy == "main_empty_first" && serial == 1))
            metrics.clear();
        if (!metrics.empty()) {
            if ((weight && policy == "weight_wrong_range") || (!weight && policy == "main_wrong_range"))
                metrics[0]["values"].push_back(99.);
            if (weight && policy == "weight_wrong_id")
                metrics[0]["metric"] = 999;
            if (weight && policy == "weight_multiple")
                metrics.push_back(metrics[0]);
        }
        const auto timing = job.value("timing", "absent");
        Json pairs = Json::array();
        for (size_t r = 0; r < ranges.size(); ++r)
            pairs.push_back(Json::array({serial * 100 + r * 19, 5 + r}));
        if (timing == "ordinary")
            parallel.push_back(pairs);
        if (timing == "empty")
            parallel.push_back(Json::array());
        if (timing == "multiple")
            parallel = Json::array({pairs, Json::array({{901, 902}})});
        if (timing == "boundary")
            parallel = Json::array(
                {Json::array({{UINT64_MAX, UINT64_MAX - 1}, {uint64_t(1) << 63, (uint64_t(1) << 53) + 1}})});
        return {{"metrics", metrics}, {"parallel_ranges", parallel}, {"flag", true}};
    }
    bool cancel() {
        const auto target = job.value("cancel_at", Json());
        const bool value = target == "always" || target == Json(polls);
        ++polls;
        trace.push_back({"cancel", value});
        return value;
    }
};
Json run(const Json &j) {
    const auto op = j.at("op");
    if (op == "decoder_types")
        return frameApiTypes;
    if (op == "select") {
        const auto range = selectMetricPasses(j.at("groups"), j.at("requested"), j.at("flag"));
        return {{"first", range.first}, {"count", range.count}};
    }
    if (op == "range_table")
        return selectMetricRangeTable(j.at("categorized"), j.value("fallback", Json::array()));
    if (op == "ranges")
        return mapMetricRanges(j.at("requested"), j.at("captured"), j.at("ergs"));
    if (op == "kinds") {
        Json result = Json::array();
        for (uint32_t t = 0; t < 65536; ++t)
            result.push_back(metricApiKind(uint16_t(t)));
        return result;
    }
    if (op == "frame_index") {
        Frame frame(QString::fromStdString(j.at("frame").get<std::string>()).toStdWString());
        return buildFrameMetricIndex(frame);
    }
    if (op == "flags")
        return parseMetricQueryFlags(j.at("iterations"), j.at("descriptions"),
                                     j.value("initial", Json::array()));
    if (op == "times")
        return updateMetricSampleTimes(j.at("previous"), j.at("parallel"));
    if (op == "weight")
        return chooseWeightMetric(j.at("descriptions"));
    if (op == "iteration_pass")
        return metricIterationPass(j.at("requested"), j.value("mapping", Json::array()));
    if (op == "values")
        return prepareMetricIterationValues(j.at("iterations"), j.at("descriptions"), j.at("requested"),
                                            j.at("weights"), j.value("groups", Json::array()),
                                            j.value("initial", Json()));
    Transport transport(j.at("transport"));
    Json result;
    std::function<bool()> cancel;
    if (j["transport"].contains("cancel_at"))
        cancel = [&] { return transport.cancel(); };
    try {
        MetricIterationRunner runner(transport);
        if (op == "outer")
            result = MetricOuterPassRunner(transport).run(
                j.at("ids"), j.at("ranges"), j.value("requested", Json(metricAllPasses)),
                j.value("request_flag", Json(false)), cancel, j.value("initial", Json()));
        else if (op == "execute")
            result = runner.execute(j.at("ids"), j.at("ranges"), j.value("options", Json::object()), cancel);
        else if (op == "collect")
            result = runner.collect(j.at("ids"), j.at("ranges"), j.value("options", Json::object()), cancel);
        else if (op == "execute_ranges")
            result = runner.executeRanges(j.at("ids"), j.at("requested_ranges"), j.at("captured"),
                                          j.at("ergs"), j.value("options", Json::object()), cancel);
        else if (op == "execute_frame") {
            Frame frame(QString::fromStdString(j.at("frame").get<std::string>()).toStdWString());
            result = runner.executeFrame(frame, j.at("ids"), j.value("requested_ranges", Json()),
                                         j.value("options", Json::object()), cancel);
        } else
            throw std::invalid_argument("Unknown probe operation");
        result = {{"result", result}};
    } catch (const MetricIterationError &e) {
        result = {{"error", e.what()}, {"partial", e.result()}};
    } catch (const std::exception &e) {
        result = {{"error", e.what()}};
    }
    result["trace"] = transport.trace;
    return result;
}
} // namespace
class MetricIterationTests : public QObject {
    Q_OBJECT
  private slots:
    void cumulativeReceiver() {
        Transport t({{"timing", "ordinary"}});
        const auto result = MetricOuterPassRunner(t).run({42, 43}, Json::array({{2, 2}}));
        QCOMPARE(result["result"]["metrics"].size(), size_t(3));
        QCOMPARE(result["result"]["parallel_ranges"].size(), size_t(3));
        QVERIFY(result["result"]["parallel_ranges"][1].empty());
        QVERIFY(t.trace.back() == Json::array({"prepare", Json::array()}));
    }
    void incompleteSuccess() {
        Transport t({{"descriptions", Json::array({{{"id", 42}, {"name", "value"}, {"kind", 3}}})}});
        const auto r = MetricIterationRunner(t).execute({42}, Json::array({{1, 1}}),
                                                        {{"weights", {1.}}, {"samples", 0}});
        QVERIFY(r["status"] == "success");
        QVERIFY(r["complete"] == false);
        QVERIFY_THROWS_EXCEPTION(MetricIterationError,
                                 MetricIterationRunner(t).collect({42}, Json::array({{1, 1}}),
                                                                  {{"weights", {1.}}, {"samples", 0}}));
    }
    void rangeEndpoints() {
        const auto r = mapMetricRanges(Json::array({{9, 0, 2}, {2, 5, 6}}),
                                       Json::array({{2, 0, 1}, {2, 2, 2}}), {11, 12, 13});
        QVERIFY(r == Json::array({{12, 13}, {0, 0}}));
        QVERIFY(selectMetricRangeTable({{"2", Json::array()}}, {1}).empty());
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
                result.push_back(encode(run(decode(job))));
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    MetricIterationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricIterationTests.moc"
