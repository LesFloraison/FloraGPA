#include "DepthStencilCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include "application/DrawResources.h"
#include "application/FrameOutput.h"
#include <QAction>
#include <QImage>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Capture fixture(bool sparse = false) {
    auto c = depthStencilCapture();
    c.add(99, 5, 0x8c, statePack(Id(0), Id(0), Id(20), 28u, 4u, 0u, 1u, 0u, 0u));
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = sparse ? 4 : 1;
    s.rtv[sparse ? 3 : 0] = 21;
    s.dsv = 722;
    s.rasterizer = 724;
    s.viewports = 22;
    s.stages[0].shader = 30;
    s.stages[4].shader = 32;
    s.stages[4].srv[0] = 99;
    if (sparse) {
        s.stages[0].srv[4] = 99;
        s.stages[4].srv[5] = UINT64_MAX;
    }
    for (auto &e : c.entries)
        if (e.id == 990 || e.id == 1990) {
            const auto payload = snapshot(s);
            std::copy(payload.begin(), payload.end(), c.bytes.begin() + ptrdiff_t(e.offset));
        }
    return c;
}
} // namespace
class DrawResourceTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-DrawResourceTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings().clear();
        applyAppearance(*qApp);
    }
    void inventory() {
        QTemporaryDir dir;
        fixture(true).save(dir.filePath("frame.gpa_frame"));
        Frame frame(dir.filePath("frame.gpa_frame").toStdWString());
        const auto rows = drawResources(frame, 1000);
        const auto report = drawResourceInventory(rows, 1000);
        QCOMPARE(report["counts"]["input_textures"], Json(1));
        QCOMPARE(report["counts"]["input_bindings"], Json(3));
        QCOMPARE(report["counts"]["rtv"], Json(1));
        QCOMPARE(report["counts"]["dsv"], Json(1));
        auto unknown =
            std::find_if(rows.begin(), rows.end(), [](const auto &b) { return b.image.view == UINT64_MAX; });
        QVERIFY(unknown != rows.end());
        QVERIFY(!unknown->error.empty());
        auto output = std::find_if(rows.begin(), rows.end(), [](const auto &b) { return b.kind == "RTV"; });
        QVERIFY(output != rows.end());
        QCOMPARE(output->slot, 3u);
        QCOMPARE(output->image.boundary, ImageBoundary::After);
        ReplayOptions edit;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {7, 1};
        edit.srvEdits[1000][{4, 0}] = desc;
        const auto changed = drawResources(frame, 1000, edit);
        auto invalid = std::find_if(changed.begin(), changed.end(),
                                    [](const auto &b) { return b.key == "in/PS/SRV/0"; });
        QVERIFY(invalid != changed.end());
        QVERIFY(!invalid->error.empty());
        QVERIFY_EXCEPTION_THROWN(
            exportDrawResources(frame, 1000, {}, {{"previews", {"unknown"}}}, dir.path().toStdWString()),
            std::exception);
    }
    void rangesBuffersAndNoRt() {
        QTemporaryDir dir;
        auto capture = stateCapture();
        State state{};
        state.omStart = 8;
        state.stages[4].srv[0] = 6;
        state.stages[5].srv[7] = 23;
        state.csUav[0] = 22;
        state.csUav[63 % 8] = 22;
        state.csExtended[55] = 22;
        capture.add(9000, 3, 3, snapshot(state));
        capture.add(9001, 7, 0x37, statePack(Id(9000), Id(0), Id(1), 3u, 0u));
        capture.save(dir.filePath("frame.gpa_frame"));
        Frame frame(dir.filePath("frame.gpa_frame").toStdWString());
        const auto rows = drawResources(frame, 9001);
        const auto report = drawResourceInventory(rows, 9001);
        QCOMPARE(report["counts"]["rtv"], Json(0));
        QCOMPARE(report["counts"]["dsv"], Json(0));
        QCOMPARE(report["counts"]["uav"], Json(3));
        QCOMPARE(report["counts"]["input_textures"], Json(1));
        QCOMPARE(report["counts"]["input_bindings"], Json(2));
        const auto &texture = rows.front();
        QVERIFY(texture.error.empty());
        QCOMPARE(texture.mipEnd, 1u);
        QCOMPARE(texture.layerEnd, 1u);
        auto buffer =
            std::find_if(rows.begin(), rows.end(), [](const auto &b) { return b.image.view == 23; });
        QVERIFY(buffer != rows.end());
        QVERIFY(buffer->error.empty());
        QVERIFY(!buffer->texture);
        QVERIFY_EXCEPTION_THROWN(
            exportDrawResources(frame, 9001, {}, {{"previews", Json::array({1})}}, dir.path().toStdWString()),
            std::exception);
    }
    void staleThumbnails() {
        ResourceBrowser browser;
        browser.resize(300, 700);
        browser.show();
        DrawResourceBinding b;
        b.key = "in/PS/SRV/0";
        b.kind = "SRV";
        b.stage = "PS";
        b.texture = true;
        b.width = b.height = 32;
        b.image.event = 1;
        b.image.resource = UINT64_MAX;
        browser.setContext("first", {b}, 1);
        browser.select(b.key);
        QVERIFY(!browser.nextPreviews()["previews"].empty());
        browser.setContext("second", {b}, 1);
        browser.acceptPreviews("first", nullptr, "missing"); // No stale file access.
        QCOMPARE(browser.contextKey(), QString("second"));
        QVERIFY(!browser.nextPreviews()["previews"].empty());
        browser.failPreviews("second", "Selection changed");
        QVERIFY(!browser.nextPreviews()["previews"].empty());
    }
    void boundaryPreviews() {
        QTemporaryDir dir;
        fixture().save(dir.filePath("frame.gpa_frame"));
        Frame frame(dir.filePath("frame.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 1000;
        const auto report = exportDrawResources(
            frame, 1000, options, {{"previews", {"in/PS/SRV/0", "out/OM/RTV/0"}}}, dir.path().toStdWString());
        QImage input, output;
        for (const auto &b : report["bindings"]) {
            if (b["key"] != "in/PS/SRV/0" && b["key"] != "out/OM/RTV/0")
                continue;
            QVERIFY2(b.contains("preview"), b.dump().c_str());
            QImage image(dir.filePath(QString::fromStdString(b["preview"])));
            QVERIFY(!image.isNull());
            QVERIFY(image.width() <= 96 && image.height() <= 96);
            (b["role"] == "input" ? input : output) = image;
        }
        QVERIFY(input != output);
        QCOMPARE(input.pixelColor(0, 0).red(), 0);
        Replay baseline(frame, options);
        baseline.run();
        const auto raw = readFrameOutput(baseline, 20, {}, 21).image;
        QCOMPARE(output.pixelColor(0, 0).red(), int(raw.rgba[0]));
    }
    void overlayAndCoordinates() {
        ImageView view;
        view.resize(480, 320);
        view.show();
        QImage base(32, 16, QImage::Format_RGBA8888);
        base.fill(QColor(12, 34, 56));
        view.setImage(base);
        QImage mask(base.size(), QImage::Format_RGBA8888);
        mask.fill(Qt::black);
        mask.setPixelColor(3, 4, Qt::white);
        view.setOverlayMask(mask);
        QVERIFY(view.hasOverlay());
        QCOMPARE(view.image(), base);
        QSignalSpy picked(&view, &ImageView::pixelSelected);
        view.setPixelPicking(true);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                          view.mapFromScene(QPointF(3.5, 4.5)));
        QCOMPARE(picked.count(), 1);
        QCOMPARE(picked[0][0].toInt(), 3);
        QCOMPARE(picked[0][1].toInt(), 4);
        QCOMPARE(qvariant_cast<QColor>(picked[0][2]), QColor(12, 34, 56));
        view.clearOverlay();
        QVERIFY(!view.hasOverlay());
        view.setOverlayMask(QImage(1, 1, QImage::Format_RGBA8888));
        QVERIFY(!view.hasOverlay());
        picked.clear();
        const auto start = view.mapFromScene(QPointF(8.5, 5.5));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(view.viewport(), start + QPoint(40, 20));
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 20));
        QVERIFY(picked.empty());
    }
    void historyQueryAndStaleResult() {
        PixelHistoryView view;
        view.setContext("fixture", 1000);
        QSignalSpy requests(&view, &PixelHistoryView::readRequested);
        view.queryPixel(1000, UINT64_MAX, 3, 4, 1, 2, 0);
        QCOMPARE(requests.count(), 1);
        const auto old = view.requestId();
        QCOMPARE(view.request().at("resource"), Json(UINT64_MAX));
        QCOMPARE(view.request().at("x"), Json(3));
        view.queryPixel(1000, 20, 8, 9, 0, 0, 1);
        QCOMPARE(requests.count(), 2);
        QVERIFY(!view.finish(old, {{"ok", false}, {"error", "old result"}}));
        QCOMPARE(view.request().at("x"), Json(8));
        QCOMPARE(view.request().at("sample"), Json(1));
    }
    void workspaceAndBackendFailure() {
        QTemporaryDir dir;
        fixture().save(dir.filePath("frame.gpa_frame"));
        MainWindow window;
        window.show();
        auto tabs = window.findChild<QTabWidget *>("analysisTabs");
        QVERIFY(tabs);
        QStringList names;
        for (int i = 0; i < tabs->count(); ++i)
            names << tabs->tabText(i);
        QVERIFY(names.contains("Resources"));
        QVERIFY(!names.contains("Output"));
        QVERIFY(!names.contains("Texture"));
        QVERIFY(!names.contains("Coverage"));
        QVERIFY(!names.contains("Pixel History"));
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(dir.filePath("frame.gpa_frame"));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<ResourceBrowser *>()->bindings().empty(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.back()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto tree = window.findChild<QTreeWidget *>("drawResources");
        QVERIFY(tree);
        QCOMPARE(window.findChild<ResourceBrowser *>()->selected()->kind, std::string("RTV"));
        auto history = window.findChild<PixelHistoryView *>();
        history->setBackendPath("missing-renderdoc.dll");
        window.findChild<QAction *>("pickPixelHistory")->setChecked(true);
        auto image = window.findChild<ImageView *>("frameOutput");
        QTRY_VERIFY_WITH_TIMEOUT(!image->image().isNull(), 30000);
        QSignalSpy pixels(image, &ImageView::pixelSelected);
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(.5, .5)));
        QCOMPARE(pixels.count(), 1);
        QTRY_VERIFY(window.findChild<QLabel *>("historySummary")->text().contains("1.45"));
        window.findChild<QAction *>("pickPixelHistory")->setChecked(false);
        window.findChild<QAction *>("toggleResourceCoverage")->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 30000);
        auto browser = window.findChild<ResourceBrowser *>();
        QVERIFY(browser->select("in/PS/SRV/0", true));
        QVERIFY(!image->hasOverlay());
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(browser->select("out/OM/DSV/0", true));
        QVERIFY(browser->select("out/OM/RTV/0", true));
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 30000);
        QCOMPARE(browser->selected()->image.resource, Id(20));
    }
    void realCaptureScreenshots() {
        const auto capture = qEnvironmentVariable("FLORA_RESOURCE_CAPTURE");
        const auto output = qEnvironmentVariable("FLORA_RESOURCE_SCREENSHOTS");
        if (capture.isEmpty() || output.isEmpty())
            QSKIP("Set capture and screenshot output to exercise a real frame");
        QVERIFY(QDir().mkpath(output));
        Frame frame(capture.toStdWString());
        Id selected = 0;
        for (auto id : frame.entryOrder()) {
            const auto &e = frame.entry(id);
            if (e.category != 7 || e.type < 0x37 || e.type > 0x3d)
                continue;
            auto s = frame.state(frame.event(id).state);
            unsigned inputs = 0;
            for (auto srv : s.stages[4].srv)
                inputs += srv != 0;
            if (inputs >= 3 && std::min(s.omStart, s.rtCount) >= 3) {
                selected = id;
                break;
            }
        }
        QVERIFY(selected);
        MainWindow window;
        window.resize(1800, 1000);
        window.show();
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.back()[0].toBool());
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == selected) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY2(tasks.back()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QTest::qWait(500);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 60000);
        auto tree = window.findChild<QTreeWidget *>("drawResources");
        QVERIFY(tree);
        QVERIFY(window.grab().save(output + "/resources.png"));
        auto browser = window.findChild<ResourceBrowser *>();
        auto input = std::find_if(browser->bindings().begin(), browser->bindings().end(), [](const auto &b) {
            return b.role == ResourceRole::Input && b.texture && b.error.empty();
        });
        QVERIFY(input != browser->bindings().end());
        tasks.clear();
        browser->select(input->key, true);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.back()[0].toBool());
        QVERIFY(window.grab().save(output + "/input.png"));
        tasks.clear();
        browser->select("out/OM/RTV/1", true);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.back()[0].toBool());
        window.findChild<QAction *>("toggleResourceCoverage")->setChecked(true);
        auto image = window.findChild<ImageView *>("frameOutput");
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 60000);
        QVERIFY(window.grab().save(output + "/coverage.png"));
        // A later BF1 draw has nonzero depth-tested coverage (the early MRT draw may have none).
        tasks.clear();
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 30072) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 60000);
        QTest::qWait(400);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 60000);
        QVERIFY(window.grab().save(output + "/coverage-visible.png"));
        window.findChild<QAction *>("pickPixelHistory")->setChecked(true);
        window.findChild<PixelHistoryView *>()->setBackendPath("missing-renderdoc.dll");
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(100.5, 100.5)));
        QTRY_VERIFY(window.findChild<QLabel *>("historySummary")->text().contains("1.45"));
        QVERIFY(window.grab().save(output + "/history.png"));
    }
};
QTEST_MAIN(DrawResourceTests)
#include "DrawResourceTests.moc"
