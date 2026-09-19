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
    void apiInspection() {
        const auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        auto select = [&](qulonglong id) {
            for (int i = 0; i < api->model()->rowCount(); ++i) {
                auto index = api->model()->index(i, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) {
                    api->setCurrentIndex(index);
                    return true;
                }
            }
            return false;
        };
        QVERIFY(select(79));
        auto props = window.findChild<QTreeWidget *>("properties");
        QTreeWidgetItem *fields = nullptr, *references = nullptr;
        for (int i = 0; i < props->topLevelItemCount(); ++i) {
            auto item = props->topLevelItem(i);
            if (item->data(0, Qt::UserRole + 1) == "apiFields")
                fields = item;
            if (item->data(0, Qt::UserRole + 1) == "apiReferences")
                references = item;
        }
        QVERIFY(fields && references);
        QCOMPARE(fields->childCount(), 8);
        QCOMPARE(fields->child(2)->text(0), QString("view"));
        QCOMPARE(fields->child(2)->text(1), QString("75"));
        QVERIFY(fields->child(2)->toolTip(0).contains("Offset 16"));
        snapshot(window, "api-fields");
        QTreeWidgetItem *view = nullptr;
        for (int i = 0; i < references->childCount(); ++i)
            if (references->child(i)->text(0) == "view")
                view = references->child(i);
        QVERIFY(view);
        done.clear();
        QVERIFY(QMetaObject::invokeMethod(props, "itemDoubleClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, view), Q_ARG(int, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!window.findChild<flora::ImageView *>("textureOutput")->image().isNull());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto filter = window.findChild<QLineEdit *>("apiResourceFilter");
        filter->setText("80");
        QVERIFY(api->model()->rowCount() > 0);
        QVERIFY(api->model()->rowCount() < 920);
        auto proxy = static_cast<flora::CaptureFilter *>(api->model());
        auto source = static_cast<flora::CaptureModel *>(proxy->sourceModel());
        for (int i = 0; i < api->model()->rowCount(); ++i)
            QVERIFY(flora::commandMatches(
                source->command(api->model()->index(i, 0).data(Qt::UserRole).toULongLong()), "", 80));
        filter->setText("0");
        QCOMPARE(api->model()->rowCount(), 0);
        filter->clear();
        QCOMPARE(api->model()->rowCount(), 920);
        QVERIFY(window.findChild<QAction *>("exportApiLog"));
    }
    void counterEditorHistory() {
        auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(captures + "/bf1_2026_01_21__16_53_05.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        auto select = [](QTableView *table, qulonglong id) {
            for (int i = 0; i < table->model()->rowCount(); ++i) {
                auto index = table->model()->index(i, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) {
                    table->setCurrentIndex(index);
                    return true;
                }
            }
            return false;
        };
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(select(api, 25784));
        QVERIFY(select(window.findChild<QTableView *>("resources"), 25733));
        auto boundary = window.findChild<QComboBox *>("bufferBoundary");
        boundary->setCurrentIndex(1);
        // Buffer navigation must take precedence over the queued output preview.
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        window.findChild<QTabWidget *>("bufferTabs")->setCurrentIndex(2);
        auto counters = window.findChild<QTreeWidget *>("uavCounters");
        QCOMPARE(counters->topLevelItemCount(), 1);
        auto original = counters->topLevelItem(0)->text(3);
        QCOMPARE(counters->topLevelItem(0)->text(0), QString("25732"));
        counters->setCurrentItem(counters->topLevelItem(0));
        auto edit = window.findChild<QAction *>("editCounter");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("counterDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto field = dialog->findChild<QLineEdit *>("counterValue");
            field->setText("0xffffffff");
            snapshot(*dialog, "counter-dialog");
            field->setText("4294967296");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            QTest::mouseClick(apply, Qt::LeftButton);
            QVERIFY(dialog->isVisible());
            field->setText("0xffffffff");
            entered = true;
            QTest::mouseClick(apply, Qt::LeftButton);
        });
        edit->trigger();
        QVERIFY(entered);
        QVERIFY(!edit->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(counters->topLevelItem(0)->text(3), QString("4294967295"));
        snapshot(window, "counter-fields");
        QAction *undo = nullptr, *redo = nullptr;
        for (auto a : window.findChildren<QAction *>()) {
            if (a->shortcut() == QKeySequence::Undo)
                undo = a;
            if (a->shortcut() == QKeySequence::Redo)
                redo = a;
        }
        QVERIFY(undo && redo);
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(counters->topLevelItem(0)->text(3), original);
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(counters->topLevelItem(0)->text(3), QString("4294967295"));
        boundary->setCurrentIndex(2);
        QVERIFY(!edit->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        counters->setCurrentItem(counters->topLevelItem(0));
        QVERIFY(!edit->isEnabled());
        QVERIFY(counters->topLevelItem(0)->text(3) != QString("4294967295"));
        QVERIFY(select(api, 25848));
        QCOMPARE(counters->topLevelItemCount(), 0);
    }
    void bufferEditorHistory() {
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
        api->setCurrentIndex(api->model()->index(2, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto resources = window.findChild<QTableView *>("resources");
        for (int i = 0; i < resources->model()->rowCount(); ++i) {
            auto index = resources->model()->index(i, 0);
            if (index.data(Qt::UserRole).toULongLong() == 104) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto boundary = window.findChild<QComboBox *>("bufferBoundary");
        boundary->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto model =
            static_cast<flora::BufferModel *>(window.findChild<QTableView *>("bufferTable")->model());
        auto original = model->bytes();
        auto edit = window.findChild<QAction *>("editBuffer");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("bufferEditDialog");
            if (!dialog)
                return;
            dialog->findChild<QLineEdit *>("bufferEditOffset")->setText("0x0");
            dialog->findChild<QPlainTextEdit *>("bufferEditHex")->setPlainText("00 00 20 41");
            snapshot(*dialog, "buffer-edit-dialog");
            entered = true;
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok),
                              Qt::LeftButton);
        });
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto edited = model->bytes();
        QCOMPARE(edited.left(4), QByteArray::fromHex("00002041"));
        QVERIFY(edited != original);
        QCOMPARE(edited.mid(4), original.mid(4));
        snapshot(window, "buffer-edited");
        QAction *undo = nullptr, *redo = nullptr;
        for (auto a : window.findChildren<QAction *>()) {
            if (a->shortcut() == QKeySequence::Undo)
                undo = a;
            if (a->shortcut() == QKeySequence::Redo)
                redo = a;
        }
        QVERIFY(undo && redo);
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), original);
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), edited);
        boundary->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), original);
    }
    void constantEditorHistory() {
        auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External capture fixtures are not configured");
        flora::MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(2, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto resources = window.findChild<QTableView *>("resources");
        // Original GF2 shaders have stripped RDEF. Compile a reflected replacement first.
        for (int i = 0; i < resources->model()->rowCount(); ++i) {
            auto index = resources->model()->index(i, 0);
            if (index.data(Qt::UserRole).toULongLong() == 102) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        window.findChild<QPlainTextEdit *>("shaderSource")
            ->setPlainText("cbuffer Fields:register(b0){row_major float2x3 basis; float4 extra;} "
                           "float4 main():SV_Target{return float4(basis[0],extra.w);}");
        QAction *compile = nullptr;
        for (auto a : window.findChildren<QAction *>())
            if (a->text() == "Compile && Apply")
                compile = a;
        QVERIFY(compile);
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY(result[0].toBool());
        done.clear();
        for (int i = 0; i < resources->model()->rowCount(); ++i) {
            auto index = resources->model()->index(i, 0);
            if (index.data(Qt::UserRole).toULongLong() == 104) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto boundary = window.findChild<QComboBox *>("bufferBoundary");
        boundary->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto fields = window.findChild<QTreeWidget *>("constantFields");
        QVERIFY(fields && fields->topLevelItemCount() > 0);
        QTreeWidgetItem *chosen = nullptr;
        for (QTreeWidgetItemIterator it(fields); *it; ++it) {
            auto field = (*it)->data(0, Qt::UserRole).toJsonObject();
            if (field["status"].toString() == "ready" && field["base_type"].toInt() == 3 &&
                field["columns"].toInt() > 1) {
                chosen = *it;
                break;
            }
        }
        QVERIFY(chosen);
        fields->setCurrentItem(chosen);
        window.findChild<QTabWidget *>("bufferTabs")->setCurrentIndex(1);
        auto offsets = chosen->data(0, Qt::UserRole).toJsonObject()["component_offsets"].toArray();
        auto model =
            static_cast<flora::BufferModel *>(window.findChild<QTableView *>("bufferTable")->model());
        auto original = model->bytes(), expected = original;
        float first = 13, last = 17;
        std::memcpy(expected.data() + offsets.first().toInteger(), &first, 4);
        std::memcpy(expected.data() + offsets.last().toInteger(), &last, 4);
        auto edit = window.findChild<QAction *>("editConstant");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("constantDialog");
            if (!dialog)
                return;
            dialog->findChild<QLineEdit *>("constantValue0")->setText("13");
            dialog->findChild<QLineEdit *>(QString("constantValue%1").arg(offsets.size() - 1))->setText("17");
            snapshot(*dialog, "constant-dialog");
            entered = true;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok),
                              Qt::LeftButton);
        });
        edit->trigger();
        QVERIFY(entered);
        QVERIFY(!edit->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), expected);
        snapshot(window, "constant-fields");
        QAction *undo = nullptr, *redo = nullptr;
        for (auto a : window.findChildren<QAction *>()) {
            if (a->shortcut() == QKeySequence::Undo)
                undo = a;
            if (a->shortcut() == QKeySequence::Redo)
                redo = a;
        }
        QVERIFY(undo && redo);
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), original);
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), expected);
        boundary->setCurrentIndex(2);
        QVERIFY(!edit->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(model->bytes(), original);
        fields->setCurrentItem(fields->topLevelItem(0)->child(0));
        QVERIFY(!edit->isEnabled());
        api->setCurrentIndex(api->model()->index(3, 0));
        QCOMPARE(fields->topLevelItemCount(), 0);
    }
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
