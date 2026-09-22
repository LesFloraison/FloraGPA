#include "DepthStencilCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/QuadView.h"
#include "application/Quad.h"
#include "application/ZipArchive.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cstdio>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing test output");
    return Json::parse(file.readAll().toStdString());
}
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write test output");
}
Json report(QuadView &view) {
    return Json::parse(view.findChild<QPlainTextEdit *>("quadReport")->toPlainText().toStdString());
}
void select(MainWindow &window, Id event) {
    auto table = window.findChild<QTableView *>("apiLog");
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const auto index = table->model()->index(row, 0);
        if (index.data(Qt::UserRole).toULongLong() == event) {
            table->setCurrentIndex(index);
            return;
        }
    }
    QFAIL("Event is absent from the API log");
}
testing::Capture bufferCapture(bool viewport = false) {
    using namespace testing;
    Capture c;
    c.add(1, 5, 0x127, statePack(Id(0), Id(0), 0u, 0u));
    auto shader = [&](Id id, uint16_t type, const char *source, const char *profile) {
        auto code = compileClassProgram(source, profile);
        c.add(id, 5, type, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), id + 1));
        auto data = statePack(uint64_t(code.size()));
        data.insert(data.end(), code.begin(), code.end());
        append(data, Id(0));
        c.add(id + 1, 9, 0x81, data);
    };
    shader(10, 0x90,
           "float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,.5,1);}",
           "vs_5_0");
    shader(
        12, 0x92,
        "float4 main(float4 p:SV_Position):SV_Target{if(p.x<2||p.x>=6)discard;return float4(.25,.5,.75,1);}",
        "ps_5_0");
    c.add(1000, 5, 0x83, statePack(Id(0), Id(0), 128u, 0u, 32u, 0u, 0u, 0u, Id(1001)));
    auto data = word(128);
    data.resize(132, 0);
    c.add(1001, 9, 1, data);
    c.add(1002, 5, 0x8d, statePack(Id(0), Id(0), Id(1000), 28u, 1u, 3u, 8u, 0u));
    c.add(20, 9, 0x87, statePack(1u, 0.f, 0.f, 8.f, 4.f, 0.f, 1.f));
    c.add(22, 5, 0x89, statePack(Id(0), Id(0), 3u, 1u, 0u, 0, 0.f, 0.f, 1u, 0u, 0u, 0u));
    State state{};
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.omStart = 64;
    state.rtCount = viewport ? 0 : 1;
    state.rtv[0] = viewport ? 0 : 1002;
    state.viewports = 20;
    state.rasterizer = 22;
    state.stages[0].shader = 10;
    state.stages[4].shader = 12;
    c.add(99, 3, 3, snapshot(state));
    c.add(100, 7, 0x37, statePack(Id(99), Id(0), Id(1), 3u, 0u));
    return c;
}
void fileAction(MainWindow &window, const char *name, const QString &path) {
    bool handled = false;
    QTimer choose;
    QObject::connect(&choose, &QTimer::timeout, &window, [&] {
        auto dialog = window.findChild<QFileDialog *>();
        if (!dialog || !dialog->isVisible())
            return;
        choose.stop();
        dialog->selectFile(path);
        handled = true;
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    choose.start(20);
    window.findChild<QAction *>(name)->trigger();
    QVERIFY(handled);
}
} // namespace
class QuadUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { applyAppearance(*qobject_cast<QApplication *>(QCoreApplication::instance())); }
    void relativeArchivePaths() {
        QTemporaryDir dir;
        const auto path = dir.filePath("quad.zip");
        writeZipArchive(path, {{"data/counts.u32le", {}, QByteArray(4, '\0')}});
        QFile archive(path);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        const auto valid = archive.readAll();
        archive.close();
        QVERIFY(valid.contains("data/counts.u32le"));
        for (const auto *name : {"/absolute", "../escape", "data/../escape", "data//empty", "data/./dot",
                                 "data/", "C:/absolute", "data/name:stream", "data\\name"}) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeZipArchive(path, {{name, {}, "invalid"}}));
            QVERIFY(archive.open(QIODevice::ReadOnly));
            QCOMPARE(archive.readAll(), valid);
            archive.close();
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 writeZipArchive(path, {{"data/a", {}, "one"}, {"data/a", {}, "two"}}));
    }
    void cellsStorageAndFrozenExport() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        auto frame = std::make_shared<Frame>(path.toStdWString());
        ReplayOptions options;
        options.until = 1000;
        Replay replay(*frame, options);
        auto result = captureQuad(replay, 1000);
        exportQuad(result, dir.filePath("result").toStdWString());
        QuadView view;
        view.resize(1000, 650);
        view.show();
        auto capture = view.findChild<QAction *>("captureQuad");
        QVERIFY(!capture->isEnabled());
        const auto state = frame->state(frame->event(1000).state);
        view.setContext(frame, 1000, "initial:0", state);
        QSignalSpy requests(&view, &QuadView::readRequested);
        auto layer = view.findChild<QLineEdit *>("quadLayer");
        for (const auto *invalid : {"-1", "4294967296", "invalid"}) {
            layer->setText(invalid);
            capture->trigger();
            QCOMPARE(requests.size(), 0);
        }
        layer->clear();
        capture->trigger();
        QVERIFY(view.finish(requests.takeLast()[1].toULongLong(), result.report, dir.filePath("result")));
        auto image = view.findChild<ImageView *>("quadImage");
        QCOMPARE(
            image->image().convertToFormat(QImage::Format_RGBA8888),
            QImage(dir.filePath("result/data/quad_counts.png")).convertToFormat(QImage::Format_RGBA8888));
        QCoreApplication::processEvents();
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(.5, .5)));
        QCOMPARE(view.findChild<QLabel *>("quadCell")->text(),
                 QString("Cell (0, 0) · Source (0, 0)–(0, 0) · Count 1"));
        QCOMPARE(report(view).at("experiment_key"), Json("initial:0"));
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            view.exportResult(artifact + "/fixture.zip");
            exportQuad(result, (artifact + "/fixture-native").toStdWString());
        }
        QFile::remove(dir.filePath("result/data/counts.u32le"));
        view.exportResult(dir.filePath("retained.zip"));
        QFile zip(dir.filePath("retained.zip"));
        QVERIFY(zip.open(QIODevice::ReadOnly));
        const auto bytes = zip.readAll();
        for (auto name : {"result.json", "data/counts.u32le", "data/locks.u32le", "data/live.u32le",
                          "data/histogram.u32le", "data/reference.u32le", "data/quad_counts.png"})
            QVERIFY(bytes.contains(name));
        // Preserve the full unsigned value rather than the preview's 8-bit band.
        QByteArray large(4, 0);
        qToLittleEndian<uint32_t>(UINT32_MAX, large.data());
        save(dir.filePath("result/data/counts.u32le"), large);
        result.report["counter_sum"] = UINT32_MAX;
        result.report["histogram_accounting_matches_reference"] = false;
        save(dir.filePath("result/quad.json"), QByteArray::fromStdString(result.report.dump()));
        capture->trigger();
        QVERIFY(view.finish(requests.takeLast()[1].toULongLong(), result.report, dir.filePath("result")));
        emit image->pixelSelected(0, 0, Qt::white);
        QVERIFY(view.findChild<QLabel *>("quadCell")->text().endsWith("Count 4294967295"));
        QCOMPARE(view.findChild<QLabel *>("quadAccounting")->text(), QString("Accounting mismatch"));
        capture->trigger();
        const auto stale = requests.takeLast()[1].toULongLong();
        view.setContext(frame, 1000, "edited:0", state);
        QVERIFY(!view.finish(stale, result.report, dir.filePath("result")));
        QVERIFY(image->image().isNull());
        QVERIFY(!view.findChild<QAction *>("exportQuad")->isEnabled());
        save(dir.filePath("result/data/counts.u32le"), QByteArray(3, 0));
        capture->trigger();
        const auto serial = requests.takeLast()[1].toULongLong();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 view.finish(serial, result.report, dir.filePath("result")));
        view.finish(serial, {{"error", "Truncated result"}});
        QVERIFY(capture->isEnabled());
        QVERIFY(image->image().isNull());
    }
    void bufferAndViewportCoordinates() {
        QTemporaryDir dir;
        for (bool viewport : {false, true}) {
            const auto key = viewport ? "viewport" : "buffer";
            const auto path = dir.filePath(QString(key) + ".gpa_frame");
            bufferCapture(viewport).save(path);
            auto frame = std::make_shared<Frame>(path.toStdWString());
            ReplayOptions options;
            options.until = 100;
            Replay replay(*frame, options);
            const auto result = captureQuad(replay, 100);
            const auto output = dir.filePath(key);
            exportQuad(result, output.toStdWString());
            QuadView view;
            view.setContext(frame, 100, key, frame->state(99));
            QSignalSpy requests(&view, &QuadView::readRequested);
            view.findChild<QAction *>("captureQuad")->trigger();
            QVERIFY(view.finish(requests.takeLast()[1].toULongLong(), result.report, output));
            auto image = view.findChild<ImageView *>("quadImage");
            emit image->pixelSelected(1, 0, Qt::white);
            const auto text = view.findChild<QLabel *>("quadCell")->text();
            QVERIFY2(text.contains(viewport ? "Source (2, 0)–(3, 1)" : "Elements 5–6 · bytes 20–27"),
                     qPrintable(text));
            const auto previous = text;
            emit image->pixelSelected(-1, 0, Qt::white);
            emit image->pixelSelected(9999, 0, Qt::white);
            QCOMPARE(view.findChild<QLabel *>("quadCell")->text(), previous);
        }
    }
    void workerExperimentsSettingsAndCancel() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        MainWindow window;
        window.resize(1600, 960);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto view = window.findChild<QuadView *>();
        QVERIFY(view);
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(view);
        select(window, 1000);
        auto capture = view->findChild<QAction *>("captureQuad");
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(report(*view).at("counter_sum"), Json(1));
        QAction *disable = nullptr, *undo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->text() == "Disable Event")
                disable = action;
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
        }
        QVERIFY(disable && undo);
        disable->trigger();
        QVERIFY(view->findChild<ImageView *>("quadImage")->image().isNull());
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(report(*view).at("counter_sum"), Json(0));
        undo->trigger();
        auto layer = view->findChild<QLineEdit *>("quadLayer");
        layer->setText("1");
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(view->findChild<QLabel *>("quadSummary")->text(), QString("Capture failed"));
        layer->clear();
        done.clear();
        capture->trigger();
        QVERIFY(window.busy());
        auto cancel = view->findChild<QAction *>("cancelQuad");
        QVERIFY(cancel->isEnabled());
        cancel->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(view->findChild<QLabel *>("quadSummary")->text(), QString("Cancelled"));
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(report(*view).at("counter_sum"), Json(1));
        Json settings{{"quad_depth", "none"}, {"quad_target", "depth"}, {"quad_layer", "0"}};
        view->restoreSettings(settings);
        const auto project = dir.filePath("experiment.json");
        fileAction(window, "saveExperiment", project);
        const auto ui = load(project).at("ui");
        for (const auto &item : settings.items())
            QCOMPARE(ui.at(item.key()), item.value());
        view->restoreSettings(Json::object());
        done.clear();
        fileAction(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(view->settings(), settings);
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(report(*view).at("depth_mode"), Json("none"));
        QCOMPARE(report(*view).at("target_kind"), Json("depth"));
        window.findChild<QComboBox *>("replayAdapter")->setCurrentIndex(1);
        QVERIFY(view->findChild<ImageView *>("quadImage")->image().isNull());
        done.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(report(*view).at("driver"), Json("warp"));
        select(window, 2000);
        QVERIFY(view->findChild<ImageView *>("quadImage")->image().isNull());
    }
    void realCapturePanel() {
        const auto path = qEnvironmentVariable("FLORA_QUAD_UI_CAPTURE");
        if (path.isEmpty())
            QSKIP("Set FLORA_QUAD_UI_CAPTURE for real-frame Quad UI validation");
        MainWindow window;
        window.resize(1800, 1050);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        select(window, 113);
        auto view = window.findChild<QuadView *>();
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(view);
        done.clear();
        view->findChild<QAction *>("captureQuad")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(report(*view).at("event_id"), Json(113));
        auto image = view->findChild<ImageView *>("quadImage");
        QVERIFY(!image->image().isNull());
        emit image->pixelSelected(0, 0, Qt::white);
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(artifact + "/quad.png"));
            view->exportResult(artifact + "/quad.zip");
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setApplicationName("FloraGPA-Quad-Test");
    app.setOrganizationName("FloraGPA-Tests");
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto box = qobject_cast<QMessageBox *>(widget))
                box->accept();
    });
    dismiss.start(20);
    QuadUiTests tests;
    try {
        return QTest::qExec(&tests, argc, argv);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Quad UI test exception: %s\n", error.what());
        return 1;
    }
}
#include "QuadUiTests.moc"
