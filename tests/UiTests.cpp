#include "app/Appearance.h"
#include "app/MainWindow.h"
#include <QAbstractItemModelTester>
#include <QAction>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QtTest>

class UiTests final : public QObject {
    Q_OBJECT
    void snapshot(QWidget &window, const QString &name) {
        auto directory = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (directory.isEmpty())
            return;
        QVERIFY(QDir().mkpath(directory));
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(directory + '/' + name + ".png"));
    }
  private slots:
    void clearEditorHistory() {
        auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::MainWindow window;
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        api->setCurrentIndex(api->model()->index(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 80) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = window.findChild<flora::ImageView *>("textureOutput");
        auto baseline = image->image();
        auto edit = window.findChild<QAction *>("editClear");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("clearDialog");
            if (!dialog)
                return;
            for (int i = 0; i < 4; ++i)
                dialog->findChild<QLineEdit *>(QString("clearValue%1").arg(i))->setText(i == 1 ? "0" : "1");
            snapshot(*dialog, "clear-dialog");
            entered = true;
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok),
                              Qt::LeftButton);
        });
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto edited = image->image();
        QVERIFY(edited != baseline);
        QCOMPARE(edited.pixelColor(0, 0), QColor(255, 0, 255));
        snapshot(window, "clear-edited");
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image(), baseline);
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image(), edited);
    }
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA-Tests");
        QCoreApplication::setApplicationName("FloraGPA-Tests");
        flora::applyAppearance(*qApp);
    }
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
        QAction *inspect = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Inspect IA")
                inspect = action;
        QVERIFY(inspect);
        // An explicit analysis request wins over pending navigation debounce.
        api->setCurrentIndex(api->model()->index(3, 0));
        api->setCurrentIndex(api->model()->index(2, 0));
        done.clear();
        inspect->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto geometry = window.findChild<QTableView *>("geometryTable");
        QVERIFY(geometry);
        QAbstractItemModelTester geometryTester(geometry->model(),
                                                QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(geometry->model()->rowCount(), 6);
        QCOMPARE(geometry->model()->index(0, 4).data().toString(), QString("-900"));
        snapshot(window, "geometry");
        done.clear();
        window.replay(true);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!window.busy());
        auto resources = window.findChild<QTableView *>("resources");
        QVERIFY(resources);
        done.clear();
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 104) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        window.findChild<QComboBox *>("bufferBoundary")->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto buffer = window.findChild<QTableView *>("bufferTable");
        QVERIFY(buffer);
        auto bufferModel = static_cast<flora::BufferModel *>(buffer->model());
        auto full = bufferModel->bytes();
        QCOMPARE(QCryptographicHash::hash(full, QCryptographicHash::Sha256).toHex(),
                 QByteArray("70dd1160191c1a5ddf797256e2b2f63d6bdaa8281fcb8c3409054f7688d0f5fc"));
        window.findChild<QComboBox *>("bufferMode")->setCurrentIndex(1);
        QCOMPARE(bufferModel->rowCount(), 44);
        QCOMPARE(bufferModel->columnCount(), 5);
        window.findChild<QLineEdit *>("bufferOffset")->setText("3");
        window.findChild<QLineEdit *>("bufferLength")->setText("17");
        QAction *read = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Read")
                read = action;
        QVERIFY(read);
        done.clear();
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(bufferModel->bytes(), full.mid(3, 17));
        QCOMPARE(bufferModel->index(0, 0).data().toString(), QString("00000003"));
        snapshot(window, "buffer");
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
        auto originalAssembly = window.findChild<QPlainTextEdit *>("shader")->toPlainText();
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
        QVERIFY(window.findChild<QPlainTextEdit *>("shader")->toPlainText() != originalAssembly);
        snapshot(window, "shader");
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
        QCOMPARE(window.findChild<QPlainTextEdit *>("shader")->toPlainText(), originalAssembly);
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image(), edited);
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"
