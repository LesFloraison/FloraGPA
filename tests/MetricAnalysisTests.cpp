#include "application/MetricAnalysis.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <limits>
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
Json stats(const MetricStatistics &s) {
    return {{"median", s.median},
            {"minimum", s.minimum},
            {"maximum", s.maximum},
            {"mean", s.mean},
            {"variation_percent", s.variationPercent}};
}
Json iterated(const IteratedMetric &v) {
    auto result = stats(v.statistics);
    result.update({{"values", v.values}, {"kind", v.kind}, {"weight", v.weight}});
    return result;
}
int kind(const Json &v) {
    if (!v.is_number_integer() ||
        (v.is_number_unsigned() ? v.get<uint64_t>() > 255 : v.get<int64_t>() < 0 || v.get<int64_t>() > 255))
        throw std::invalid_argument("Metric kind must be a byte");
    return v.get<int>();
}
IteratedMetric construct(const Json &job) {
    return iteratedMetric(job.at("values").get<std::vector<double>>(), kind(job.at("kind")),
                          job.value("weight", 1.));
}
Json run(const Json &j) {
    const auto op = j.at("op");
    if (op == "sum")
        return metricSequentialSum(j.at("values").get<std::vector<double>>());
    if (op == "stats")
        return stats(metricStatistics(j.at("values").get<std::vector<double>>()));
    if (op == "construct")
        return iterated(construct(j));
    if (op == "combine")
        return iterated(combineMetrics(construct(j.at("left")), construct(j.at("right"))));
    if (op == "aggregate") {
        std::vector<int64_t> groups;
        if (j.contains("groups") && !j["groups"].is_null()) {
            for (const auto &g : j["groups"])
                if (!g.is_number_integer() ||
                    (g.is_number_unsigned() ? g.get<uint64_t>() >= 0x80000000ULL
                                            : g.get<int64_t>() < 0 || g.get<int64_t>() >= 0x80000000LL))
                    throw std::invalid_argument("Range groups must be nonnegative int32 IDs");
            groups = j["groups"].get<std::vector<int64_t>>();
        }
        Json result = Json::array();
        for (const auto &v :
             aggregateMetricRanges(j.at("values").get<std::vector<std::vector<double>>>(), kind(j.at("kind")),
                                   j.at("weights").get<std::vector<double>>(), groups))
            result.push_back(iterated(v));
        return result;
    }
    if (op == "summary")
        return metricSampleSummary(j.at("values"));
    if (op == "assembly")
        return assembleMetricIterations(j.at("iterations"));
    if (op == "matrix")
        return metricProfileMatrix(j.at("profile"));
    if (op == "summaries")
        return summarizeMetricRecords(j.at("profile"));
    if (op == "groups")
        return groupMetricChoices(j.at("choices"));
    if (op == "plan")
        return planMetrics(j.at("catalog"), j.at("requested"));
    if (op == "requested")
        return requestedMetricResults(j.at("profile"), j.at("plan"));
    if (op == "publisher_profile")
        return publisherMetricProfile(j.at("profile"), j.at("publisher"));
    if (op == "publisher_analysis")
        return publisherMetricAnalysis(j.at("profile"), j.at("publisher"));
    throw std::invalid_argument("Unknown probe operation");
}
} // namespace
class MetricAnalysisTests : public QObject {
    Q_OBJECT
  private slots:
    void numericalBoundaries() {
        const auto nan = std::numeric_limits<double>::quiet_NaN(),
                   inf = std::numeric_limits<double>::infinity();
        QCOMPARE(metricStatistics({}).mean, 0.);
        QVERIFY(std::isnan(metricStatistics(std::vector{nan}).mean));
        const auto values = iteratedMetric({0, 1}, 3);
        QCOMPARE(values.statistics.median, .5);
        QCOMPARE(values.statistics.variationPercent, 100.);
        QVERIFY(std::signbit(metricStatistics(std::vector{-0., 0.}).minimum));
        QVERIFY(!std::signbit(metricStatistics(std::vector{-0., 0.}).maximum));
        QVERIFY(std::isnan(iteratedMetric({inf}, 3, 0).values[0]));
        QCOMPARE(combineMetrics(iteratedMetric({2}, 0, 7), iteratedMetric({3}, 0, 9)).values[0], 5.);
        QCOMPARE(combineMetrics(iteratedMetric({2}, 3, 1), iteratedMetric({8}, 3, 2)).values[0], 6.);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                 combineMetrics(iteratedMetric({1}, 0), iteratedMetric({1, 2}, 0)));
        const auto summary = metricSampleSummary(Json::array({nullptr, true, false, nan, inf, 0., 2.}));
        QCOMPARE(summary.at("valid_samples").get<int>(), 2);
        QCOMPARE(summary.at("mean").get<double>(), 1.);
    }
    void orderedChoices() {
        const auto grouped = groupMetricChoices(Json::array({{2}, {1}, {1, 2}}));
        QVERIFY(grouped == Json::array({{{"metrics", {0}}, {"compatible_groups", {2}}},
                                        {{"metrics", {1, 2}}, {"compatible_groups", {1}}}}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, groupMetricChoices(Json::array({{true}})));
    }
    void assemblyIdentity() {
        const Json input = Json::array({Json::array({{{"metric", UINT64_MAX}, {"values", {1, 2}}}}),
                                        Json::array({{{"metric", UINT64_MAX}, {"values", {3, 4}}}})});
        const auto result = assembleMetricIterations(input);
        QVERIFY(result[0]["ranges"] == Json::array({{1., 3.}, {2., 4.}}));
        auto corrupt = input;
        corrupt[1][0]["metric"] = true;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, assembleMetricIterations(corrupt));
    }
    void publisherUsesConvertedSamples() {
        Json profile = {{"sample_count", 2},          {"sample_schedule", "metric_set_then_iteration"},
                        {"selection_mode", "events"}, {"selected_events", {7}},
                        {"records", Json::array()},   {"validation", {{"passes", Json::array()}}}};
        const Json descriptor = {{"name", "GpuTime"}, {"label", "GPU Time"}, {"unit", "ns"}};
        profile["sets"] = Json::array({{{"name", "A"}, {"metrics", Json::array({descriptor})}}});
        for (int i = 0; i < 2; ++i) {
            profile["validation"]["passes"].push_back({{"pass_index", i},
                                                       {"sample_index", i},
                                                       {"set", "A"},
                                                       {"image_matches", true},
                                                       {"event_mapping_valid", true}});
            profile["records"].push_back({{"pass_index", i},
                                          {"sample_index", i},
                                          {"set", "A"},
                                          {"event", 7},
                                          {"available", true},
                                          {"values", Json::array({{{"type", 1}, {"value", 999 + i}}})}});
        }
        const auto original = profile;
        Json publisher = {{"records", profile["records"]}};
        publisher["records"][0]["values"][0]["value"] = 0.;
        publisher["records"][1]["values"][0]["value"] = 1.;
        const auto analysis = publisherMetricAnalysis(profile, publisher);
        QCOMPARE(analysis["statistics"][0]["mean"].get<double>(), .5);
        QCOMPARE(analysis["statistics"][0]["variation_percent"].get<double>(), 100.);
        QVERIFY(analysis["statistics"][0]["unit"] == "us");
        QVERIFY(profile == original);
        profile["records"][0]["available"] = false;
        QCOMPARE(publisherMetricAnalysis(profile, publisher)["statistics"][0]["valid_samples"].get<int>(), 1);
        publisher["records"][0]["event"] = 8;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, publisherMetricAnalysis(profile, publisher));
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
    MetricAnalysisTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricAnalysisTests.moc"
