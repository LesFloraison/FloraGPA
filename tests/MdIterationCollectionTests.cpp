#include "application/MdIterations.h"
#include "application/MetricIterations.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open " + path.toStdString());
    return Json::parse(file.readAll().toStdString());
}
} // namespace
class MdIterationCollectionTests : public QObject {
    Q_OBJECT
  private slots:
    void requestDomain() {
        const Json base = {{"symbols", {"CsThreads"}}};
        const auto cfg = scheduledMetricRequest(base);
        QCOMPARE(cfg["samples"], Json(1));
        QCOMPARE(cfg["requested_pass"], Json(metricAllPasses));
        QVERIFY(cfg["frame_ranges"].is_null());
        for (const auto key : {"samples", "warmup"}) {
            for (const Json value : {Json(-1), Json(101), Json(true), Json(1.)}) {
                auto request = base;
                request[key] = value;
                QVERIFY_THROWS_EXCEPTION(std::invalid_argument, scheduledMetricRequest(request));
            }
        }
        for (const Json value :
             {Json(true), Json("1"), Json(), Json(std::numeric_limits<double>::quiet_NaN()),
              Json(std::numeric_limits<double>::infinity())}) {
            auto request = base;
            request["weights"] = Json::array({value});
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, scheduledMetricRequest(request));
        }
    }
    void incompleteAndCancelledArtifacts() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
        if (root.isEmpty() || qEnvironmentVariableIntValue("FLORA_TEST_INTEL_METRICS") != 1)
            QSKIP("Set reference root and Intel metric opt-in");
        try {
            Frame frame((root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const Json base = {{"symbols", {"CsThreads"}}, {"frame_ranges", {0}},
                               {"weights", {1.}},          {"samples", 2},
                               {"requested_pass", 0},      {"warmup", 0}};
            for (bool cancelled : {false, true}) {
                auto request = base;
                request["samples"] = cancelled ? 2 : 0;
                const auto out = dir.filePath(cancelled ? "cancelled" : "empty");
                QVERIFY_THROWS_EXCEPTION(
                    MetricIterationError,
                    collectScheduledMetrics(frame, out, request, {}, {}, [=] { return cancelled; }));
                const auto audit = read(out + "/scheduler-audit.json");
                QVERIFY(audit["protocol"]["complete"] == false);
                QCOMPARE(audit["failure"]["type"], Json("MetricIterationError"));
                QCOMPARE(audit["adapter"]["owned"], Json(0));
                QCOMPARE(audit["adapter"]["native_pool"]["cached"], Json(0));
                QCOMPARE(audit["adapter"]["local_lock_depth"], Json(0));
                QVERIFY(audit["adapter"]["priority_mutex"]["closed"] == true);
                QVERIFY(read(out + "/raw-records.json").empty());
                QVERIFY(QFile::exists(out + "/publisher-values.csv"));
                QVERIFY(!QFile::exists(out + "/scheduled-profile.json"));
            }
            auto request = base;
            request["samples"] = 1;
            const auto retry = collectScheduledMetrics(frame, dir.filePath("retry"), request);
            QVERIFY(retry["protocol"]["complete"] == true);
            QCOMPARE(retry["records"].size(), size_t(1));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     collectScheduledMetrics(frame, dir.filePath("retry"), request));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        Json result = Json::array();
        for (const auto &request : read(args[2])) {
            try {
                result.push_back(scheduledMetricRequest(request));
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
    MdIterationCollectionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MdIterationCollectionTests.moc"
