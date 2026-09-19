#include "app/MainWindow.h"
#include <QAbstractItemModelTester>
#include <QAction>
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTabWidget>
#include <QtTest>

class UiTests final : public QObject {
    Q_OBJECT
  private slots:
    void modelsAndSelection() {
        const auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("Set FLORA_TEST_CAPTURE_DIR to enable capture interaction tests");
        auto frame = std::make_shared<flora::Frame>(
            std::filesystem::path((captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString()));
        flora::CaptureModel model(flora::CaptureModel::Kind::Commands);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setFrame(frame);
        QCOMPARE(model.rowCount(), 920);
        QCOMPARE(model.idAt(model.rowOf(1455)), flora::Id(1455));
        flora::CaptureFilter filter;
        filter.setSourceModel(&model);
        filter.workOnly = true;
        filter.refresh();
        QCOMPARE(filter.rowCount(), 75);
        filter.setFilterKeyColumn(-1);
        filter.setFilterFixedString("DrawIndexed");
        QCOMPARE(filter.rowCount(), 72);
    }
    void replayAndNavigate() {
        const auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("Set FLORA_TEST_CAPTURE_DIR to enable GPU interaction tests");
        flora::MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = window.findChild<flora::ImageView *>("frameOutput");
        QVERIFY(image);
        auto pixels = image->image().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(pixels.size(), QSize(2560, 1440));
        QByteArray data(reinterpret_cast<const char *>(pixels.constBits()), pixels.sizeInBytes());
        QCOMPARE(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(),
                 QByteArray("2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"));
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        api->setCurrentIndex(api->model()->index(2, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto pipeline = window.findChild<QTreeWidget *>("pipeline");
        QVERIFY(pipeline->topLevelItemCount() >= 8);
        done.clear();
        window.replay(true);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!window.busy());
    }
    void textureAndShaderExperiment() {
        const auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::MainWindow window;
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = window.findChild<flora::ImageView *>("frameOutput");
        auto baseline = image->image();
        auto resources = window.findChild<QTableView *>("resources");
        QVERIFY(resources);
        auto select = [&](flora::Id id) {
            for (int row = 0; row < resources->model()->rowCount(); ++row) {
                auto index = resources->model()->index(row, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) {
                    resources->setCurrentIndex(index);
                    return true;
                }
            }
            return false;
        };
        done.clear();
        QVERIFY(select(123));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(window.findChild<flora::ImageView *>("textureOutput")->image().size(), QSize(128, 128));
        QVERIFY(select(102));
        auto source = window.findChild<QPlainTextEdit *>("shaderSource");
        QVERIFY(source);
        source->setPlainText("float4 main() : SV_Target { return float4(1, 0, 1, 1); }");
        QAction *compile = nullptr, *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->text() == "Compile && Apply")
                compile = action;
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(compile);
        QVERIFY(undo);
        QVERIFY(redo);
        done.clear();
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY(result[0].toBool());
        auto edited = image->image();
        QVERIFY(edited != baseline);
        QVERIFY(undo->isEnabled());
        auto reflection = window.findChild<QTreeWidget *>("shaderReflection");
        QVERIFY(reflection);
        QVERIFY(window.findChild<QPlainTextEdit *>("shader")->toPlainText().contains("ps_4_0"));
        done.clear();
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image(), baseline);
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image(), edited);
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"
