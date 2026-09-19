#include "application/Experiment.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
class ExperimentTests final : public QObject {
    Q_OBJECT
  private slots:
    void historyAndCompatibility() {
        auto directory = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (directory.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::Frame frame(std::filesystem::path(
            (directory + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString()));
        flora::Experiment project(frame);
        flora::ReplayOptions options;
        project.setEnabled(frame, 181, false);
        QVERIFY(!project.enabled(181));
        project.apply(frame, options);
        QVERIFY(options.disabled.contains(181));
        QVERIFY(project.undo());
        project.apply(frame, options);
        QVERIFY(options.disabled.empty());
        QVERIFY(project.redo());
        QTemporaryDir temp;
        auto path = temp.path() + "/experiment.json";
        project.save(path);
        flora::Experiment loaded(frame);
        loaded.load(path, frame);
        QVERIFY(!loaded.enabled(181));
        QCOMPARE(loaded.document(), project.document());
        auto document = loaded.document();
        document["large_metadata_id"] = UINT64_MAX;
        auto bytes = QByteArray::fromStdString(document.dump());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        file.close();
        loaded.load(path, frame);
        QCOMPARE(loaded.document().at("large_metadata_id").get<uint64_t>(), UINT64_MAX);
        loaded.save(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("18446744073709551615"));
        file.close();
        document["frame_sha256"] = std::string(64, '0');
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArray::fromStdString(document.dump()));
        file.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loaded.load(path, frame));
        QVERIFY(!loaded.enabled(181));
        loaded.undo();
        loaded.setEnabled(frame, 219, false);
        QVERIFY(!loaded.canRedo());
        QVERIFY(loaded.enabled(181));
        QVERIFY(!loaded.enabled(219));
    }
};
QTEST_GUILESS_MAIN(ExperimentTests)
#include "ExperimentTests.moc"
