#include "DepthStencilCapture.h"
#include "app/Appearance.h"
#include "app/CoverageView.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include "application/Coverage.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
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
} // namespace
class CoverageUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { applyAppearance(*qobject_cast<QApplication *>(QCoreApplication::instance())); }
    void controlsAndFrozenResults() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        auto frame = std::make_shared<Frame>(path.toStdWString());
        const auto state = frame->state(frame->event(1000).state);
        ReplayOptions options;
        options.until = 1000;
        Replay replay(*frame, options);
        const auto result = captureCoverage(replay, 1000);
        exportCoverage(result, dir.filePath("result").toStdWString());
        CoverageView view;
        view.resize(1000, 650);
        view.show();
        auto capture = view.findChild<QAction *>("captureCoverage");
        QVERIFY(!capture->isEnabled());
        view.setContext(frame, 1000, "original", state);
        auto target = view.findChild<QComboBox *>("coverageTarget");
        QCOMPARE(target->count(), 3);
        QVERIFY(target->findData("rt1") == -1);
        QSignalSpy requests(&view, &CoverageView::readRequested);
        auto layer = view.findChild<QLineEdit *>("coverageLayer");
        layer->setText("4294967296");
        capture->trigger();
        QCOMPARE(requests.size(), 0);
        layer->clear();
        capture->trigger();
        QCOMPARE(requests.size(), 1);
        const auto serial = requests.takeLast()[1].toULongLong();
        QVERIFY(view.finish(serial, result.report, dir.filePath("result")));
        auto image = view.findChild<ImageView *>("coverageImage");
        QCOMPARE(image->image().convertToFormat(QImage::Format_RGBA8888),
                 QImage(dir.filePath("result/overlay.png")).convertToFormat(QImage::Format_RGBA8888));
        QSignalSpy pixels(&view, &CoverageView::pixelRequested);
        QCoreApplication::processEvents();
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(.5, .5)));
        QCOMPARE(pixels.size(), 1);
        QCOMPARE(Json::parse(pixels[0][0].toString().toStdString()), result.report);
        view.exportResult(dir.filePath("coverage.zip"));
        QFile zip(dir.filePath("coverage.zip"));
        QVERIFY(zip.open(QIODevice::ReadOnly));
        const auto bytes = zip.readAll();
        for (auto name : {"coverage.json", "coverage.png", "after_draw.png", "overlay.png"})
            QVERIFY(bytes.contains(name));
        // Export owns the completed bytes, independent of the next worker's directory.
        QFile::remove(dir.filePath("result/overlay.png"));
        view.exportResult(dir.filePath("retained.zip"));
        capture->trigger();
        const auto stale = requests.takeLast()[1].toULongLong();
        view.setContext(frame, 1000, "edited", state);
        QVERIFY(!view.finish(stale, result.report, dir.filePath("result")));
        QVERIFY(image->image().isNull());
        QVERIFY(!view.findChild<QAction *>("exportCoverage")->isEnabled());
        target->setCurrentIndex(target->findData("depth"));
        auto changed = state;
        changed.dsv = 0;
        changed.rtCount = 8;
        changed.omStart = 1;
        changed.rtv[7] = 999;
        view.setContext(frame, 1000, "bindings", changed);
        QCOMPARE(target->count(), 2);
        QCOMPARE(target->currentData().toString(), QString("auto"));
    }
    void workerSelectionFailureAndCancel() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto view = window.findChild<CoverageView *>();
        QVERIFY(view);
        auto tabs = window.findChild<QTabWidget *>("analysisTabs");
        tabs->setCurrentWidget(view);
        select(window, 1000);
        auto capture = view->findChild<QAction *>("captureCoverage");
        QVERIFY(capture->isEnabled());
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = view->findChild<ImageView *>("coverageImage");
        QVERIFY(!image->image().isNull());
        emit image->pixelSelected(0, 0, image->image().pixelColor(0, 0));
        const auto history = window.findChild<PixelHistoryView *>()->request();
        QCOMPARE(history["resource"], Json(20));
        QCOMPARE(history["gpa_event"], Json(1000));
        QAction *disable = nullptr, *undo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->text() == "Disable Event")
                disable = action;
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
        }
        QVERIFY(disable);
        QVERIFY(undo);
        disable->trigger();
        QVERIFY(image->image().isNull());
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(view->findChild<QLabel *>("coverageSummary")->text().endsWith("0 covered"));
        undo->trigger();
        QVERIFY(image->image().isNull());
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(view->findChild<QLabel *>("coverageSummary")->text().endsWith("1 covered"));
        auto layer = view->findChild<QLineEdit *>("coverageLayer");
        layer->setText("1");
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QVERIFY(image->image().isNull());
        QVERIFY(capture->isEnabled());
        layer->clear();
        view->findChild<QComboBox *>("coverageMode")->setCurrentIndex(1);
        view->findChild<QCheckBox *>("coverageDepth")->setChecked(false);
        done.clear();
        capture->trigger();
        QVERIFY(window.busy());
        auto cancel = view->findChild<QAction *>("cancelCoverage");
        QVERIFY(cancel->isEnabled());
        cancel->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(view->findChild<QLabel *>("coverageSummary")->text(), QString("Cancelled"));
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!image->image().isNull());
        view->exportResult(dir.filePath("geometry.zip"));
        QFile geometry(dir.filePath("geometry.zip"));
        QVERIFY(geometry.open(QIODevice::ReadOnly));
        QVERIFY(geometry.readAll().contains("rasterized_geometry"));
        // Device changes invalidate both the image and its resource navigation.
        window.findChild<QComboBox *>("replayAdapter")->setCurrentIndex(1);
        QVERIFY(image->image().isNull());
        done.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        done.clear();
        capture->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!image->image().isNull());
    }
    void bufferNavigationAndViewport() {
        QTemporaryDir dir;
        const auto path = dir.filePath("buffer.gpa_frame");
        bufferCapture().save(path);
        MainWindow window;
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto view = window.findChild<CoverageView *>();
        select(window, 100);
        done.clear();
        view->findChild<QAction *>("captureCoverage")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = view->findChild<ImageView *>("coverageImage");
        QCOMPARE(image->image().size(), QSize(8, 1));
        done.clear();
        emit image->pixelSelected(2, 0, image->image().pixelColor(2, 0));
        QCOMPARE(window.findChild<QLineEdit *>("bufferOffset")->text(), QString("20"));
        QCOMPARE(window.findChild<QLineEdit *>("bufferLength")->text(), QString("4"));
        QCOMPARE(window.findChild<QComboBox *>("bufferBoundary")->currentIndex(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        const auto viewportPath = dir.filePath("viewport.gpa_frame");
        bufferCapture(true).save(viewportPath);
        auto frame = std::make_shared<Frame>(viewportPath.toStdWString());
        ReplayOptions options;
        options.until = 100;
        Replay replay(*frame, options);
        const auto result = captureCoverage(replay, 100);
        exportCoverage(result, dir.filePath("viewport").toStdWString());
        CoverageView viewport;
        viewport.setContext(frame, 100, "viewport", frame->state(99));
        QSignalSpy requests(&viewport, &CoverageView::readRequested),
            pixels(&viewport, &CoverageView::pixelRequested);
        viewport.findChild<QAction *>("captureCoverage")->trigger();
        QVERIFY(
            viewport.finish(requests.takeLast()[1].toULongLong(), result.report, dir.filePath("viewport")));
        emit viewport.findChild<ImageView *>("coverageImage")->pixelSelected(2, 0, Qt::white);
        QCOMPARE(pixels.size(), 0);
    }
    void realCapturePanel() {
        const auto capture = qEnvironmentVariable("FLORA_COVERAGE_UI_CAPTURE");
        if (capture.isEmpty())
            QSKIP("Set FLORA_COVERAGE_UI_CAPTURE for real-frame UI validation");
        MainWindow window;
        window.resize(1600, 960);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        Frame frame(capture.toStdWString());
        Id event = 0;
        for (const auto &[id, e] : frame.entries())
            if (e.category == 7 && e.type >= 0x37 && e.type <= 0x3d)
                event = std::max(event, id);
        select(window, event);
        auto view = window.findChild<CoverageView *>();
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(view);
        done.clear();
        view->findChild<QAction *>("captureCoverage")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = view->findChild<ImageView *>("coverageImage");
        QVERIFY(!image->image().isNull());
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            QVERIFY(window.grab().save(artifact + "/coverage.png"));
            view->exportResult(artifact + "/coverage.zip");
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setApplicationName("FloraGPA-Coverage-Test");
    app.setOrganizationName("FloraGPA-Tests");
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto box = qobject_cast<QMessageBox *>(widget))
                box->accept();
    });
    dismiss.start(20);
    CoverageUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "CoverageUiTests.moc"
