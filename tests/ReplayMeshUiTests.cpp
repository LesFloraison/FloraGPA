#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/ReplayDebugView.h"
#include "app/ReplayMeshView.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <qfloat16.h>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
QByteArray readBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read mesh fixture");
    return f.readAll();
}
Json read(const QString &path) { return Json::parse(readBytes(path).toStdString()); }
QAction *action(QWidget &w, const char *name) { return w.findChild<QAction *>(name); }
} // namespace
class ReplayMeshUiTests : public QObject {
    Q_OBJECT
    QString output_;
  private slots:
    void bf1MeshToVertexDebug() {
        const auto root = qEnvironmentVariable("FLORA_DX12_BF1_VERTEX_ROOT");
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty() || captures.isEmpty()) QSKIP("Set BF1 capture and mesh evidence directories");
        MainWindow window; window.resize(1600, 1000); window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(captures + "/bf1_2025_12_10__14_29_55.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("36560"); QTest::keyClick(locate, Qt::Key_Return);
        auto geometry = window.findChild<ReplayMeshView *>();
        auto stage = geometry->findChild<QComboBox *>("replayMeshStage");
        auto table = geometry->findChild<QTableView *>("replayMeshPositions");
        ReplayDebugView *debug{};
        for (auto candidate : window.findChildren<ReplayDebugView *>())
            if (candidate->findChild<QLineEdit *>("debugVertex")) debug = candidate;
        QVERIFY(debug);
        done.clear();
        for (const auto input : {true, false}) {
            const auto directory = root + (input ? "/bf1-input-half1" : "/bf1-particle-vsout2");
            const auto report = read(directory + "/result.json");
            stage->setCurrentIndex(stage->findData(input ? "VSIn" : "VSOut"));
            if (input) geometry->findChild<QLineEdit *>("replayMeshAttribute")->setText("1");
            QVERIFY(geometry->finish(geometry->requestId(), report, directory));
            QVERIFY(!action(*geometry, "replayMeshDebugVertex")->isEnabled());
            table->selectRow(1428);
            QVERIFY(action(*geometry, "replayMeshDebugVertex")->isEnabled());
            QSignalSpy selected(geometry, &ReplayMeshView::vertexDebugRequested);
            action(*geometry, "replayMeshDebugVertex")->trigger();
            QCOMPARE(selected.size(), 1);
            QCOMPARE(selected[0][0].toULongLong(), 36560ULL);
            QCOMPARE(selected[0][1].toUInt(), 3966U); // Vertex 1428 appears at ordinals 3966 and 3969.
            QCOMPARE(debug->request().at("vertex"), Json(3966));
            QCOMPARE(debug->request().at("instance"), Json(0));
            QCOMPARE(debug->request().at("execution"), Json("36560"));
            QCOMPARE(window.findChild<QTabWidget *>("checkpointStageTabs")->currentWidget(), debug);
            geometry->setWorkerBusy(true); QVERIFY(!action(*geometry, "replayMeshDebugVertex")->isEnabled());
            geometry->setWorkerBusy(false); QVERIFY(action(*geometry, "replayMeshDebugVertex")->isEnabled());
            if (!input) {
                auto wrong = report; wrong["instance"] = 1;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, geometry->finish(geometry->requestId(), wrong, directory));
                auto legacy = report; legacy.erase("draw_parameters"); legacy.erase("instance");
                QVERIFY(geometry->finish(geometry->requestId(), legacy, directory)); table->selectRow(1428);
                QVERIFY(!action(*geometry, "replayMeshDebugVertex")->isEnabled());
            }
        }
        QVERIFY(done.empty()); QVERIFY(!window.busy()); // Navigation prepares invocation; it does not launch replay.
        locate->setText("36561"); QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(!action(*geometry, "replayMeshDebugVertex")->isEnabled());
    }
    void bf1VertexInvocation() {
        const auto root = qEnvironmentVariable("FLORA_DX12_BF1_VERTEX_ROOT");
        if (root.isEmpty()) QSKIP("Set the BF1 input/vertex invocation evidence directory");
        const auto directory = root + "/bf1-input-half1";
        const auto mesh = read(directory + "/result.json");
        const auto debug = read(root + "/bf1-vertex-invocation1/result.json");
        QVERIFY(mesh.at("ok").get<bool>()); QVERIFY(debug.at("ok").get<bool>());
        const auto &invocation = debug.at("vertex_invocation");
        QCOMPARE(invocation.at("vertex_ordinal"), Json(3966));
        QCOMPARE(invocation.at("actual_vertex_index"), Json(1428));
        QCOMPARE(invocation.at("index_byte_offset"), Json("7932"));
        QCOMPARE(mesh.at("index_count"), Json(4974));
        const auto &input = mesh.at("mesh").at("input");
        QCOMPARE(input.at("index_width"), Json(2));
        QCOMPARE(input.at("first_vertex"), Json("0"));
        QCOMPARE(input.at("layouts")[1].at("format").at("compByteWidth"), Json(2));
        const auto rawIndices = readBytes(directory + "/input_indices.bin");
        uint16_t index{}; memcpy(&index, rawIndices.constData() + 7932, 2); QCOMPARE(index, uint16_t(1428));
        const auto rawVertices = readBytes(directory + "/input_vertices.bin");
        const auto vertices = readBytes(directory + "/post_vertices.bin");
        const auto &inputs = debug.at("trace").at("inputs");
        const auto v1 = std::find_if(inputs.begin(), inputs.end(), [](const Json &v) { return v.at("name") == "v1"; });
        QVERIFY(v1 != inputs.end());
        for (size_t i = 0; i < 4; ++i) {
            qfloat16 half; memcpy(&half, rawVertices.constData() + 1428 * 80 + i * 2, 2);
            float converted; memcpy(&converted, vertices.constData() + 1428 * 16 + i * 4, 4);
            QCOMPARE(converted, float(half));
            QCOMPARE(converted, v1->at("value").at("f32v")[i].get<float>());
        }
        ReplayMeshView geometry; geometry.resize(1200, 850); geometry.show();
        geometry.setContext("bf1-input", 36560, true);
        auto stage = geometry.findChild<QComboBox *>("replayMeshStage");
        stage->setCurrentIndex(stage->findData("VSIn"));
        geometry.findChild<QLineEdit *>("replayMeshAttribute")->setText("1");
        QVERIFY(geometry.finish(geometry.requestId(), mesh, directory));
        auto table = geometry.findChild<QTableView *>("replayMeshPositions");
        QCOMPARE(table->model()->rowCount(), 3316);
        QCOMPARE(table->model()->index(1428, 0).data().toString(), QString("1428"));
        QCOMPARE(table->model()->index(1428, 1).data().toString().toDouble(), 0.498046875);
        table->selectRow(1428); table->scrollTo(table->model()->index(1428, 0));
        ReplayDebugView view("vs"); view.resize(1200, 850); view.show();
        view.setContext("bf1-input", 36560, true);
        view.findChild<QLineEdit *>("debugVertex")->setText("3966");
        QVERIFY(view.finish(view.requestId(), debug));
        auto label = view.findChild<QLabel *>("debugGpuInvocationReference");
        QVERIFY(!label->isHidden()); QVERIFY(label->text().contains("ordinal 3966"));
        QVERIFY(label->text().contains("actual vertex 1428")); QVERIFY(label->text().contains("7932"));
        QCOMPARE(Json::parse(label->toolTip().toStdString()), invocation);
        QTemporaryDir temp; geometry.exportResult(temp.filePath("mesh.csv"));
        QCOMPARE(readBytes(temp.filePath("mesh.csv")), readBytes(directory + "/post_vertices.csv"));
        geometry.exportResult(temp.filePath("mesh.vbin"));
        QCOMPARE(readBytes(temp.filePath("mesh.vbin")), rawVertices);
        if (!output_.isEmpty()) {
            QVERIFY(geometry.grab().save(output_ + "/bf1-half-input.png"));
            QVERIFY(view.grab().save(output_ + "/bf1-vertex-invocation.png"));
        }
        const auto serial = view.requestId(); view.setContext("changed", 36560, true);
        QVERIFY(!view.finish(serial, debug)); QVERIFY(label->isHidden());
        auto legacy = debug; legacy.erase("vertex_invocation");
        QVERIFY(view.finish(view.requestId(), legacy)); QVERIFY(label->isHidden());
    }
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPATests");
        QCoreApplication::setApplicationName("ReplayMeshTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty()) {
            output_ += "/replay-mesh";
            QVERIFY(QDir().mkpath(output_));
        }
    }
    void selectionAndTokens() {
        ReplayMeshView view;
        QVERIFY(!action(view, "replayMeshRead")->isEnabled());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        view.setContext("frame", 105);
        QCOMPARE(view.request(),
                 Json({{"action", "postmesh"}, {"gpa_event", 105}, {"stage", "VSOut"}, {"instance", 0}}));
        const auto old = view.requestId();
        auto instance = view.findChild<QLineEdit *>("replayMeshInstance");
        instance->setText("4294967295");
        QCOMPARE(view.request().at("instance"), Json(UINT32_MAX));
        QVERIFY(view.requestId() > old);
        QVERIFY(!view.finish(old, {{"ok", true}}, "missing"));
        for (const auto invalid : {"4294967296", "-1", "1.5", "garbage", ""}) {
            instance->setText(invalid);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        }
        instance->setText("1");
        view.findChild<QComboBox *>("replayMeshStage")->setCurrentIndex(1);
        QCOMPARE(view.request().at("stage"), Json("GSOut"));
        view.setWorkerBusy(true);
        QVERIFY(!action(view, "replayMeshRead")->isEnabled());
        QVERIFY(action(view, "replayMeshCancel")->isEnabled());
        QVERIFY(!instance->isEnabled());
        view.setWorkerBusy(false);
        QVERIFY(action(view, "replayMeshRead")->isEnabled());
        QSignalSpy finished(&view, &ReplayMeshView::inspectionFinished);
        QVERIFY(!view.finish(view.requestId(), {{"ok", false}, {"error", "No mesh"}}, ""));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
    }
    void recordedReportsAndExports() {
        const auto root = qEnvironmentVariable("FLORA_MESH_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_MESH_REPORTS to native/oracle comparison outputs");
        int verified = 0;
        for (const auto &name : QDir(root).entryList({"*-native"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            const auto directory = QDir(root).filePath(name);
            auto report = read(directory + "/result.json");
            if (!report.at("ok").get<bool>())
                continue;
            ReplayMeshView view;
            view.resize(1000, 650);
            view.show();
            view.setContext("recorded", 105);
            if (report.at("stage") == "GSOut")
                view.findChild<QComboBox *>("replayMeshStage")->setCurrentIndex(1);
            const auto token = view.requestId();
            QVERIFY(view.finish(token, report, directory));
            QCOMPARE(view.result(), report);
            auto table = view.findChild<QTableView *>("replayMeshPositions");
            QCOMPARE(table->model()->rowCount(), report.at("vertex_count").get<int>());
            const auto lines = readBytes(directory + "/post_vertices.csv").split('\n');
            for (int row = 0; row < table->model()->rowCount(); ++row) {
                const auto fields = lines[row + 1].trimmed().split(',');
                QCOMPARE(table->model()->columnCount(), fields.size());
                for (int col = 0; col < fields.size(); ++col)
                    QCOMPARE(table->model()->index(row, col).data().toString().toUtf8(), fields[col]);
            }
            QVERIFY(view.findChild<QPlainTextEdit *>("replayMeshDetails")->isHidden());
            action(view, "replayMeshDetailsToggle")->trigger();
            QCOMPARE(Json::parse(
                         view.findChild<QPlainTextEdit *>("replayMeshDetails")->toPlainText().toStdString()),
                     report);
            QTemporaryDir temp;
            for (const auto suffix : {"csv", "obj", "json"}) {
                const auto path = temp.path() + "/mesh." + suffix;
                view.exportResult(path);
                if (QString(suffix) == "json")
                    QCOMPARE(read(path), report);
                else
                    QCOMPARE(readBytes(path),
                             readBytes(directory + (QString(suffix) == "csv" ? "/post_vertices.csv"
                                                                             : "/post_geometry.obj")));
            }
            // Reject an internally inconsistent result without replacing accepted exports.
            auto bad = report;
            bad["vertex_count"] = 999999;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(token, bad, directory));
            QCOMPARE(view.result(), report);
            // Result ownership survives deletion of the worker's temporary artifact directory.
            {
                QTemporaryDir source;
                for (const auto &file : QDir(directory).entryList({"post_*"}, QDir::Files))
                    QVERIFY(QFile::copy(directory + '/' + file, source.path() + '/' + file));
                QVERIFY(view.finish(token, report, source.path()));
            }
            view.exportResult(temp.path() + "/retained.obj");
            QCOMPARE(readBytes(temp.path() + "/retained.obj"), readBytes(directory + "/post_geometry.obj"));
            view.setContext("different-frame", 105);
            QVERIFY(view.result().is_null());
            QCOMPARE(table->model()->rowCount(), 0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.exportResult(temp.path() + "/stale.obj"));
            ++verified;
        }
        QVERIFY(verified >= 10);
    }
    void mainWindow_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void mainWindow() {
        QFETCH(bool, warp);
        const auto capture = qEnvironmentVariable("FLORA_DEBUG_SOURCE_CAPTURE");
        if (capture.isEmpty())
            QSKIP("Set FLORA_DEBUG_SOURCE_CAPTURE to shader_sources/source.gpa_frame");
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        if (adapter->currentIndex() != int(warp)) {
            adapter->setCurrentIndex(int(warp));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
        }
        auto api = window.findChild<QTableView *>("apiLog");
        auto select = [&](qulonglong event) {
            for (int i = 0; i < api->model()->rowCount(); ++i)
                if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == event) {
                    api->setCurrentIndex(api->model()->index(i, 0));
                    return;
                }
            QFAIL("Missing API event");
        };
        select(105);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto view = window.findChild<ReplayMeshView *>();
        QVERIFY(view);
        auto tabs = window.findChild<QTabWidget *>("analysisTabs");
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Geometry")
                tabs->setCurrentIndex(i);
        window.findChild<QTabWidget *>("geometryViews")->setCurrentWidget(view);
        view->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*view, "replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        const auto original = view->result();
        QCOMPARE(original.at("vertex_count"), Json(3));
        QCOMPARE(original.at("face_count"), Json(1));
        if (!output_.isEmpty()) {
            const auto suffix = warp ? QString("warp") : QString("hardware");
            for (const auto ext : {"json", "csv", "obj"})
                view->exportResult(output_ + "/mesh-" + suffix + '.' + ext);
            QVERIFY(window.grab().save(output_ + "/mesh-" + suffix + ".png"));
        }
        // Shared recapture and retained exports across a shader debug worker job.
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-ps");
        debug->selectPixel(48, 32, 0);
        debug->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*debug, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(debug->exportReport().at("capture"), original.at("capture"));
        QCOMPARE(view->result(), original);
        QTemporaryDir temp;
        view->exportResult(temp.path() + "/retained.csv");
        QVERIFY(readBytes(temp.path() + "/retained.csv").startsWith("vertex,x,y,z,w\r\n"));
        action(*view, "replayMeshRead")->trigger();
        action(*view, "replayMeshCancel")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().is_null());
        action(*view, "replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->result().at("capture"), original.at("capture"));
        view->findChild<QComboBox *>("replayMeshStage")->setCurrentIndex(1);
        QVERIFY(view->result().is_null());
        action(*view, "replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        view->findChild<QComboBox *>("replayMeshStage")->setCurrentIndex(0);
        QAction *disable{}, *undo{};
        for (auto a : window.findChildren<QAction *>()) {
            if (a->text() == "Disable Event")
                disable = a;
            if (a->text() == "Undo")
                undo = a;
        }
        QVERIFY(disable && undo);
        disable->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().is_null());
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->result().at("capture"), original.at("capture"));
        // The existing independent geometry inspector remains reachable in its own tab.
        const auto beforeIndependent = view->result();
        QCOMPARE(beforeIndependent.at("mesh"), original.at("mesh"));
        if (!output_.isEmpty()) {
            QFile diff(output_ + (warp ? "/undo-report-diff-warp.json" : "/undo-report-diff-hardware.json"));
            QVERIFY(diff.open(QIODevice::WriteOnly));
            const auto bytes = Json::diff(original, beforeIndependent).dump(2);
            QCOMPARE(diff.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
        }
        // This fixture generates positions from SV_VertexID and has no IA layout.
        auto independentStage = window.findChild<QComboBox *>("geometryStage");
        independentStage->setCurrentIndex(independentStage->findData("vs"));
        action(window, "inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        tasks.clear();
        QCOMPARE(window.findChild<QTabWidget *>("geometryViews")->currentIndex(), 0);
        QCOMPARE(view->result(), beforeIndependent);
        window.findChild<QTabWidget *>("geometryViews")->setCurrentWidget(view);
        select(51);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        action(*view, "replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        select(105);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        view->setBackendPath("C:/missing/renderdoc.dll");
        action(*view, "replayMeshRead")->trigger();
        QCOMPARE(tasks.size(), 1);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        view->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*view, "replayMeshRead")->trigger();
        select(51);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(view->result().is_null());
    }
};
QTEST_MAIN(ReplayMeshUiTests)
#include "ReplayMeshUiTests.moc"
