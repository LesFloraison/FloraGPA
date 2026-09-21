#include "SyntheticCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QSignalSpy>
#include <QStatusBar>
#include <QtTest>
using namespace flora;
namespace {
void select(MainWindow &window, Id id) {
    auto resources = window.findChild<QTableView *>("resources");
    for (int row = 0; row < resources->model()->rowCount(); ++row) {
        const auto index = resources->model()->index(row, 0);
        if (index.data(Qt::UserRole).toULongLong() == id) {
            resources->setCurrentIndex(index);
            return;
        }
    }
    throw std::runtime_error("Missing shader resource");
}
void fileAction(MainWindow &window, const char *name, const QString &path) {
    bool handled = false;
    QTimer choose;
    QObject::connect(&choose, &QTimer::timeout, &window, [&] {
        auto dialog = window.findChild<QFileDialog *>();
        if (!dialog || !dialog->isVisible())
            return;
        choose.stop();
        QTimer::singleShot(3000, dialog, &QDialog::reject);
        dialog->selectFile(path);
        handled = true;
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    choose.start(20);
    window.findChild<QAction *>(name)->trigger();
    QVERIFY(handled);
}
nlohmann::json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing test document");
    return nlohmann::json::parse(file.readAll().toStdString());
}
} // namespace
class ExternalShaderUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { applyAppearance(*qobject_cast<QApplication *>(QCoreApplication::instance())); }
    void assemblyDraftApplyUndoAndReload() {
        const auto tool = qEnvironmentVariable("FLORA_TEST_SHADER_TOOL");
        if (tool.isEmpty())
            QSKIP("Optional cmd_Decompiler.exe is not configured");
        QTemporaryDir dir;
        const auto capture = dir.filePath("fixture.gpa_frame");
        testing::graphicsCounterCapture(false).save(capture);
        MainWindow window;
        window.setShaderTool(tool);
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        select(window, 32);
        auto assembly = window.findChild<QPlainTextEdit *>("shader");
        auto source = window.findChild<QPlainTextEdit *>("shaderSource");
        auto entry = window.findChild<QLineEdit *>("shaderEntry");
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(assembly && source && entry && output);
        const auto original = assembly->toPlainText();
        const QString red = "l(0x3f800000,0x00000000,0x00000000,0x3f800000)";
        const QString green = "l(0x00000000,0x3f800000,0x00000000,0x3f800000)";
        QVERIFY2(original.contains(red), qPrintable(original));
        auto modified = original;
        modified.replace(red, green);
        assembly->setPlainText(modified);
        source->setPlainText("// unapplied HLSL draft\n");
        entry->setText("draftEntry");
        select(window, 30);
        source->setPlainText("// vertex draft");
        select(window, 32);
        QCOMPARE(assembly->toPlainText(), modified);
        QCOMPARE(source->toPlainText(), QString("// unapplied HLSL draft\n"));
        QCOMPARE(entry->text(), QString("draftEntry"));
        done.clear();
        window.findChild<QAction *>("assembleShader")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY2(result[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        const auto project = dir.filePath("experiment.json");
        fileAction(window, "saveExperiment", project);
        const auto saved = read(project);
        QCOMPARE(saved.at("ui").at("shader_documents").at("32:DXBC 汇编"),
                 nlohmann::json(modified.toStdString()));
        QCOMPARE(saved.at("ui").at("shader_entries").at("32:HLSL"), nlohmann::json("draftEntry"));
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
        QCOMPARE(assembly->toPlainText(), modified);
        done.clear();
        window.findChild<QAction *>("readShaderAssembly")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(assembly->toPlainText(), original);
        done.clear();
        fileAction(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        select(window, 30);
        select(window, 32);
        QCOMPARE(assembly->toPlainText(), modified);
        QCOMPARE(source->toPlainText(), QString("// unapplied HLSL draft\n"));
        QCOMPARE(entry->text(), QString("draftEntry"));
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            QVERIFY(window.grab().save(artifact + "/external-shader-assembly.png"));
        }
    }
    void cancellationKillsExternalTree() {
        QTemporaryDir dir;
        const auto tool = dir.filePath("shader tool.exe");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/FloraShaderToolStub.exe", tool));
        const auto record = dir.filePath("invocation.json");
        QFile config(dir.filePath("shader-tool-stub.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QByteArray::fromStdString(
            nlohmann::json{{"child", true}, {"sleep_ms", 60000}, {"record", record.toStdString()}}.dump()));
        config.close();
        const auto capture = dir.filePath("fixture.gpa_frame");
        testing::graphicsCounterCapture(false).save(capture);
        MainWindow window;
        window.setShaderTool(tool);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        select(window, 32);
        auto assembly = window.findChild<QPlainTextEdit *>("shader");
        const auto draft = assembly->toPlainText() + "\n// retained draft\n";
        assembly->setPlainText(draft);
        done.clear();
        window.findChild<QAction *>("assembleShader")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(record).size() > 0, 10000);
        const auto invocation = read(record);
        auto parent = OpenProcess(SYNCHRONIZE, FALSE, invocation.at("pid").get<DWORD>());
        auto child = OpenProcess(SYNCHRONIZE, FALSE, invocation.at("child").get<DWORD>());
        QVERIFY(parent && child);
        auto cleanup = qScopeGuard([&] {
            CloseHandle(parent);
            CloseHandle(child);
        });
        QAction *cancel = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence("Escape"))
                cancel = action;
        QVERIFY(cancel && cancel->isEnabled());
        cancel->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(WaitForSingleObject(parent, 5000), DWORD(WAIT_OBJECT_0));
        QCOMPARE(WaitForSingleObject(child, 5000), DWORD(WAIT_OBJECT_0));
        QCOMPARE(assembly->toPlainText(), draft);
        QCOMPARE(window.findChild<ImageView *>("frameOutput")->image().pixelColor(0, 0), QColor(Qt::red));
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setApplicationName("FloraGPA-ExternalShader-Test");
    app.setOrganizationName("FloraGPA-Tests");
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto box = qobject_cast<QMessageBox *>(widget)) {
                if (box->standardButtons().testFlag(QMessageBox::Discard))
                    box->done(QMessageBox::Discard);
                else
                    box->accept();
            }
    });
    dismiss.start(20);
    ExternalShaderUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ExternalShaderUiTests.moc"
