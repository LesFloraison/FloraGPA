#include "SyntheticCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QSignalSpy>
#include <QStatusBar>
#include <QtTest>
using namespace flora;
class HlslRecoveryUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { applyAppearance(*qobject_cast<QApplication *>(QCoreApplication::instance())); }
    void recoverApplyUndoAndStaleResult() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("fixture.gpa_frame");
        testing::graphicsCounterCapture(false).save(capture);
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        auto resources = window.findChild<QTableView *>("resources");
        auto select = [&](Id id) {
            for (int row = 0; row < resources->model()->rowCount(); ++row) {
                const auto index = resources->model()->index(row, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) {
                    resources->setCurrentIndex(index);
                    return;
                }
            }
            throw std::runtime_error("Missing shader test resource");
        };
        select(32);
        auto recover = window.findChild<QAction *>("recoverShader");
        auto compile = window.findChild<QAction *>("compileShader");
        auto editor = window.findChild<QPlainTextEdit *>("shaderSource");
        auto sources = window.findChild<QComboBox *>("shaderSources");
        QVERIFY(recover && compile && editor && sources);
        done.clear();
        recover->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(sources->currentText(), QString("Reconstructed HLSL"));
        QVERIFY(editor->toPlainText().startsWith("// Reconstructed from DXBC."));
        QVERIFY(editor->toPlainText().contains(".Append("));
        const auto reconstructed = editor->toPlainText();
        done.clear();
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY2(result[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        done.clear();
        recover->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(sources->currentText(), QString("Applied HLSL (verified)"));
        QCOMPARE(editor->toPlainText(), reconstructed);
        auto modified = reconstructed;
        QVERIFY(modified.contains("result.field0=asfloat(o0.xyzw);"));
        modified.replace("result.field0=asfloat(o0.xyzw);", "result.field0=float4(0,1,0,1);");
        editor->setPlainText(modified);
        done.clear();
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY(result[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            QVERIFY(window.grab().save(artifact + "/hlsl-recovery-applied.png"));
        }
        QAction *undo = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
        QVERIFY(undo);
        done.clear();
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        editor->setPlainText("invalid HLSL");
        done.clear();
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(editor->toPlainText(), QString("invalid HLSL"));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        done.clear();
        recover->trigger();
        QVERIFY(window.busy());
        select(30);
        editor->setPlainText("// new selection draft");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(editor->toPlainText(), QString("// new selection draft"));
        QVERIFY2(window.statusBar()->currentMessage() == "Cancelled" ||
                     window.statusBar()->currentMessage().contains("context changed"),
                 qPrintable(window.statusBar()->currentMessage()));
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("FloraGPA-HlslRecovery-Test");
    app.setOrganizationName("FloraGPA-Tests");
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto box = qobject_cast<QMessageBox *>(widget))
                box->accept();
    });
    dismiss.start(20);
    HlslRecoveryUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "HlslRecoveryUiTests.moc"
