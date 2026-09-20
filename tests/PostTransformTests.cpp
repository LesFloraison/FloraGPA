#include "DepthStencilCapture.h"
#include "StreamCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "application/PostTransform.h"
#include "application/SessionUi.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class PostTransformTests final : public QObject {
    Q_OBJECT
    void projectFile(MainWindow &window, const char *action, const QString &path) {
        bool handled = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->selectFile(path);
            handled = true;
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>(action)->trigger();
        QVERIFY(handled);
    }
  private slots:
    void geometryProjectSelection() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/geometry.gpa_frame";
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        using Json = nlohmann::json;
        const Json original{{"geometry_selection",
                             {{"stage", "GS"},
                              {"stream", "3"},
                              {"instance", "4294967295"},
                              {"ia_table", "唯一顶点"},
                              {"future", 42}}}};
        auto state = replayUiState(frame, original);
        QCOMPARE(state.geometryStage, std::string("gs"));
        QCOMPARE(state.geometryStream, 3u);
        QCOMPARE(state.geometryInstance, std::string("4294967295"));
        QCOMPARE(state.geometryTable, std::string("unique_vertices"));
        auto saved = replayUiDocument(frame, state, original);
        QVERIFY(saved.at("geometry_selection") == original.at("geometry_selection"));
        state.geometryStage = "ia";
        QCOMPARE(replayUiState(frame, replayUiDocument(frame, state)).geometryStage, std::string("ia"));
        for (const auto *invalid : {"-1", "4294967296", "x", "1.2", "+1"}) {
            state.geometryInstance = invalid;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayUiDocument(frame, state));
        }
    }
    void outputIsolationAndFailure() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/counter.gpa_frame";
        graphicsCounterCapture(true).save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions o;
            o.warp = warp;
            o.until = 200;
            o.before = true;
            o.shaders[30] = compileClassProgram(
                "RWStructuredBuffer<uint> data:register(u1);"
                "float4 main(uint id:SV_VertexID):SV_Position {uint n=data.IncrementCounter();data[n]=100;"
                "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],n/4.,1);}",
                "vs_5_0");
            Replay replay(frame, o);
            replay.run();
            const auto storage = replay.readBuffer(10), target = replay.readTexture(20);
            const auto count = replay.readCounter(12);
            const auto commands = replay.counts;
            QCOMPARE(count, 0u);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                const auto geometry = inspectPostTransform(replay, 200);
                QCOMPARE(geometry.report.at("vertices").get<unsigned>(), 3u);
                QVERIFY(geometry.report.at("pre_raster_uav_isolation").at("stages") ==
                        nlohmann::json::array({"vs"}));
                QCOMPARE(replay.readBuffer(10), storage);
                QCOMPARE(replay.readTexture(20), target);
                QCOMPARE(replay.readCounter(12), count);
                QVERIFY(replay.counts == commands);
            }
            PostTransformOptions bounded;
            bounded.maxBytes = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 200, bounded));
            QCOMPARE(replay.readBuffer(10), storage);
            QCOMPARE(replay.readCounter(12), count);
            QCOMPARE(inspectPostTransform(replay, 200).bytes.size(), size_t(48));
            // Native state is usable immediately after inspection, without a replay reset.
            replay.inspectNativeState([](auto context, const auto &) { context->Draw(3, 0); });
            const auto after = replay.readBuffer(10);
            const auto afterCount = replay.readCounter(12);
            o.before = false;
            Replay baseline(frame, o);
            baseline.run();
            QCOMPARE(after, baseline.readBuffer(10));
            QCOMPARE(afterCount, baseline.readCounter(12));
        }
    }
    void streamCursorAndSignatureProvider() {
        QTemporaryDir dir;
        for (bool signature : {false, true})
            for (bool dirty : {false, true}) {
                const auto path = dir.path() + "/so.gpa_frame";
                streamCapture(signature, dirty).save(path);
                Frame frame(path.toStdWString());
                for (bool warp : {false, true}) {
                    ReplayOptions o;
                    o.warp = warp;
                    o.until = 150;
                    o.before = true;
                    Replay replay(frame, o);
                    replay.run();
                    const auto before = replay.readBuffer(70);
                    for (unsigned i = 0; i < 2; ++i) {
                        auto geometry = inspectPostTransform(replay, 150);
                        QCOMPARE(geometry.bytes.size(), size_t(48));
                        QCOMPARE(replay.readBuffer(70), before);
                    }
                    // A direct producer after the helper must resume the original hidden cursor.
                    replay.inspectNativeState(
                        [&](auto context, const auto &) { context->Draw(signature ? 3 : 1, 0); });
                    auto actual = replay.readBuffer(70);
                    o.before = false;
                    Replay baseline(frame, o);
                    baseline.run();
                    QCOMPARE(actual, baseline.readBuffer(70));
                }
            }
    }
    void overflowFailureRestoresState() {
        QTemporaryDir dir;
        auto capture = streamCapture();
        for (const auto &entry : capture.entries)
            if (entry.id == 100)
                put(capture.bytes, entry.offset + 24, 1025u);
        const auto path = dir.path() + "/overflow.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions options;
            options.warp = warp;
            options.until = 100;
            options.before = true;
            options.shaders[30] =
                compileClassProgram("float4 main():SV_Position{return float4(0,0,0,1);}", "vs_5_0");
            Replay replay(frame, options);
            replay.run();
            const auto before = replay.readBuffer(70);
            PostTransformOptions bounded;
            bounded.maxBytes = 1024 * 16;
            // The first draw fills the private buffer; the expansion then exceeds the limit.
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 100, bounded));
            QCOMPARE(replay.readBuffer(70), before);
            const auto geometry = inspectPostTransform(replay, 100);
            QCOMPARE(geometry.report.at("vertices").get<unsigned>(), 3075u);
            QCOMPARE(geometry.report.at("attempts").size(), size_t(2));
            QCOMPARE(replay.readBuffer(70), before);
            replay.inspectNativeState([](auto context, const auto &) { context->Draw(1025, 0); });
            const auto actual = replay.readBuffer(70);
            options.before = false;
            Replay baseline(frame, options);
            baseline.run();
            QCOMPARE(actual, baseline.readBuffer(70));
        }
    }
    void emptyAndNonfiniteExports() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/depth.gpa_frame";
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        ReplayOptions o;
        o.warp = true;
        o.until = 1000;
        o.before = true;
        Replay replay(frame, o);
        replay.run();
        auto geometry = inspectPostTransform(replay, 1000);
        const auto destination = dir.path().toStdWString();
        exportPostTransform(geometry, destination);
        QVERIFY(QFile::exists(dir.path() + "/geometry.obj"));
        o.shaders[30] = compileClassProgram(
            "float4 main(uint id:SV_VertexID):SV_Position {return float4(id,0,0,0);}", "vs_5_0");
        Replay zeroW(frame, o);
        zeroW.run();
        geometry = inspectPostTransform(zeroW, 1000);
        QVERIFY(!geometry.report.at("obj_unavailable_reason").is_null());
        auto tables = postTransformTables(geometry);
        QVERIFY(!tables.at("obj_unavailable_reason").is_null());
        exportPostTransform(geometry, destination);
        QVERIFY(!QFile::exists(dir.path() + "/geometry.obj"));
        QVERIFY(QFile::exists(dir.path() + "/vertices.bin"));
        o.disabled.insert(1000);
        Replay disabled(frame, o);
        disabled.run();
        auto empty = inspectPostTransform(disabled, 1000);
        QVERIFY(empty.bytes.empty());
        QCOMPARE(empty.report.at("vertices").get<unsigned>(), 0u);
        QVERIFY(!empty.report.at("enabled").get<bool>());
    }
    void mainWindowInspection() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/depth.gpa_frame";
        depthStencilCapture().save(path);
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            const auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto stage = window.findChild<QComboBox *>("geometryStage");
        stage->setCurrentIndex(1);
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto table = window.findChild<QTableView *>("geometryTable");
        QCOMPARE(table->model()->rowCount(), 3);
        QCOMPARE(table->model()->headerData(0, Qt::Horizontal).toString(), QString("vertex"));
        window.findChild<QLineEdit *>("geometryInstance")->setText("0");
        QCOMPARE(table->model()->rowCount(), 0);
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(table->model()->rowCount(), 3);
        auto mesh = window.findChild<MeshView *>("iaMesh");
        QVERIFY(mesh);
        const auto rendered = mesh->grab().toImage();
        int minX = rendered.width(), maxX = -1;
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x) {
                auto color = rendered.pixelColor(x, y);
                if (color.blue() > 180 && color.green() > 140 && color.red() < 160) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                }
            }
        QVERIFY2(maxX - minX > rendered.height() / 2, "Triangle preview must include horizontal edges");
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            dialog->setDirectory(dir.path());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>("exportGeometry")->trigger();
        for (const auto *file : {"geometry.json", "vertices.bin", "vertices.csv", "geometry.obj"})
            QVERIFY(QFile::exists(dir.path() + "/FloraGPA-Geometry-1000/" + file));
        const auto evidence = qEnvironmentVariable("FLORA_POST_TRANSFORM_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(window.grab().save(evidence + "/post-transform.png"));
        }
        stage->setCurrentIndex(0);
        QCOMPARE(table->model()->rowCount(), 0);
        QVERIFY(!window.findChild<QLineEdit *>("geometryInstance")->isEnabled());
        stage->setCurrentIndex(stage->findData("gs"));
        window.findChild<QSpinBox *>("geometryStream")->setValue(3);
        window.findChild<QLineEdit *>("geometryInstance")->setText("2");
        const auto project = dir.path() + "/geometry.json";
        projectFile(window, "saveExperiment", project);
        QFile saved(project);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto document = nlohmann::json::parse(saved.readAll().toStdString());
        QCOMPARE(document.at("ui").at("geometry_selection").at("stage"), nlohmann::json("GS"));
        saved.close();
        stage->setCurrentIndex(0);
        window.findChild<QSpinBox *>("geometryStream")->setValue(0);
        window.findChild<QLineEdit *>("geometryInstance")->clear();
        tasks.clear();
        projectFile(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(stage->currentData().toString(), QString("gs"));
        QCOMPARE(window.findChild<QSpinBox *>("geometryStream")->value(), 3);
        QCOMPARE(window.findChild<QLineEdit *>("geometryInstance")->text(), QString("2"));
        QCOMPARE(table->model()->rowCount(), 0);
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("FloraGPA-PostTransformTests");
    app.setApplicationName("FloraGPA-PostTransformTests");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    applyAppearance(app);
    PostTransformTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "PostTransformTests.moc"
