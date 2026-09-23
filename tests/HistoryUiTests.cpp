#include "MsaaCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing history test report");
    auto bytes = f.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
QAction *action(QWidget &view, const char *name) { return view.findChild<QAction *>(name); }
} // namespace
class HistoryUiTests : public QObject {
    Q_OBJECT
    QString output_;
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-HistoryTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty())
            QVERIFY(QDir().mkpath(output_));
    }
    void backendReports() {
        const auto root = qEnvironmentVariable("FLORA_HISTORY_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_HISTORY_REPORTS to the native/Python history comparison corpus");
        const auto directories = QDir(root).entryList({"*-native"}, QDir::Dirs);
        int verified = 0;
        for (const auto &directory : directories) {
            const auto result = read(root + '/' + directory + "/result.json");
            if (!result.value("ok", false) || result.value("action", "") != "history")
                continue;
            PixelHistoryView view;
            view.resize(1200, 660);
            view.show();
            view.setContext("fixture", 1);
            QVERIFY(view.finish(view.requestId(), result));
            auto rows = view.findChild<QTreeWidget *>("historyRecords");
            QCOMPARE(rows->topLevelItemCount(), int(result["history"].size()));
            QSignalSpy jump(&view, &PixelHistoryView::eventRequested);
            for (int i = 0; i < rows->topLevelItemCount(); ++i) {
                const auto &record = result["history"][size_t(i)];
                auto item = rows->topLevelItem(i);
                rows->setCurrentItem(item);
                QCOMPARE(Json::parse(
                             view.findChild<QPlainTextEdit *>("historyDetails")->toPlainText().toStdString()),
                         record);
                if (record["gpa_event"].is_null())
                    QVERIFY(!action(view, "locateHistoryEvent")->isEnabled());
                else {
                    jump.clear();
                    action(view, "locateHistoryEvent")->trigger();
                    QCOMPARE(jump.size(), 1);
                    QCOMPARE(jump[0][0].toULongLong(), qulonglong(record["gpa_event"].get<uint64_t>()));
                }
                if (record["record_kind"] == "cpu_write_snapshot") {
                    QCOMPARE(item->text(2), QString("—"));
                    QVERIFY(item->text(3).contains("CPU snapshot"));
                }
                if (record["color_interpretation"] == "uint")
                    QCOMPARE(item->text(5).split(", ")[0],
                             QString::number(record["postMod"]["col"]["uintValue"][0].get<uint32_t>()));
                if (record["color_interpretation"] == "sint")
                    QCOMPARE(item->text(5).split(", ")[0],
                             QString::number(record["postMod"]["col"]["intValue"][0].get<int32_t>()));
            }
            QTemporaryDir temporary;
            const auto path = temporary.path() + "/history.json";
            view.exportResult(path);
            QCOMPARE(read(path), result);
            // A later event selection may retain this frame/experiment snapshot for navigation.
            view.setContext("fixture", 2);
            QVERIFY(action(view, "exportPixelHistory")->isEnabled());
            const auto old = view.requestId();
            view.setContext("changed-experiment", 2);
            QVERIFY(!view.finish(old, result));
            QCOMPARE(rows->topLevelItemCount(), 0);
            QVERIFY(!action(view, "exportPixelHistory")->isEnabled());
            ++verified;
        }
        QCOMPARE(verified, 22);
    }
    void cpuWriteMainWindow() {
        const auto path = qEnvironmentVariable("FLORA_HISTORY_COMMANDS");
        if (path.isEmpty() || !flora::historyBackendCompatible("C:/Program Files/RenderDoc/renderdoc.dll"))
            QSKIP("Set FLORA_HISTORY_COMMANDS to the command fixture and install RenderDoc 1.45");
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto view = window.findChild<PixelHistoryView *>();
        QVERIFY(view);
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(view);
        view->selectPixel(84, 0, 0, 1, 0, 0);
        view->findChild<QLineEdit *>("historyEvent")->setText("213");
        action(*view, "readPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto rows = view->findChild<QTreeWidget *>("historyRecords");
        QTreeWidgetItem *cpu{};
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->text(0) == "213")
                cpu = rows->topLevelItem(i);
        QVERIFY(cpu);
        QCOMPARE(cpu->text(2), QString("—"));
        QVERIFY(cpu->text(3).contains("CPU snapshot"));
        rows->setCurrentItem(cpu);
        action(*view, "historyDetailsToggle")->trigger();
        QTRY_VERIFY(view->findChild<QPlainTextEdit *>("historyDetails")->isVisible());
        QTRY_VERIFY(view->findChild<QPlainTextEdit *>("historyDetails")->height() > 50);
        if (!output_.isEmpty()) {
            view->exportResult(output_ + "/history-cpu.json");
            const auto report = read(output_ + "/history-cpu.json");
            const auto capture = QString::fromStdString(report.at("capture").get<std::string>());
            QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"),
                                output_ + "/recapture-cpu.json"));
            QVERIFY(window.grab().save(output_ + "/history-cpu.png"));
        }
        window.findChild<QLineEdit *>("apiSearch")->setText("not present");
        action(*view, "locateHistoryEvent")->trigger();
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 qulonglong(213));
        QVERIFY(rows->topLevelItemCount() > 0);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
    }
    void mainWindow_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void mainWindow() {
        QFETCH(bool, warp);
        if (!flora::historyBackendCompatible("C:/Program Files/RenderDoc/renderdoc.dll"))
            QSKIP("Optional RenderDoc 1.45 is not installed");
        QTemporaryDir directory;
        const auto frame = directory.path() + "/msaa.gpa_frame";
        testing::msaaOutputCapture().save(frame);
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QStringList dialogs;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, [&] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(widget)) {
                    dialogs << box->text();
                    box->reject();
                }
        });
        dismiss.start(25);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(frame);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        QVERIFY(adapter);
        if (adapter->currentIndex() != int(warp)) {
            adapter->setCurrentIndex(int(warp));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
        }
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 110) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto history = window.findChild<PixelHistoryView *>();
        QVERIFY(history);
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(history);
        history->selectPixel(20, 2, 2, 0, 0, 2);
        history->findChild<QLineEdit *>("historyEvent")->setText("100");
        auto readAction = action(*history, "readPixelHistory");
        QVERIFY(readAction->isEnabled());
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        const auto exported = directory.path() + "/history.json";
        history->exportResult(exported);
        const auto original = read(exported);
        QVERIFY(original["history_events"].get<size_t>() > 0);
        const auto &last = original["history"].back();
        QCOMPARE(last["gpa_event"].get<uint64_t>(), uint64_t(100));
        QCOMPARE(last["postMod"]["col"]["uintValue"][0].get<uint32_t>(), uint32_t(12));
        QCOMPARE(last["postMod"]["col"]["uintValue"][1].get<uint32_t>(), uint32_t(16777219));
        const auto firstCapture = original["capture"];
        history->findChild<QSpinBox *>("historyX")->setValue(3);
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        history->exportResult(exported);
        QCOMPARE(read(exported)["capture"], firstCapture);
        // Cancellation and retry use the existing Windows job isolation.
        readAction->trigger();
        action(*history, "cancelPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
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
        QCOMPARE(history->findChild<QTreeWidget *>("historyRecords")->topLevelItemCount(), 0);
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        history->exportResult(exported);
        const auto edited = read(exported);
        QVERIFY(edited["capture"] != firstCapture);
        QVERIFY(!edited["gpa_command_map"].contains("110"));
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        history->exportResult(exported);
        QCOMPARE(read(exported)["capture"], firstCapture);
        if (!output_.isEmpty()) {
            history->exportResult(output_ + (warp ? "/history-warp.json" : "/history-hardware.json"));
            const auto capture = QString::fromStdString(firstCapture.get<std::string>());
            QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"),
                                output_ + (warp ? "/recapture-warp.json" : "/recapture-hardware.json")));
            QVERIFY(window.grab().save(output_ + (warp ? "/history-warp.png" : "/history-hardware.png")));
        }
        QVERIFY2(dialogs.empty(), qPrintable(dialogs.join('\n')));
        // API navigation remains usable under filters and keeps the historical rows visible.
        auto rows = history->findChild<QTreeWidget *>("historyRecords");
        rows->setCurrentItem(rows->topLevelItem(0));
        window.findChild<QLineEdit *>("apiSearch")->setText("no match");
        action(*history, "locateHistoryEvent")->trigger();
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(100));
        QVERIFY(rows->topLevelItemCount() > 0);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        history->setBackendPath(directory.path() + "/missing.dll");
        readAction->trigger();
        QCOMPARE(tasks.size(), 1);
        QVERIFY(!tasks.takeLast()[0].toBool());
        history->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        history->exportResult(exported);
        QCOMPARE(read(exported)["capture"], firstCapture);
        // A changed API selection cancels an active analysis, rather than accepting stale rows.
        readAction->trigger();
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 110) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTest::qWait(1000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QCOMPARE(rows->topLevelItemCount(), 0);
        QVERIFY(!action(*history, "exportPixelHistory")->isEnabled());
        if (!warp) {
            // A selected output pixel fills the history query without replacing the Output tab.
            auto output = window.findChild<ImageView *>("frameOutput");
            QVERIFY(output);
            QVERIFY(QMetaObject::invokeMethod(output, "pixelSelected", Qt::DirectConnection, Q_ARG(int, 2),
                                              Q_ARG(int, 1), Q_ARG(QColor, QColor(1, 2, 3))));
            QCOMPARE(history->findChild<QSpinBox *>("historyX")->value(), 2);
            QCOMPARE(history->findChild<QSpinBox *>("historyY")->value(), 1);
            QCOMPARE(history->findChild<QLineEdit *>("historyResource")->text(), QString("20"));
        }
    }
};
QTEST_MAIN(HistoryUiTests)
#include "HistoryUiTests.moc"
