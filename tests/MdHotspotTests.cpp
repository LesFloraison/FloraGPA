#include "application/Experiment.h"
#include "application/MdHotspots.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <cstring>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing test artifact");
    return Json::parse(f.readAll().toStdString());
}
Json decode(Json v) {
    if (v.is_object() && v.size() == 1 && v.contains("$f")) {
        auto bytes = QByteArray::fromHex(QByteArray::fromStdString(v["$f"].get<std::string>()));
        if (bytes.size() != 8)
            throw std::invalid_argument("Invalid binary64");
        double x;
        std::memcpy(&x, bytes.data(), 8);
        return x;
    }
    if (v.is_structured())
        for (auto &child : v)
            child = decode(std::move(child));
    return v;
}
Json encode(Json v) {
    if (v.is_number_float()) {
        double x = v.get<double>();
        return {{"$f", std::isnan(x)
                           ? "nan"
                           : QByteArray(reinterpret_cast<const char *>(&x), 8).toHex().toStdString()}};
    }
    if (v.is_structured())
        for (auto &child : v)
            child = encode(std::move(child));
    return v;
}
Json run(const Json &j) {
    const auto op = j.at("op");
    if (op == "normalize")
        return normalizeMetricGroups(j.at("groups"), j.at("events"));
    if (op == "aggregate")
        return aggregateMetricProfiles(j.at("profile"), j.at("weights"), j.value("groups", Json()));
    if (op == "publisher")
        return aggregatePublisherMetricProfiles(j.at("profile"), j.at("weights"), j.at("publisher"),
                                                j.at("weight_publisher"), j.value("groups", Json()));
    if (op == "load")
        return loadPublisherMetricAggregates(QString::fromStdString(j.at("directory").get<std::string>()),
                                             j.at("result"));
    if (op == "csv")
        return metricGroupsCsv(j.at("result"));
    throw std::invalid_argument("Unknown probe");
}
} // namespace
class MdHotspotTests : public QObject {
    Q_OBJECT
  private slots:
    void groupDomain() {
        const auto groups = normalizeMetricGroups(
            {{{"name", "overlap A"}, {"events", {9, 3}}}, {{"name", "overlap B"}, {"events", {3}}}}, {3, 9});
        QCOMPARE(groups[0]["events"], Json::array({3, 9}));
        QCOMPARE(groups[1]["events"], Json::array({3}));
        QCOMPARE(normalizeMetricGroups(nullptr, {9, 3})[0]["name"], Json("selected"));
        for (const auto &bad :
             {Json::array(), Json{{"name", "A"}}, Json::array({{{"name", " "}, {"events", {3}}}}),
              Json::array({{{"name", "A"}, {"events", {3, 3}}}}),
              Json::array({{{"name", "A"}, {"events", {true}}}}),
              Json::array({{{"name", "A"}, {"events", {4}}}})})
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, normalizeMetricGroups(bad, {3, 9}));
    }
    void collectionCancellationAndIdentity() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
        if (root.isEmpty() || qEnvironmentVariableIntValue("FLORA_TEST_INTEL_METRICS") != 1)
            QSKIP("Set reference root and Intel metrics opt-in");
        try {
            Frame frame((root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
            QTemporaryDir temp;
            const Json base = {{"events", {113}}, {"samples", 1}, {"warmup", 0}, {"publisher_values", true}};
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                collectMetricGroups(frame, temp.filePath("cancelled"), base, {}, {}, [] { return true; }));
            QVERIFY(!QFile::exists(temp.filePath("cancelled/aggregates.json")));
            auto bad = base;
            bad["groups"] = {{{"name", "bad"}, {"events", {UINT32_MAX}}}};
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     collectMetricGroups(frame, temp.filePath("bad-group"), bad));
            QVERIFY(!QFile::exists(temp.filePath("bad-group/aggregates.json")));
            QVERIFY(read(temp.filePath("bad-group/weights/priority-audit.json"))["lock"]["closed"] == true);
            bool stop{};
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     collectMetricGroups(
                                         frame, temp.filePath("midway"), base, {}, {}, [&] { return stop; },
                                         [&](const Json &) { stop = true; }));
            QVERIFY(!QFile::exists(temp.filePath("midway/aggregates.json")));
            QVERIFY(read(temp.filePath("midway/weights/priority-audit.json"))["lock"]["closed"] == true);
            auto result = collectMetricGroups(frame, temp.filePath("retry"), base);
            QVERIFY(!result["records"].empty());
            QVERIFY(
                loadPublisherMetricAggregates(temp.filePath("retry"), result).contains("publisher_sources"));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     collectMetricGroups(frame, temp.filePath("retry"), base));
            Experiment experiment(frame);
            experiment.save(temp.filePath("experiment.json"));
            QVERIFY_THROWS_EXCEPTION(
                std::invalid_argument,
                collectMetricGroups(frame, temp.filePath("changed"), base, temp.filePath("experiment.json"),
                                    {}, {}, [&](const Json &progress) {
                                        if (progress.at("phase") == "weights") {
                                            QFile f(temp.filePath("changed/experiment.json"));
                                            if (!f.open(QIODevice::Append) || f.write(" ") != 1)
                                                throw std::runtime_error("Cannot change test snapshot");
                                        }
                                    }));
            QVERIFY(!QFile::exists(temp.filePath("changed/aggregates.json")));
            QVERIFY(read(temp.filePath("changed/metrics/priority-audit.json"))["lock"]["closed"] == true);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        Json result = Json::array();
        for (const auto &job : read(args[2]))
            try {
                result.push_back(encode(run(decode(job))));
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        QFile out(args[3]);
        if (!out.open(QIODevice::WriteOnly))
            return 3;
        out.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    MdHotspotTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MdHotspotTests.moc"
