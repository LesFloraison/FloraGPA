#include "application/MdIterationResults.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
} // namespace
class MdIterationResultTests : public QObject {
    Q_OBJECT
  private slots:
    void schemaRejectedBeforeFileAccess() {
        const Json base = {{"mode", "recovered_metric_iterations"},
                           {"schema_version", 1},
                           {"value_semantics", "publisher_binary64"},
                           {"requested_samples", 1},
                           {"warmup_count", 0},
                           {"requested_pass", 0},
                           {"actual_iteration_count", 1}};
        for (const auto key : {"schema_version", "requested_samples", "warmup_count", "requested_pass",
                               "actual_iteration_count"}) {
            auto profile = base;
            profile[key] = true;
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     loadScheduledMetricResult("missing-result", profile));
            profile[key] = 1.;
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     loadScheduledMetricResult("missing-result", profile));
        }
    }
    void savedCollections() {
        const auto root = qEnvironmentVariable("FLORA_TEST_SCHEDULED_RESULTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_TEST_SCHEDULED_RESULTS to the saved collection matrix");
        for (const auto name : {"gf2-all-ranges", "gf2-repeat-zero", "gf2-nonzero", "gf2-mapped",
                                "gf2-cached", "bf1-worker", "gf2-experiment"}) {
            const auto path = QDir(root).filePath(name);
            try {
                const auto profile = read(path + "/scheduled-profile.json"), before = profile;
                const auto publisher = loadScheduledMetricResult(path, profile);
                QVERIFY(publisher == read(path + "/publisher-values.json"));
                QVERIFY(profile == before);
            } catch (const std::exception &e) {
                QFAIL(e.what());
            }
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        Json results = Json::array();
        for (const auto &job : read(args[2])) {
            try {
                const auto path = QString::fromStdString(job.at("folder").get<std::string>());
                const auto profile =
                    job.contains("profile") ? job.at("profile") : read(path + "/scheduled-profile.json");
                const auto before = profile;
                auto result = loadScheduledMetricResult(path, profile);
                if (profile != before)
                    throw std::runtime_error("Profile mutated");
                results.push_back({{"publisher", std::move(result)}});
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        }
        QFile out(args[3]);
        if (!out.open(QIODevice::WriteOnly))
            return 2;
        out.write(QByteArray::fromStdString(results.dump(2)));
        return 0;
    }
    MdIterationResultTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MdIterationResultTests.moc"
