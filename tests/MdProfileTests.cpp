#include "application/Experiment.h"
#include "application/MdProfile.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing fixture");
    return Json::parse(f.readAll().toStdString());
}
} // namespace
class MdProfileTests : public QObject {
    Q_OBJECT
  private slots:
    void requestDomain() {
        const auto defaults = uniformMetricRequest(Json::object());
        QVERIFY(defaults["sets"] == Json::array({"RenderBasic"}));
        for (const auto key : {"samples", "warmup"})
            for (const auto &value : {Json(-1), Json(101), Json(true), Json(1.), Json("1"), Json()})
                QVERIFY_THROWS_EXCEPTION(std::invalid_argument, uniformMetricRequest({{key, value}}));
        for (const auto &bad :
             {Json{{"samples", 0}}, Json{{"sets", Json::array()}}, Json{{"sets", {"A", "A"}}},
              Json{{"symbols", {"GpuTime"}}}, Json{{"events", {1}}, {"interval", {1, 2}}},
              Json{{"frame_ranges", "all"}, {"interval", {1, 2}}}, Json{{"interval", {1}}}})
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, uniformMetricRequest(bad));
        QVERIFY(uniformMetricRequest({{"sets", nullptr}})["sets"].is_null());
        QVERIFY(uniformMetricRequest({{"sets", Json::array()}, {"symbols", {"GpuTime"}}})["symbols"] ==
                Json::array({"GpuTime"}));
    }
    void cancelledAndInvalidCollection() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
        if (root.isEmpty() || qEnvironmentVariableIntValue("FLORA_TEST_INTEL_METRICS") != 1)
            QSKIP("Set reference root and Intel metrics opt-in");
        try {
            Frame frame((root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
            QTemporaryDir temp;
            const Json base = {
                {"sets", {"RenderBasic"}}, {"frame_ranges", {0}}, {"warmup", 0}, {"publisher_values", true}};
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                collectUniformMetrics(frame, temp.filePath("cancelled"), base, {}, {}, [] { return true; }));
            QVERIFY(!QFile::exists(temp.filePath("cancelled/profile.json")));
            QVERIFY(read(temp.filePath("cancelled/priority-audit.json"))["lock"]["closed"] == true);
            auto bad = base;
            bad["frame_ranges"] = {UINT32_MAX};
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     collectUniformMetrics(frame, temp.filePath("invalid"), bad));
            QVERIFY(!QFile::exists(temp.filePath("invalid/profile.json")));
            QVERIFY(read(temp.filePath("invalid/priority-audit.json"))["lock"]["closed"] == true);
            auto repeats = base;
            repeats["samples"] = 2;
            bool stop = false;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     collectUniformMetrics(
                                         frame, temp.filePath("partial"), repeats, {}, {},
                                         [&] { return stop; }, [&](const Json &) { stop = true; }));
            QVERIFY(QFile::exists(temp.filePath("partial/pass-00/raw-results.json")));
            QVERIFY(!QFile::exists(temp.filePath("partial/profile.json")));
            Frame relative(std::filesystem::relative(frame.path()));
            auto result = collectUniformMetrics(relative, temp.filePath("retry"), base);
            const QFileInfo savedFrame(QString::fromStdString(result["frame"].get<std::string>()));
            QVERIFY(savedFrame.isAbsolute());
            QCOMPARE(savedFrame.canonicalFilePath(),
                     QFileInfo(QString::fromStdWString(frame.path().wstring())).canonicalFilePath());
            QCOMPARE(result["records"].size(), size_t(1));
            QVERIFY(result["validation"]["image_matches"] == true);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     collectUniformMetrics(frame, temp.filePath("retry"), base));
            Experiment experiment(frame);
            experiment.save(temp.filePath("experiment.json"));
            const auto modified = temp.filePath("modified");
            QVERIFY_THROWS_EXCEPTION(
                std::invalid_argument,
                collectUniformMetrics(frame, modified, base, temp.filePath("experiment.json"), {}, {},
                                      [&](const Json &) {
                                          QFile f(modified + "/experiment.json");
                                          if (!f.open(QIODevice::Append) || f.write(" ") != 1)
                                              throw std::runtime_error("Cannot mutate test snapshot");
                                      }));
            QVERIFY(read(modified + "/validation.json")["experiment_matches"] == false);
            QVERIFY(!QFile::exists(modified + "/profile.json"));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        Json results = Json::array();
        for (const auto &request : read(args[2])) {
            try {
                results.push_back(uniformMetricRequest(request));
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        }
        QFile f(args[3]);
        if (!f.open(QIODevice::WriteOnly))
            return 2;
        f.write(QByteArray::fromStdString(results.dump(2)));
        return 0;
    }
    MdProfileTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MdProfileTests.moc"
