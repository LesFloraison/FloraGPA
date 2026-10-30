#include "DepthStencilCapture.h"
#include "MsaaCapture.h"
#include "TextureEditCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
QAction *action(QWidget &view, const char *name) { return view.findChild<QAction *>(name); }
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read native history UI export");
    return Json::parse(file.readAll().toStdString());
}
Json fixture() {
    Json value{{"available", true},
               {"decoded_color", {{"rgba", {UINT32_MAX, 16777219, 0, 1}}}},
               {"depth", nullptr},
               {"stencil", nullptr},
               {"bytes", "ffffffff030000010000000001000000"}};
    return {{"ok", true},
            {"context", {{"action", "history"}, {"options", {{"history_scope", "events"}}}}},
            {"data",
             {{"schema", 2},
              {"kind", "pixel_event_history"},
              {"source", "native_dx11_replay"},
              {"fragment_history_available", false},
              {"selection",
               {{"resource", 20},
                {"x", 0},
                {"y", 0},
                {"slice", 0},
                {"mip", 0},
                {"layer", 0},
                {"sample", 0},
                {"typed_format", nullptr}}},
              {"records",
               Json::array({{{"event", uint64_t(1) << 40},
                             {"api", "Draw"},
                             {"boundary", "submission_after_binding"},
                             {"record_kind", "event_snapshot"},
                             {"footprint", {{"coverage", "potential"}}},
                             {"fragment", nullptr},
                             {"shader_output", nullptr},
                             {"failure_reasons", nullptr},
                             {"write_executed", nullptr},
                             {"before", {{"available", false}, {"reason", "resource_not_materialized"}}},
                             {"after", value}}})}}}};
}
} // namespace
testing::Capture volumeCapture() {
    auto capture = testing::textureEditCapture(28, 1, true, false, 0x86);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
    view.Texture3D = {1, 1, 1};
    capture.add(25, 5, 0x8d, testing::statePack(Id(0), Id(0), Id(20), view));
    capture.add(50, 7, 0x32, testing::statePack(Id(0), Id(1), Id(25), uint8_t(1), 1.f, 0.f, 1.f, 1.f));
    return capture;
}
class NativeHistoryUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-NativeHistoryTests");
        QSettings().clear();
        QSettings().setValue("analysis/renderdoc", "C:/missing/renderdoc.dll");
        applyAppearance(*qApp);
    }
    void presentation() {
        PixelHistoryView view;
        view.setNativeMode(true);
        view.setBackendPath("C:/missing/renderdoc.dll", false);
        view.setContext("frame", 100);
        view.selectPixel(20, 0, 0, 0, 0, 0);
        QVERIFY(!action(view, "historyBackend")->isEnabled());
        QVERIFY(!view.findChild<QLineEdit *>("historyEvent")->isEnabled());
        QVERIFY(!view.request().contains("gpa_event"));
        const auto result = fixture();
        QVERIFY(view.finishNative(view.requestId(), result));
        auto tree = view.findChild<QTreeWidget *>("historyRecords");
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto item = tree->topLevelItem(0);
        QCOMPARE(item->text(5), QString("4294967295, 16777219, 0, 1"));
        QVERIFY(item->text(3).contains("execution unknown"));
        QVERIFY(item->text(4).contains("resource_not_materialized"));
        QCOMPARE(item->text(2), QString("Event snapshot"));
        QVERIFY(view.findChild<QLabel *>("historySnapshotValues")->text().contains("4294967295, 16777219"));
        tree->setCurrentItem(item);
        QCOMPARE(Json::parse(view.findChild<QPlainTextEdit *>("historyDetails")->toPlainText().toStdString()),
                 result["data"]["records"][0]);
        QSignalSpy locate(&view, &PixelHistoryView::eventRequested);
        action(view, "locateHistoryEvent")->trigger();
        QCOMPARE(locate.size(), 1);
        QCOMPARE(locate[0][0].toULongLong(), qulonglong(1) << 40);
        QTemporaryDir directory;
        view.exportResult(directory.filePath("export.json"));
        QCOMPARE(read(directory.filePath("export.json")), result);
        for (int mutation = 0; mutation < 5; ++mutation) {
            auto bad = result;
            if (mutation == 0)
                bad["data"]["selection"]["sample"] = 1;
            if (mutation == 1)
                bad["data"]["records"][0]["fragment"] = 0;
            if (mutation == 2)
                bad["data"]["records"][0]["write_executed"] = 1;
            if (mutation == 3)
                bad["data"]["records"][0]["event"] = -1;
            if (mutation == 4)
                bad["context"]["options"]["history_scope"] = "private-draw";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finishNative(view.requestId(), bad));
            QCOMPARE(tree->topLevelItem(0)->text(5), item->text(5));
        }
        const auto serial = view.requestId();
        view.setContext("changed-experiment", 100);
        QVERIFY(!view.finishNative(serial, result));
        QVERIFY(!action(view, "exportPixelHistory")->isEnabled());
        view.setWorkerBusy(true);
        QVERIFY(!action(view, "historyNativeEvents")->isEnabled());
        view.setWorkerBusy(false);
        view.setNativeMode(false);
        QVERIFY(view.request().contains("gpa_event"));
        QVERIFY(!view.request().contains("history_scope"));
        QVERIFY(!view.finishNative(view.requestId(), result));
    }
    void mainWindow_data() {
        QTest::addColumn<unsigned>("vendor");
        QTest::addColumn<int>("kind");
        for (unsigned vendor : {0u, 0x10deu, 0x8086u})
            for (int kind = 0; kind < 4; ++kind)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(kind))) << vendor << kind;
    }
    void mainWindow() {
        QFETCH(unsigned, vendor);
        QFETCH(int, kind);
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        (kind == 1   ? testing::msaaOutputCapture()
         : kind == 3 ? volumeCapture()
                     : testing::depthStencilCapture())
            .save(capture);
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QStringList errors;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, [&] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(widget)) {
                    errors << box->text();
                    box->reject();
                }
        });
        dismiss.start(25);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        const auto index = vendor ? adapter->findData(vendor) : 1;
        QVERIFY(index >= 0);
        adapter->setCurrentIndex(index);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto view = window.findChild<PixelHistoryView *>();
        QVERIFY(view);
        for (auto tabs : window.findChildren<QTabWidget *>())
            if (tabs->indexOf(view) >= 0)
                tabs->setCurrentWidget(view);
        QVERIFY(view->isVisible());
        for (auto parent = view->parentWidget(); parent; parent = parent->parentWidget())
            if (auto dock = qobject_cast<QDockWidget *>(parent)) {
                qobject_cast<QMainWindow *>(dock->parentWidget())->resizeDocks({dock}, {720}, Qt::Horizontal);
                break;
            }
        view->setNativeMode(true);
        view->setBackendPath("C:/missing/renderdoc.dll", false);
        view->selectPixel(kind == 2 ? 720 : 20, kind == 1 ? 3 : 0, kind == 1 ? 2 : 0, kind == 3 ? 1 : 0,
                          kind == 1 ? 1 : 0, kind == 1 ? 2 : 0, kind == 3 ? 1 : 0);
        tasks.clear();
        action(*view, "readPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(errors.join('\n')));
        const auto path = directory.filePath("history.json");
        view->exportResult(path);
        const auto original = read(path);
        const auto &rows = original.at("data").at("records");
        QCOMPARE(rows.size(), (kind == 1 || kind == 3) ? size_t(1) : size_t(3));
        if (kind == 0) {
            QCOMPARE(rows[1]["after"]["bytes"], Json("ff0000ff"));
            QCOMPARE(rows[2]["pixel_storage_changed"], Json(false));
            QVERIFY(rows[2]["write_executed"].is_null());
        } else if (kind == 1) {
            QCOMPARE(rows[0]["after"]["decoded_color"]["rgba"], Json::array({32, 16777219, 0, 1}));
        } else if (kind == 3) {
            QCOMPARE(rows[0]["after"]["bytes"], Json("ff00ffff"));
            QCOMPARE(original["data"]["selection"]["slice"], Json(1));
            QCOMPARE(original["data"]["selection"]["layer"], Json(0));
            QCOMPARE(original["data"]["selection"]["mip"], Json(1));
        } else {
            QCOMPARE(rows[0]["after"]["stencil"], Json(83));
            QVERIFY(view->findChild<QTreeWidget *>("historyRecords")
                        ->topLevelItem(0)
                        ->text(5)
                        .contains("Stencil: 83"));
        }
        QCOMPARE(original["context"]["adapter"]["warp"], Json(!vendor));
        if (vendor)
            QCOMPARE(original["context"]["adapter"]["vendor"], Json(vendor));
        for (const auto &module : original.at("execution").at("loaded_modules")) {
            const auto name = QString::fromStdString(module.get<std::string>()).toLower();
            QVERIFY(!name.contains("renderdoc"));
            QVERIFY(!name.contains("intelswtools\\gpa\\"));
        }
        const auto output = qEnvironmentVariable("FLORA_NATIVE_HISTORY_UI_EVIDENCE");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            const auto base = output + '/' + QString::fromLatin1(QTest::currentDataTag());
            view->exportResult(base + ".json");
            if (kind == 1)
                QVERIFY(window.grab().save(base + ".png"));
        }
        tasks.clear();
        action(*view, "readPixelHistory")->trigger();
        action(*view, "cancelPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        QVERIFY(!action(*view, "exportPixelHistory")->isEnabled());
        tasks.clear();
        action(*view, "readPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(errors.join('\n')));
        view->exportResult(path);
        QCOMPARE(read(path)["context_id"], original["context_id"]);
        QCOMPARE(read(path)["data"], original["data"]);
        QVERIFY2(errors.empty(), qPrintable(errors.join('\n')));
        if (kind == 0) {
            auto tree = view->findChild<QTreeWidget *>("historyRecords");
            tree->setCurrentItem(tree->topLevelItem(0));
            window.findChild<QLineEdit *>("apiSearch")->setText("no match");
            action(*view, "locateHistoryEvent")->trigger();
            auto api = window.findChild<QTableView *>("apiLog");
            QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(800));
            QCOMPARE(tree->topLevelItemCount(), 3);
            QTest::qWait(300);
            QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
            for (int i = 0; i < api->model()->rowCount(); ++i)
                if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 2000)
                    api->setCurrentIndex(api->model()->index(i, 0));
            QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(2000));
            QTest::qWait(300);
            QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
            QAction *disable = nullptr, *undo = nullptr;
            for (auto a : window.findChildren<QAction *>()) {
                if (a->text() == "Disable Event")
                    disable = a;
                if (a->text() == "Undo")
                    undo = a;
            }
            QVERIFY(disable && undo);
            disable->trigger();
            QTest::qWait(300);
            QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
            QVERIFY(!action(*view, "exportPixelHistory")->isEnabled());
            tasks.clear();
            action(*view, "readPixelHistory")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(errors.join('\n')));
            view->exportResult(path);
            const auto edited = read(path);
            QVERIFY(edited["context_id"] != original["context_id"]);
            QCOMPARE(edited["data"]["records"][2]["enabled"], Json(false));
            QCOMPARE(edited["data"]["records"][2]["write_executed"], Json(false));
            undo->trigger();
            QTest::qWait(300);
            QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
            tasks.clear();
            action(*view, "readPixelHistory")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(errors.join('\n')));
            view->exportResult(path);
            QCOMPARE(read(path)["data"], original["data"]);
            QVERIFY2(errors.empty(), qPrintable(errors.join('\n')));
            // Image picking must use the native route even with an unusable legacy library.
            action(window, "pickPixelHistory")->setChecked(true);
            tasks.clear();
            QVERIFY(QMetaObject::invokeMethod(window.findChild<ImageView *>("frameOutput"), "pixelSelected",
                                              Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 0),
                                              Q_ARG(QColor, QColor(Qt::red))));
            view->setNativeMode(false);
            QTest::qWait(400);
            QVERIFY(tasks.empty());
            QVERIFY(!window.busy());
            view->setNativeMode(true);
            tasks.clear();
            QVERIFY(QMetaObject::invokeMethod(window.findChild<ImageView *>("frameOutput"), "pixelSelected",
                                              Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 0),
                                              Q_ARG(QColor, QColor(Qt::red))));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(errors.join('\n')));
            view->exportResult(path);
            QCOMPARE(read(path)["context"]["options"]["history_scope"], Json("events"));
            QCOMPARE(read(path)["data"], original["data"]);
        }
    }
};
QTEST_MAIN(NativeHistoryUiTests)
#include "NativeHistoryUiTests.moc"
