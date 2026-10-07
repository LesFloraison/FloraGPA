#pragma once
#include "app/MainWindow.h"
#include <QAction>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>
#include <QtTest>

namespace flora::testing {
// Keep driving the visible test-owned dialog until its accepted signal confirms
// completion. A queued accept invocation alone need not close the dialog.
inline void metricFileAction(MainWindow &window, const char *name, const QString &path) {
    bool accepted = false, timedOut = false;
    QStringList selected;
    QPointer<QFileDialog> observed;
    QElapsedTimer elapsed; elapsed.start();
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &timer, [&] {
        QFileDialog *dialog = nullptr;
        for (auto candidate : window.findChildren<QFileDialog *>())
            if (candidate->isVisible()) { dialog = candidate; break; }
        if (elapsed.elapsed() > 10000) {
            timedOut = true;
            if (dialog) {
                qInfo() << "Dialog timeout" << dialog->windowTitle() << "requested" << path
                        << "exists" << QFileInfo::exists(path) << "selected" << dialog->selectedFiles()
                        << "directory" << dialog->directory().absolutePath();
                for (auto line : dialog->findChildren<QLineEdit *>())
                    qInfo() << "Dialog input" << line->objectName() << line->text();
                timer.stop(); dialog->reject();
            }
            return;
        }
        if (!dialog) return;
        if (observed != dialog) {
            observed = dialog;
            QObject::connect(dialog, &QFileDialog::accepted, &timer, [&, dialog] {
                selected = dialog->selectedFiles(); accepted = true; timer.stop();
            });
        }
        // Qt 6.11.2 deliberately leaves the filename editor unchanged while
        // a visible dialog's editor has focus. Release test-owned focus first.
        if (auto focus = dialog->focusWidget()) focus->clearFocus();
        dialog->selectFile(path);
        const auto files = dialog->selectedFiles();
        if (files.size() == 1 && QFileInfo(files.front()).absoluteFilePath() == QFileInfo(path).absoluteFilePath())
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    timer.start(20);
    auto action = window.findChild<QAction *>(name);
    QVERIFY(action && action->isEnabled());
    action->trigger();
    QVERIFY2(!timedOut, "Experiment file dialog did not accept within ten seconds");
    QVERIFY(accepted);
    QCOMPARE(selected.size(), 1);
    QCOMPARE(QFileInfo(selected.front()).absoluteFilePath(), QFileInfo(path).absoluteFilePath());
}
} // namespace flora::testing
