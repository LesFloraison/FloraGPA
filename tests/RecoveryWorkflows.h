#pragma once
#include "app/CaptureStructureDialog.h"
#include "app/MainWindow.h"
#include "application/ContextInspector.h"
#include <QCryptographicHash>
#include <QFileDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabWidget>
#include <QtTest>

namespace flora::testing {
// Optional sustained checks against the real MainWindow actions. These checks
// compare existing decoders, not an independent reference implementation.
inline QByteArray recoveryRead(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read workflow evidence");
    return file.readAll();
}

inline void recoveryChoose(QWidget &owner, const QString &path, const std::function<void()> &action,
                           bool &passed) {
    passed = false;
    bool seen = false;
    QTimer poll, watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&] {
        if (auto chooser = owner.findChild<QFileDialog *>()) {
            seen = true;
            if (path.isEmpty()) { chooser->reject(); return; }
            auto edit = chooser->findChild<QLineEdit *>("fileNameEdit");
            QVERIFY(edit);
            edit->setText(path);
            QMetaObject::invokeMethod(chooser, "accept", Qt::QueuedConnection);
        }
    });
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        if (auto chooser = owner.findChild<QFileDialog *>()) chooser->reject();
    });
    poll.start(10); watchdog.start(30000); action();
    QVERIFY(seen);
    passed = true;
}

inline void recoveryWorkflows(MainWindow &window, const QString &directory,
                              nlohmann::json &evidence, bool &passed,
                              const std::function<void(const char *)> &observe = {}) {
    passed = false;
    QVERIFY(QDir().mkpath(directory));
    const auto snapshot = [&](const char *stage) { if (observe) observe(stage); };
    snapshot("before_query");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto frame = std::make_shared<Frame>(window.capturePath().toStdWString());
    const auto queries = QueryInspection::prepare(frame);
    QVERIFY(queries->size() > 0);
    auto kinds = window.findChild<QComboBox *>("apiKinds");
    auto search = window.findChild<QLineEdit *>("apiSearch");
    auto table = window.findChild<QTableView *>("apiLog");
    QVERIFY(kinds && search && table);
    const auto reset = qScopeGuard([&] { search->clear(); kinds->setCurrentIndex(0); });
    kinds->setCurrentIndex(1); search->setText("GetData");
    auto proxy = static_cast<CaptureFilter *>(table->model());
    auto model = static_cast<CaptureModel *>(proxy->sourceModel());
    QCOMPARE(size_t(table->model()->rowCount()), queries->size());
    evidence["query_events"] = nlohmann::json::array();
    QSignalSpy replayed(&window, &MainWindow::taskFinished);
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const auto index = table->model()->index(row, 0);
        const auto id = index.data(Qt::UserRole).toULongLong();
        table->setCurrentIndex(index);
        QVERIFY(queries->find(id));
        QCOMPARE(model->command(id), *queries->find(id));
        auto properties = window.findChild<QTreeWidget *>("properties");
        QVERIFY(properties && !properties->findItems("query_result", Qt::MatchExactly).empty());
        evidence["query_events"].push_back(id);
    }
    // Selecting a record schedules a debounced replay. Restore Final and wait
    // for that worker before starting exports; busy() alone misses the timer.
    auto boundary = window.findChild<QComboBox *>("outputBoundary");
    QVERIFY(boundary); QCOMPARE(boundary->currentIndex(), 2);
    boundary->setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(!replayed.empty() && !window.busy(), 60000);
    QVERIFY(replayed.last()[0].toBool());
    snapshot("after_query");
    QSignalSpy exported(&window, &MainWindow::exportFinished);
    auto exportApi = window.findChild<QAction *>("exportApiLog");
    QVERIFY(exportApi);
    bool chosen = false;
    recoveryChoose(window, {}, [&] { exportApi->trigger(); }, chosen);
    QVERIFY(chosen); QCOMPARE(exported.size(), 0); QVERIFY(!window.busy());
    snapshot("after_api_cancel");
    const auto apiPath = directory + "/api";
    QVERIFY(QDir().mkpath(apiPath));
    recoveryChoose(window, apiPath, [&] { exportApi->trigger(); }, chosen);
    QVERIFY(chosen);
    QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000);
    QVERIFY(exported.last()[0].toBool()); QVERIFY(!window.busy());
    QTemporaryDir expected;
    QVERIFY(expected.isValid());
    exportCommands(*frame, expected.path().toStdWString(), "GetData");
    for (const auto name : {"commands.json", "commands.csv"})
        QCOMPARE(recoveryRead(apiPath + '/' + name), recoveryRead(expected.filePath(name)));
    snapshot("after_api_export");

    auto structure = window.findChild<QAction *>("inspectCaptureStructure");
    QVERIFY(structure);
    bool inspected = false;
    QTimer::singleShot(0, &window, [&] {
        auto dialog = window.findChild<CaptureStructureDialog *>("captureStructureDialog");
        QVERIFY(dialog);
        const auto close = qScopeGuard([&] { dialog->reject(); });
        auto inventory = dialog->findChild<QTreeView *>("contextInventory");
        auto lists = dialog->findChild<QTreeView *>("commandListInventory");
        QVERIFY(inventory && lists);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->busy() && inventory->model() && lists->model(), 30000);
        snapshot("structure_open");
        auto tabs = dialog->findChild<QTabWidget *>();
        auto button = dialog->findChild<QPushButton *>("exportCaptureStructure");
        QVERIFY(tabs && button && button->isEnabled());
        QSignalSpy saved(dialog, &CaptureStructureDialog::exportFinished);
        recoveryChoose(*dialog, {}, [&] { button->click(); }, chosen);
        QVERIFY(chosen); QCOMPARE(saved.size(), 0); QVERIFY(!dialog->busy());
        snapshot("after_structure_cancel");
        for (int tab = 0; tab < 2; ++tab) {
            tabs->setCurrentIndex(tab); saved.clear();
            const auto target = directory + (tab ? "/command-lists.json" : "/contexts.json");
            recoveryChoose(*dialog, target, [&] { button->click(); }, chosen);
            QVERIFY(chosen);
            QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 30000);
            QVERIFY(saved.last()[0].toBool()); QVERIFY(!dialog->busy());
            const auto document = tab ? inspectCommandLists(*frame) : inspectContexts(*frame);
            QCOMPARE(recoveryRead(target), QByteArray::fromStdString(document.dump(2) + "\n"));
            snapshot(tab ? "after_lists_export" : "after_contexts_export");
        }
        inspected = true;
    });
    structure->trigger();
    QVERIFY(inspected);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!window.findChild<CaptureStructureDialog *>());
    snapshot("after_structure_close");
    evidence["files"] = nlohmann::json::object();
    for (const auto name : {"api/commands.json", "api/commands.csv", "contexts.json", "command-lists.json"}) {
        const auto bytes = recoveryRead(directory + '/' + name);
        evidence["files"][name] = {{"bytes", bytes.size()},
            {"sha256", QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString()}};
    }
    evidence["cancelled_choosers"] = 2;
    passed = true;
}
} // namespace flora::testing
