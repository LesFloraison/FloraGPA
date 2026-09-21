#include "ClassCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/ShaderProjectDialog.h"
#include "application/ShaderInspector.h"
#include "application/ShaderProject.h"
#include <QAction>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QtTest>

using namespace flora;
using Json = nlohmann::json;
namespace {
Json project() {
    return {
        {"format", "FloraGPA shader project 1"},
        {"root", "main.hlsl"},
        {"entry", "main"},
        {"profile", "ps_5_0"},
        {"files", Json::array({{{"name", "main.hlsl"},
                                {"text", "#include \"color.hlsl\"\nfloat4 main():SV_Target{return COLOR;}"}},
                               {{"name", "color.hlsl"}, {"text", "#define COLOR float4(0,1,0,1)"}}})}};
}
void save(const QString &path, const Json &value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create test JSON");
    file.write(QByteArray::fromStdString(value.dump()));
}
Json load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read test JSON");
    return Json::parse(file.readAll().toStdString());
}
void snapshot(QWidget &widget, const QString &name) {
    const auto root = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
    if (root.isEmpty())
        return;
    QDir().mkpath(root);
    if (!widget.grab().save(root + '/' + name + ".png"))
        throw std::runtime_error("Cannot save screenshot");
}
} // namespace
class ShaderProjectUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { applyAppearance(*qobject_cast<QApplication *>(QCoreApplication::instance())); }
    void editorRoundTrip() {
        auto p = project();
        p["files"][0]["text"] = "// first\r\n// second\r\n";
        ShaderProjectDialog dialog(p);
        dialog.show();
        QCOMPARE(dialog.draft(), p);
        auto source = dialog.findChild<QPlainTextEdit *>("shaderProjectSource");
        source->insertPlainText("// edited\n");
        dialog.addFile("include/新.hlsl");
        source->insertPlainText("#define X 1");
        QCOMPARE(dialog.draft()["files"][2]["text"], Json("#define X 1"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dialog.addFile("INCLUDE\\新.HLSL"));
        dialog.removeFile();
        QCOMPARE(dialog.draft()["files"].size(), size_t(2));
        QTemporaryDir dir;
        dialog.exportProject(dir.filePath("project.json"));
        auto expected = validateShaderProject(dialog.draft());
        dialog.loadProject(project());
        dialog.importProject(dir.filePath("project.json"));
        QCOMPARE(dialog.draft(), expected);
        auto root = dialog.findChild<QComboBox *>("shaderProjectRoot");
        root->setCurrentIndex(1);
        dialog.findChild<QComboBox *>("shaderProjectFile")->setCurrentIndex(1);
        dialog.removeFile();
        QCOMPARE(root->currentIndex(), -1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dialog.exportProject(dir.filePath("invalid.json")));
        QVERIFY(!QFile::exists(dir.filePath("invalid.json")));
        dialog.setBusy(true);
        QVERIFY(source->isReadOnly());
        QVERIFY(!dialog.findChild<QAction *>("applyShaderProject")->isEnabled());
        dialog.setBusy(false);
        dialog.loadProject(validateShaderProject(project()));
        snapshot(dialog, "shader-project-sources");
        auto tabs = dialog.findChild<QTabWidget *>();
        tabs->setCurrentIndex(1);
        snapshot(dialog, "shader-project-settings");
    }
    void experimentAndCli() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("fixture.gpa_frame");
        testing::graphicsClassCapture().save(capture);
        Frame frame(capture.toStdWString());
        Experiment experiment(frame);
        const auto original = experiment.shaderBytes(frame, 32);
        auto compiled = compileShaderProject(project());
        experiment.setShaderProject(frame, 32, compiled.bytecode, project());
        QCOMPARE(experiment.shaderProject(32), validateShaderProject(project()));
        const auto path = dir.filePath("experiment.json");
        experiment.save(path);
        Experiment loaded(frame);
        loaded.load(path, frame);
        QCOMPARE(loaded.shaderProject(32), validateShaderProject(project()));
        QCOMPARE(loaded.shaderBytes(frame, 32), compiled.bytecode);
        QVERIFY(loaded.undo());
        QVERIFY(loaded.shaderProject(32).is_null());
        QCOMPARE(loaded.shaderBytes(frame, 32), original);
        QVERIFY(loaded.redo());
        loaded.setShader(frame, 32, compiled.bytecode, "// edited", "main");
        QVERIFY(loaded.shaderProject(32).is_null());
        QVERIFY(loaded.undo());
        QVERIFY(!loaded.shaderProject(32).is_null());
        auto cli = [&](QStringList args, bool success) {
            QProcess process;
            process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
            if (!process.waitForStarted() || !process.waitForFinished(30000))
                throw std::runtime_error("CLI timed out");
            if ((process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) != success)
                throw std::runtime_error(process.readAllStandardError().toStdString());
        };
        save(dir.filePath("source.json"), project());
        cli({"compile-project", capture, "--id", "32", "--source", dir.filePath("source.json"), "--out",
             dir.filePath("compiled")},
            true);
        const auto report = load(dir.filePath("compiled/report.json"));
        QCOMPARE(report.at("compilation"), compiled.report);
        cli({"shader", capture, "--id", "32", "--experiment", path, "--out", dir.filePath("inspected")},
            true);
        const auto metadata = load(dir.filePath("inspected/shader.json"));
        QCOMPARE(metadata.at("source_project").at("available"), Json(true));
        QCOMPARE(load(dir.filePath("inspected/shader_project.json")), validateShaderProject(project()));
        auto corrupt = load(path);
        corrupt["history"][0]["operations"][0]["source_project"]["files"][1]["text"] =
            "#define COLOR float4(1,0,0,1)";
        save(dir.filePath("mismatch.json"), corrupt);
        cli({"shader", capture, "--id", "32", "--experiment", dir.filePath("mismatch.json"), "--out",
             dir.filePath("mismatch")},
            true);
        QCOMPARE(load(dir.filePath("mismatch/shader.json")).at("source_project").at("available"),
                 Json(false));
        QVERIFY(!QFile::exists(dir.filePath("mismatch/shader_project.json")));
        auto wrong = project();
        wrong["profile"] = "vs_5_0";
        wrong["files"][0]["text"] = "float4 main(float4 p:POSITION):SV_Position{return p;}";
        save(dir.filePath("wrong.json"), wrong);
        cli({"compile-project", capture, "--id", "32", "--source", dir.filePath("wrong.json"), "--out",
             dir.filePath("wrong")},
            false);
        QVERIFY(!QFile::exists(dir.filePath("wrong/replacement.dxbc")));
        const auto evidence = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!evidence.isEmpty()) {
            for (const auto folder : {"compiled", "inspected", "mismatch"}) {
                const auto destination = evidence + "/cli/" + folder;
                QDir().mkpath(destination);
                for (const auto &name : QDir(dir.filePath(folder)).entryList(QDir::Files))
                    QVERIFY(
                        QFile::copy(dir.filePath(QString(folder) + '/' + name), destination + '/' + name));
            }
        }
    }
    void workerApplyAndStaleDraft() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("fixture.gpa_frame");
        testing::graphicsClassCapture().save(capture);
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
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 32)
                resources->setCurrentIndex(index);
        }
        window.findChild<QPlainTextEdit *>("shaderSource")
            ->setPlainText("float4 main():SV_Target{return 1;}");
        done.clear();
        window.findChild<QAction *>("openShaderProject")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto editor = window.findChild<ShaderProjectDialog *>();
        QVERIFY(editor);
        editor->loadProject(project());
        done.clear();
        editor->findChild<QAction *>("applyShaderProject")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY2(result[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        editor->exportProject(dir.filePath("draft.json"));
        auto broken = project();
        broken["files"][0]["text"] = "#include \"missing.hlsl\"\n";
        editor->loadProject(broken);
        done.clear();
        editor->findChild<QAction *>("applyShaderProject")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(editor->findChild<QAction *>("applyShaderProject")->isEnabled());
        editor->loadProject(project());
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        done.clear();
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        editor->findChild<QAction *>("applyShaderProject")->trigger();
        QVERIFY(!window.busy());
        QVERIFY(window.statusBar()->currentMessage().contains("context changed"));
        editor->exportProject(dir.filePath("stale-draft.json"));
        QCOMPARE(load(dir.filePath("stale-draft.json")), validateShaderProject(project()));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        editor->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        done.clear();
        window.findChild<QAction *>("openShaderProject")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        editor = window.findChild<ShaderProjectDialog *>();
        QVERIFY(editor);
        QCOMPARE(editor->draft(), validateShaderProject(project()));
        snapshot(window, "shader-project-applied");
    }
    void capturedProject_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void capturedProject() {
        QFETCH(bool, warp);
        const auto base = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (base.isEmpty())
            QSKIP("Optional original GPA fixture directory is not configured");
        const auto sample = base + "/analysis/capture_samples/shader_project/";
        const auto manifest = load(sample + "manifest.json");
        Frame frame((sample + "project.gpa_frame").toStdWString());
        QCOMPARE(frame.sha256(), manifest.at("sha256").get<std::string>());
        const auto id = manifest.at("shader_id").get<Id>();
        const auto code = frame.shader(frame.resource(id).data);
        auto p = shaderProjectFromSources(inspectShader(code).at("embedded_sources"), "ps_5_0");
        QCOMPARE(p.at("files").size(), size_t(3));
        p["root"] = manifest.at("source_project").at("root");
        QCOMPARE(verifyShaderProject(code, p).at("includes").size(), size_t(2));
        Experiment experiment(frame);
        auto render = [&] {
            ReplayOptions options;
            options.warp = warp;
            experiment.apply(frame, options);
            Replay replay(frame, options);
            replay.run();
            return replay.output();
        };
        auto original = render();
        QCOMPARE(sha256(original.rgba), manifest.at("rgba_sha256").get<std::string>());
        for (auto &f : p["files"]) {
            if (f.at("name").get<std::string>().ends_with("color.hlsl"))
                f["text"] = "#include \"C:/FloraVirtual/include/constants.hlsl\"\nfloat4 "
                            "projectColor(){return float4(RED_VALUE,1,0,alphaValue);}\n";
        }
        auto compiled = compileShaderProject(p);
        experiment.setShaderProject(frame, id, compiled.bytecode, p);
        auto green = render();
        QCOMPARE(green.width, 64u);
        QCOMPARE(green.height, 64u);
        std::vector<uint8_t> expected;
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x) {
                const std::array<uint8_t, 4> pixel =
                    x < 32 ? std::array<uint8_t, 4>{128, 0, 0, 255} : std::array<uint8_t, 4>{0, 255, 0, 255};
                expected.insert(expected.end(), pixel.begin(), pixel.end());
            }
        QCOMPARE(green.rgba, expected);
        QTemporaryDir dir;
        experiment.save(dir.filePath("edited.json"));
        Experiment loaded(frame);
        loaded.load(dir.filePath("edited.json"), frame);
        QCOMPARE(loaded.shaderProject(id), validateShaderProject(p));
        auto before = experiment.document();
        auto broken = p;
        broken["files"][0]["text"] = "#include \"missing.hlsl\"";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, compileShaderProject(broken));
        QCOMPARE(experiment.document(), before);
        QVERIFY(experiment.undo());
        QCOMPARE(render().rgba, original.rgba);
        QVERIFY(experiment.redo());
        QCOMPARE(render().rgba, expected);
        const auto evidence = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!evidence.isEmpty()) {
            save(evidence + (warp ? "/captured-warp.json" : "/captured-hardware.json"),
                 {{"original_sha256", sha256(original.rgba)},
                  {"edited_sha256", sha256(green.rgba)},
                  {"compilation", compiled.report},
                  {"project", p}});
        }
    }
};
QTEST_MAIN(ShaderProjectUiTests)
#include "ShaderProjectUiTests.moc"
