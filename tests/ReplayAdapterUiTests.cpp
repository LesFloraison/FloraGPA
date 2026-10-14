#include "MsaaCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/ReplayAdapters.h"
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read adapter test input");
    return Json::parse(file.readAll().toStdString());
}
QByteArray rgba(const QImage &source) {
    const auto image = source.convertToFormat(QImage::Format_RGBA8888);
    QByteArray result;
    for (int y = 0; y < image.height(); ++y)
        result.append(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    return result;
}
void fileAction(MainWindow &window, const char *name, const QString &path) {
    bool handled = false, timedOut = false;
    QString unexpected;
    QTimer choose, deadline;
    QObject::connect(&choose, &QTimer::timeout, &window, [&] {
        for (auto message : window.findChildren<QMessageBox *>()) {
            if (!message->isVisible()) continue;
            if (message->windowTitle() == "Unsaved experiment" && message->button(QMessageBox::Discard))
                message->button(QMessageBox::Discard)->click();
            else {
                unexpected = message->text();
                message->reject();
            }
        }
        for (auto dialog : window.findChildren<QFileDialog *>()) {
            if (!dialog->isVisible()) continue;
            auto editor = dialog->findChild<QLineEdit *>("fileNameEdit");
            QVERIFY(editor);
            editor->setText(path);
            handled = true;
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        }
    });
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &window, [&] {
        timedOut = true;
        for (auto dialog : window.findChildren<QFileDialog *>()) dialog->reject();
        for (auto message : window.findChildren<QMessageBox *>()) message->reject();
    });
    choose.start(20);
    deadline.start(10000);
    window.findChild<QAction *>(name)->trigger();
    QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected));
    QVERIFY(handled && !timedOut);
}
} // namespace
class ReplayAdapterUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void commandScope() {
        QComboBox combo;
        populateReplayAdapters(combo);
        restoreReplayAdapter(combo, false, 0x8086);
        for (const auto *command : {"replay", "buffer", "texture", "statistics", "texture-storage",
                                   "geometry", "post-geometry", "replay-pipeline", "predicate",
                                   "shader-checkpoint", "draw-resources", "coverage", "quad", "timings"})
            QCOMPARE(replayAdapterArguments(combo, command), QStringList({"--gpu-vendor", "32902"}));
        for (const auto *command : {"shader", "compile", "assemble", "compile-project", "inventory",
                                   "metric-profile", "metric-iterations", "metric-catalog"})
            QVERIFY(replayAdapterArguments(combo, command).empty());
        restoreReplayAdapter(combo, true, {});
        QCOMPARE(combo.currentIndex(), 1);
        QCOMPARE(replayAdapterArguments(combo, "replay"), QStringList({"--warp"}));
        QVERIFY(!selectedReplayVendor(combo));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, restoreReplayAdapter(combo, true, 0x8086));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, restoreReplayAdapter(combo, false, 0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, restoreReplayAdapter(combo, false, 65536));
    }
    void devicesProjectsAndFailure() {
        QTemporaryDir dir;
        const auto path = dir.filePath("adapter.gpa_frame");
        testing::msaaOutputCapture(false).save(path);
        MainWindow window;
        window.show();
        auto combo = window.findChild<QComboBox *>("replayAdapter");
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(combo && output);
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        const auto expected = QByteArray::fromHex("1e0000ff").repeated(7 * 5);
        QCOMPARE(rgba(output->image()), expected);
        const auto count = combo->count();
        if (count <= 2) QSKIP("No enumerated hardware vendor on this host");
        for (int index = 2; index < count; ++index) {
            done.clear();
            combo->setCurrentIndex(index);
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            const auto description = combo->itemData(index, Qt::ToolTipRole).toString().section('\n', 0, 0);
            QVERIFY2(window.statusBar()->currentMessage().startsWith(description),
                     qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(rgba(output->image()), expected);
            qInfo() << "Verified native GUI adapter" << description;
        }
        const auto selected = selectedReplayVendor(*combo);
        QVERIFY(selected);
        const auto project = dir.filePath("adapter.json");
        fileAction(window, "saveExperiment", project);
        qInfo() << "Saved vendor project" << *selected;
        QCOMPARE(readJson(project)["ui"]["flora_gpu_vendor"], Json(*selected));
        // A metadata-only shader read must not receive the replay device option.
        auto resources = window.findChild<QTableView *>("resources");
        QVERIFY(resources);
        done.clear();
        bool found = false;
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() != 10) continue;
            resources->setCurrentIndex(index); found = true; break;
        }
        QVERIFY(found);
        auto readShader = window.findChild<QAction *>("readShaderAssembly");
        QVERIFY(readShader && readShader->isEnabled());
        readShader->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        qInfo() << "Shader metadata read completed with vendor selected";
        done.clear();
        combo->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(rgba(output->image()), expected);
        done.clear();
        fileAction(window, "openExperiment", project);
        qInfo() << "Restored vendor project";
        QCOMPARE(selectedReplayVendor(*combo), selected);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(rgba(output->image()), expected);
        uint32_t missing = 0xffff;
        while (combo->findData(missing) >= 0) --missing;
        done.clear();
        restoreReplayAdapter(*combo, false, missing);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QVERIFY(window.statusBar()->currentMessage().contains("No DXGI adapter for vendor"));
        QCOMPARE(selectedReplayVendor(*combo), std::optional<uint32_t>(missing));
        done.clear();
        restoreReplayAdapter(*combo, false, selected);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(rgba(output->image()), expected);
    }
    void acceptedFrames() {
        const auto input = qEnvironmentVariable("FLORA_ADAPTER_UI_MANIFEST");
        if (input.isEmpty()) QSKIP("Set FLORA_ADAPTER_UI_MANIFEST for preserved capture GUI validation");
        const auto request = readJson(input);
        const auto destination = qEnvironmentVariable("FLORA_ADAPTER_UI_OUTPUT");
        QVERIFY(!destination.isEmpty() && !QFileInfo::exists(destination));
        QVERIFY(QDir().mkpath(destination));
        Json results = Json::array();
        for (const auto &frame : request.at("frames")) {
            MainWindow window;
            window.show();
            auto combo = window.findChild<QComboBox *>("replayAdapter");
            auto output = window.findChild<ImageView *>("frameOutput");
            const auto vendor = request.at("vendor").get<uint32_t>();
            QVERIFY(combo && output && combo->findData(vendor) >= 0);
            restoreReplayAdapter(*combo, false, vendor);
            const auto description = combo->currentData(Qt::ToolTipRole).toString().section('\n', 0, 0);
            QSignalSpy done(&window, &MainWindow::taskFinished);
            window.openCapture(QString::fromStdString(frame.at("path").get<std::string>()));
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 180000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            QVERIFY(window.statusBar()->currentMessage().startsWith(description));
            const auto pixels = rgba(output->image());
            const auto hash = QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex();
            QCOMPARE(hash.toStdString(), frame.at("rgba_sha256").get<std::string>());
            const auto name = QString::number(results.size());
            QFile bytes(destination + '/' + name + ".rgba");
            QVERIFY(bytes.open(QIODevice::WriteOnly));
            QCOMPARE(bytes.write(pixels), pixels.size());
            results.push_back({{"frame", frame.at("path")}, {"rgba_sha256", hash.toStdString()},
                               {"selected_vendor", vendor}, {"actual_adapter", description.toStdString()},
                               {"width", output->image().width()}, {"height", output->image().height()}});
            QFile report(destination + "/results.json");
            QVERIFY(report.open(QIODevice::WriteOnly));
            const auto json = QByteArray::fromStdString(results.dump(2));
            QCOMPARE(report.write(json), json.size());
            qInfo() << name << description << hash;
        }
        QCOMPARE(results.size(), request.at("frames").size());
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setApplicationName("FloraReplayAdapterUiTests");
    app.setOrganizationName("FloraGPA.Tests");
    applyAppearance(app);
    ReplayAdapterUiTests test;
    return QTest::qExec(&test, argc, argv);
}
#include "ReplayAdapterUiTests.moc"
