#include "ClassCapture.h"
#include "ConstantBufferCapture.h"
#include "DepthStencilCapture.h"
#include "IaSetterCapture.h"
#include "MsaaCapture.h"
#include "PipelineSetterCapture.h"
#include "PredicateCapture.h"
#include "SamplerCapture.h"
#include "SrvBindingCapture.h"
#include "SrvCapture.h"
#include "StateCapture.h"
#include "StreamCapture.h"
#include "SyntheticCapture.h"
#include "TextureEditCapture.h"
#include "app/Appearance.h"
#include "app/BlendDialog.h"
#include "app/CommandStateView.h"
#include "app/ConstantBufferDialog.h"
#include "app/IaSetterDialog.h"
#include "app/MainWindow.h"
#include "app/OutputDialog.h"
#include "app/PipelineSetterDialog.h"
#include "app/PredicateView.h"
#include "app/RasterizerDialog.h"
#include "app/SamplerDialog.h"
#include "app/SrvDialog.h"
#include "app/ViewDialog.h"
#include "application/BlendEdits.h"
#include "application/OutputEdits.h"
#include "application/RasterizerEdits.h"
#include "application/SamplerEdits.h"
#include "application/SetterEdits.h"
#include "application/SrvEdits.h"
#include "application/ViewEdits.h"
#include <QAbstractItemModelTester>
#include <QAction>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QtTest>

class UiTests final : public QObject {
    Q_OBJECT
    void projectFile(flora::MainWindow &window, const char *actionName, const QString &path) {
        const bool native = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        auto restore =
            qScopeGuard([&] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native); });
        bool handled = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->selectFile(path);
            handled = true;
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>(actionName)->trigger();
        QVERIFY(handled);
    }
    void snapshot(QWidget &window, const QString &name) {
        auto directory = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (directory.isEmpty())
            return;
        QVERIFY(QDir().mkpath(directory));
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(directory + '/' + name + ".png"));
    }
  private slots:
    void iaSetterDialogControls() {
        using namespace flora;
        using Json = nlohmann::json;
        QTemporaryDir dir;
        auto capture = testing::iaSetterCapture();
        capture.buffer(UINT64_MAX - 1, UINT64_MAX - 2, D3D11_BIND_VERTEX_BUFFER, 0, {});
        capture.save(dir.path() + "/ia.gpa_frame");
        Frame frame((dir.path() + "/ia.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("iaSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto rows = dialog->findChild<QTableWidget *>("ia_rows");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            rows->item(0, 1)->setText("2049");
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("ia_error")->isVisible());
            rows->item(0, 1)->setText("2048");
            rows->item(0, 2)->setText("0xffffffff");
            auto box = qobject_cast<QComboBox *>(rows->cellWidget(0, 0));
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(UINT64_MAX - 1))));
            rows->setCurrentCell(1, 0);
            dialog->findChild<QPushButton *>("ia_remove")->click();
            dialog->findChild<QSpinBox *>("ia_start")->setValue(31);
            dialog->findChild<QPushButton *>("ia_add")->click();
            QCOMPARE(rows->rowCount(), 1);
            snapshot(*dialog, "ia-vertex-setter");
            apply->click();
        });
        QVERIFY(editIaSetterDialog(&parent, frame, 90, capturedSetter(frame, 90), [&](const auto &values) {
            ++commits;
            QCOMPARE(values.at("start_slot"), Json(31));
            QCOMPARE(values.at("buffers")[0], Json(UINT64_MAX - 1));
            QCOMPARE(values.at("offsets")[0], Json(UINT32_MAX));
        }));
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("iaSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(editIaSetterDialog(&parent, frame, 92, capturedSetter(frame, 92), [&](const auto &values) {
            ++commits;
            QCOMPARE(values.at("input_layout"), Json(0));
        }));
        QCOMPARE(commits, 2);
    }
    void pipelineSetterDialogControls() {
        using namespace flora;
        using Json = nlohmann::json;
        QTemporaryDir dir;
        auto capture = testing::pipelineSetterCapture();
        capture.add(UINT64_MAX - 1, 5, 0x89,
                    testing::statePack(Id(0), Id(0), 3u, 1u, 0u, 0, 0.f, 0.f, 1u, 0u, 0u, 0u));
        capture.save(dir.path() + "/pipeline.gpa_frame");
        Frame frame((dir.path() + "/pipeline.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("pipelineSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto mask = dialog->findChild<QLineEdit *>("ps_sample_mask");
            mask->setText("4294967296");
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("ps_error")->isVisible());
            mask->setText("0xffffffff");
            dialog->findChild<QTableWidget *>("ps_rows")->item(0, 0)->setText("-1");
            snapshot(*dialog, "pipeline-blend-setter");
            apply->click();
        });
        QVERIFY(editPipelineSetterDialog(&parent, frame, 91, capturedSetter(frame, 91), [&](const auto &v) {
            ++commits;
            QCOMPARE(v.at("sample_mask"), Json(UINT32_MAX));
            QCOMPARE(v.at("blend_factor")[0], Json(-1.));
        }));
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("pipelineSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto rows = dialog->findChild<QTableWidget *>("ps_rows");
            rows->item(0, 2)->setText("-1");
            apply->click();
            QCOMPARE(commits, 1);
            QVERIFY(dialog->findChild<QLabel *>("ps_error")->isVisible());
            rows->item(0, 2)->setText("2");
            dialog->findChild<QPushButton *>("ps_add")->click();
            QCOMPARE(rows->rowCount(), 2);
            snapshot(*dialog, "pipeline-viewport-setter");
            apply->click();
        });
        QVERIFY(editPipelineSetterDialog(&parent, frame, 94, capturedSetter(frame, 94), [&](const auto &v) {
            ++commits;
            QCOMPARE(v.at("viewports_values").size(), size_t(2));
        }));
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("pipelineSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto box = dialog->findChild<QComboBox *>("ps_rasterizer");
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(UINT64_MAX - 1))));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(editPipelineSetterDialog(&parent, frame, 93, capturedSetter(frame, 93), [&](const auto &v) {
            ++commits;
            QCOMPARE(v.at("rasterizer").template get<Id>(), UINT64_MAX - 1);
        }));
        QCOMPARE(commits, 3);
    }
    void pipelineSetterWorkerHistory() {
        using namespace flora;
        auto capture = testing::depthStencilCapture();
        capture.add(900, 7, 0x350a,
                    testing::statePack(Id(0), Id(1), 1u, uint8_t(1), 0.f, 0.f, 1.f, 1.f, 0.f, 1.f));
        QTemporaryDir dir;
        capture.save(dir.path() + "/pipeline.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/pipeline.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(output);
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 900) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto boundary = window.findChild<QComboBox *>("outputBoundary");
        boundary->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("pipelineSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto rows = dialog->findChild<QTableWidget *>("ps_rows");
            rows->setCurrentCell(0, 0);
            dialog->findChild<QPushButton *>("ps_remove")->click();
            QCOMPARE(rows->rowCount(), 0);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        for (auto action : {undo, redo}) {
            done.clear();
            action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY(done.takeLast()[0].toBool());
            QCOMPARE(output->image().pixelColor(0, 0),
                     action == undo ? QColor(255, 0, 0, 255) : QColor(0, 0, 0, 255));
        }
        snapshot(window, "pipeline-setter-workspace");
    }
    void constantBufferDialogControls() {
        using namespace flora;
        QTemporaryDir dir;
        auto capture = testing::constantBufferCapture();
        capture.buffer(UINT64_MAX - 1, UINT64_MAX - 2, 4, 0, {});
        capture.save(dir.path() + "/cb.gpa_frame");
        Frame frame((dir.path() + "/cb.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("constantBufferDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto rows = dialog->findChild<QTableWidget *>("cb_rows");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            rows->item(0, 1)->setText("1");
            apply->click();
            QVERIFY(dialog->findChild<QLabel *>("cb_error")->isVisible());
            QCOMPARE(commits, 0);
            rows->item(0, 1)->setText("0xfffffff0");
            rows->item(0, 2)->setText("4096");
            auto box = qobject_cast<QComboBox *>(rows->cellWidget(0, 0));
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(UINT64_MAX - 1))));
            rows->setCurrentCell(1, 0);
            dialog->findChild<QPushButton *>("cb_remove")->click();
            dialog->findChild<QSpinBox *>("cb_start")->setValue(13);
            dialog->findChild<QPushButton *>("cb_add")->click();
            QCOMPARE(rows->rowCount(), 1);
            auto window = dialog->findChild<QCheckBox *>("cb_window");
            window->setChecked(false);
            QVERIFY(rows->isColumnHidden(1));
            window->setChecked(true);
            QVERIFY(!rows->isColumnHidden(1));
            snapshot(*dialog, "constant-buffer-range-editor");
            apply->click();
        });
        QVERIFY(editConstantBufferDialog(&parent, frame, 90, capturedSetter(frame, 90), [&](const auto &v) {
            ++commits;
            QCOMPARE(v.at("buffers")[0].template get<Id>(), UINT64_MAX - 1);
            QCOMPARE(v.at("first_constants")[0].template get<uint32_t>(), 0xfffffff0u);
        }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("constantBufferDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QCheckBox *>("cb_window")->setChecked(false);
            snapshot(*dialog, "constant-buffer-whole-editor");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(editConstantBufferDialog(&parent, frame, 90, capturedSetter(frame, 90), [&](const auto &v) {
            ++commits;
            QVERIFY(v.at("first_constants").is_null());
            QVERIFY(v.at("constant_counts").is_null());
        }));
        QCOMPARE(commits, 2);
    }
    void constantBufferMissingWindowArray() {
        using namespace flora;
        QTemporaryDir dir;
        auto capture = testing::constantBufferCapture();
        capture.add(96, 7, 0x253, testing::cbSetter(0x253, 2, {60}, std::vector<uint32_t>{16}, std::nullopt));
        capture.add(97, 7, 0x253, testing::cbSetter(0x253, 2, {60}, std::nullopt, std::vector<uint32_t>{16}));
        capture.save(dir.path() + "/partial.gpa_frame");
        Frame frame((dir.path() + "/partial.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        for (Id event : {96, 97}) {
            QTimer::singleShot(0, &parent, [&] {
                auto dialog = parent.findChild<QDialog *>("constantBufferDialog");
                QVERIFY(dialog);
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                QVERIFY(dialog->findChild<QCheckBox *>("cb_window")->isChecked());
                auto rows = dialog->findChild<QTableWidget *>("cb_rows");
                auto missing = rows->item(0, event == 96 ? 2 : 1);
                QVERIFY(missing->text().isEmpty());
                auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
                apply->click();
                QVERIFY(dialog->findChild<QLabel *>("cb_error")->isVisible());
                missing->setText("16");
                apply->click();
            });
            QVERIFY(editConstantBufferDialog(
                &parent, frame, event, capturedSetter(frame, event), [&](const auto &v) {
                    ++commits;
                    QCOMPARE(v.at("first_constants"), nlohmann::json::array({16}));
                    QCOMPARE(v.at("constant_counts"), nlohmann::json::array({16}));
                }));
        }
        QCOMPARE(commits, 2);
    }
    void constantBufferWorkerHistory() {
        using namespace flora;
        QTemporaryDir dir;
        testing::constantBufferCapture().save(dir.path() + "/cb.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/cb.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(output);
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 0));
        auto api = window.findChild<QTableView *>("apiLog");
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        bool selected = false;
        done.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 90) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("constantBufferDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto rows = dialog->findChild<QTableWidget *>("cb_rows");
            for (int i = 0; i < 2; ++i) {
                auto box = qobject_cast<QComboBox *>(rows->cellWidget(i, 0));
                box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(60 + 2 * i))));
                rows->item(i, 1)->setText("16");
                rows->item(i, 2)->setText("16");
            }
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(64, 32, 0, 0));
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        for (auto action : {undo, redo}) {
            done.clear();
            action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY(done.takeLast()[0].toBool());
            QCOMPARE(output->image().pixelColor(0, 0),
                     action == undo ? QColor(0, 0, 0, 0) : QColor(64, 32, 0, 0));
        }
        snapshot(window, "constant-buffer-workspace");
    }
    void iaSetterWorkerHistory() {
        using namespace flora;
        auto capture = testing::iaSetterCapture();
        std::erase_if(capture.entries, [](const Entry &e) { return e.id == 200 || e.id == 300; });
        for (const auto &e : capture.entries) {
            if (e.id == 34) {
                testing::put(capture.bytes, e.offset + 128, Id(66));
                testing::put(capture.bytes, e.offset + 136, 42u);
                testing::put(capture.bytes, e.offset + 140, 0u);
            }
            if (e.id == 67) {
                testing::put(capture.bytes, e.offset + 4, 0u);
                testing::put(capture.bytes, e.offset + 8, 1u);
                testing::put(capture.bytes, e.offset + 12, 2u);
            }
        }
        capture.add(195, 7, 0x32, testing::statePack(Id(0), Id(1), Id(21), uint8_t(1), 0.f, 0.f, 0.f, 1.f));
        capture.add(200, 7, 0x39, testing::statePack(Id(34), Id(0), Id(1), 3u, 0u, 0));
        QTemporaryDir dir;
        capture.save(dir.path() + "/pipeline.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/pipeline.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(output);
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 91) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto boundary = window.findChild<QComboBox *>("outputBoundary");
        boundary->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("iaSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QComboBox *>("ia_resource")->setCurrentIndex(0);
            dialog->findChild<QComboBox *>("ia_format")->setCurrentIndex(0);
            dialog->findChild<QLineEdit *>("ia_offset")->setText("0");
            snapshot(*dialog, "ia-index-setter");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        for (auto action : {undo, redo}) {
            done.clear();
            action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY(done.takeLast()[0].toBool());
            QCOMPARE(output->image().pixelColor(0, 0),
                     action == undo ? QColor(255, 0, 0, 255) : QColor(0, 0, 0, 255));
        }
        snapshot(window, "ia-setter-workspace");
    }
    void shaderSetterWorkerHistory() {
        using namespace flora;
        auto capture = testing::graphicsClassCapture();
        auto program = testing::compileClassProgram(
            "interface I{uint apply(uint x);};class A:I{uint value;uint apply(uint x){return x+value;}};"
            "class B:I{uint value;uint apply(uint x){return x+value*2;}};"
            "cbuffer Classes:register(b0){A first[2];B second[2];} I selected;"
            "float4 main():SV_Target{return float4(0,selected.apply(0)==20?1:0,0,1);}",
            "ps_5_0");
        capture.add(1002, 5, 0x92, testing::statePack(Id(0), Id(0), Id(0), Id(0), Id(60), Id(0), Id(1003)));
        auto code = testing::statePack(uint64_t(program.size()));
        code.insert(code.end(), program.begin(), program.end());
        testing::append(code, Id(0));
        capture.add(1003, 9, 0x81, code);
        capture.add(90, 7, 0x34e7, testing::statePack(Id(0), Id(1), Id(32), 1u, uint8_t(1), Id(62)));
        QTemporaryDir dir;
        capture.save(dir.path() + "/pipeline.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/pipeline.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(output);
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 90) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto boundary = window.findChild<QComboBox *>("outputBoundary");
        boundary->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("pipelineSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto rows = dialog->findChild<QTableWidget *>("ps_rows");
            QCOMPARE(rows->rowCount(), 1);
            rows->item(0, 0)->setText("18446744073709551616");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            apply->click();
            QVERIFY(dialog->findChild<QLabel *>("ps_error")->isVisible());
            rows->item(0, 0)->setText("0x3e");
            auto shaders = dialog->findChild<QComboBox *>("ps_shader");
            shaders->setCurrentIndex(shaders->findData(QVariant::fromValue(qulonglong(1002))));
            snapshot(*dialog, "shader-setter-dialog");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 255, 0, 255));
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        for (auto action : {undo, redo}) {
            done.clear();
            action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY(done.takeLast()[0].toBool());
            QCOMPARE(output->image().pixelColor(0, 0),
                     action == undo ? QColor(255, 0, 0, 255) : QColor(0, 255, 0, 255));
        }
        // The effective stage is the resource opened by pipeline navigation and compilation.
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 200) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto pipeline = window.findChild<QTreeWidget *>("pipeline");
        auto ps = pipeline->findItems("PS", Qt::MatchExactly).value(0);
        QVERIFY(ps);
        QCOMPARE(ps->data(1, Qt::UserRole).toULongLong(), qulonglong(1002));
        QVERIFY(ps->childCount() >= 2);
        QMetaObject::invokeMethod(pipeline, "itemDoubleClicked", Qt::DirectConnection,
                                  Q_ARG(QTreeWidgetItem *, ps), Q_ARG(int, 1));
        auto source = window.findChild<QPlainTextEdit *>("shaderSource");
        source->setPlainText("float4 main():SV_Target{return float4(0,0,1,1);}");
        QAction *compile = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Compile && Apply")
                compile = action;
        QVERIFY(compile);
        done.clear();
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY(result[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 255, 255));
        done.clear();
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 255, 0, 255));
        done.clear();
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        ps = pipeline->findItems("PS", Qt::MatchExactly).value(0);
        QCOMPARE(ps->data(1, Qt::UserRole).toULongLong(), qulonglong(32));
        auto properties = window.findChild<QTreeWidget *>("properties");
        auto ids = properties->findItems("ID", Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!ids.empty());
        QCOMPARE(ids[0]->text(1), QString("32"));
        snapshot(window, "shader-setter-workspace");
    }
    void imagePixelGestures() {
        flora::ImageView image;
        image.resize(320, 240);
        QImage data(4, 3, QImage::Format_RGBA8888);
        data.fill(QColor(11, 22, 33, 44));
        image.setImage(data);
        image.show();
        QCoreApplication::processEvents();
        QSignalSpy picked(&image, &flora::ImageView::pixelSelected);
        const auto position = image.mapFromScene(QPointF(1.5, 1.5));
        QTest::mouseClick(image.viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked[0][0].toInt(), 1);
        QCOMPARE(picked[0][1].toInt(), 1);
        QCOMPARE(picked[0][2].value<QColor>(), QColor(11, 22, 33, 44));
        QTest::mousePress(image.viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QTest::mouseMove(image.viewport(), position + QPoint(35, 0));
        QTest::mouseRelease(image.viewport(), Qt::LeftButton, Qt::NoModifier, position + QPoint(35, 0));
        QCOMPARE(picked.size(), 1);
        QTest::mouseClick(image.viewport(), Qt::RightButton, Qt::NoModifier, position);
        QCOMPARE(picked.size(), 1);
    }
    void outputProjectSettings() {
        using namespace flora;
        QTemporaryDir dir;
        testing::msaaOutputCapture(false).save(dir.path() + "/msaa.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/msaa.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto target = window.findChild<QComboBox *>("outputTarget");
        auto channel = window.findChild<QComboBox *>("outputChannel");
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        auto sample = window.findChild<QSpinBox *>("outputSample");
        auto layer = window.findChild<QSpinBox *>("outputLayer");
        auto high = window.findChild<QLineEdit *>("outputHigh");
        auto image = window.findChild<ImageView *>("frameOutput");
        done.clear();
        target->setCurrentIndex(target->findData("rt0"));
        layer->setValue(1);
        sample->setValue(3);
        adapter->setCurrentIndex(1);
        high->setText("2.00");
        channel->setCurrentText("R");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image().pixelColor(0, 0), QColor(18, 18, 18, 255));
        auto api = window.findChild<QTableView *>("apiLog");
        done.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row)
            if (api->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 110)
                api->setCurrentIndex(api->model()->index(row, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        const auto path = dir.path() + "/output.json";
        projectFile(window, "saveExperiment", path);
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto document = nlohmann::json::parse(saved.readAll().toStdString());
        saved.close();
        QCOMPARE(document["ui"]["driver"], nlohmann::json("warp"));
        QCOMPARE(document["ui"]["frame_display"]["sample"], nlohmann::json("3"));
        QCOMPARE(document["ui"]["frame_display"]["high"], nlohmann::json("2.00"));
        QVERIFY(document["history"].empty());
        QCOMPARE(document["ui"]["event"], nlohmann::json(110));
        QCOMPARE(document["ui"]["flora_output_boundary"], nlohmann::json(2));
        done.clear();
        target->setCurrentIndex(target->findData("rt7"));
        sample->setValue(-1);
        adapter->setCurrentIndex(0);
        high->setText("1");
        channel->setCurrentText("RGBA");
        for (int row = 0; row < api->model()->rowCount(); ++row)
            if (api->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 100)
                api->setCurrentIndex(api->model()->index(row, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(image->image().isNull());
        done.clear();
        projectFile(window, "openExperiment", path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(target->currentData().toString(), QString("rt0"));
        QCOMPARE(channel->currentText(), QString("R"));
        QCOMPARE(high->text(), QString("2.00"));
        QCOMPARE(sample->value(), 3);
        QCOMPARE(layer->value(), 1);
        QCOMPARE(adapter->currentIndex(), 1);
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), 110ull);
        QCOMPARE(window.findChild<QComboBox *>("outputBoundary")->currentIndex(), 2);
        QCOMPARE(image->image().pixelColor(0, 0), QColor(18, 18, 18, 255));
        auto invalid = document;
        invalid["ui"]["driver"] = "invalid";
        QFile broken(dir.path() + "/invalid.json");
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write(QByteArray::fromStdString(invalid.dump()));
        broken.close();
        projectFile(window, "openExperiment", broken.fileName());
        QCOMPARE(adapter->currentIndex(), 1);
        QCOMPARE(sample->value(), 3);
        QCOMPARE(image->image().pixelColor(0, 0), QColor(18, 18, 18, 255));
        snapshot(window, "output-project-restored");
    }
    void outputPixelNavigation() {
        using namespace flora;
        QTemporaryDir dir;
        testing::msaaOutputCapture(false).save(dir.path() + "/msaa.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/msaa.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = window.findChild<ImageView *>("frameOutput");
        done.clear();
        window.findChild<QComboBox *>("outputChannel")->setCurrentText("R");
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(2.5, 1.5)));
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 0ull);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(2.5, 1.5)));
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 110ull);
        auto props = window.findChild<QTreeWidget *>("properties");
        QCOMPARE(props->topLevelItem(0)->text(0), QString("Output Pixel"));
        QCOMPARE(props->topLevelItem(0)->child(0)->text(1), QString("20"));
        QCOMPARE(props->topLevelItem(0)->child(2)->text(1), QString("2, 1"));
        QCOMPARE(props->topLevelItem(0)->child(4)->text(1), QString("1"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        snapshot(window, "output-pixel-navigation");
        // Inspecting another resource does not invalidate this frame's own provenance.
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row)
            if (resources->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 10)
                resources->setCurrentIndex(resources->model()->index(row, 0));
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentIndex(0);
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(1.5, 2.5)));
        QCOMPARE(props->topLevelItem(0)->text(0), QString("Output Pixel"));
        QCOMPARE(props->topLevelItem(0)->child(2)->text(1), QString("1, 2"));
        testing::Capture buffer;
        buffer.add(1, 5, 0x127, std::vector<uint8_t>(24));
        buffer.buffer(20, 21, 32, 0, {0, 0, 0, 0});
        buffer.buffer(24, 25, 0, 0, {0, 1, 2, 3});
        buffer.add(22, 5, 0x8d, testing::statePack(Id(0), Id(0), Id(20), 42u, 1u, 1u, 2u, 0u));
        buffer.add(100, 7, 0x34ff, testing::statePack(Id(0), Id(1), 1u, uint8_t(1), Id(22), Id(0)));
        buffer.add(101, 7, 0x3e, testing::statePack(Id(0), Id(1), Id(20), Id(24)));
        buffer.save(dir.path() + "/buffer.gpa_frame");
        done.clear();
        window.openCapture(dir.path() + "/buffer.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto target = window.findChild<QComboBox *>("outputTarget");
        target->setCurrentIndex(target->findData("rt0"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(1.5, .5)));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(window.findChild<QLineEdit *>("bufferOffset")->text(), QString("8"));
        QCOMPARE(window.findChild<QLineEdit *>("bufferLength")->text(), QString("4"));
        auto table = window.findChild<QTableView *>("bufferTable");
        QCOMPARE(table->model()->index(0, 1).data().toString(), QString("02 00 00 00"));
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 101ull);
        snapshot(window, "output-buffer-pixel");
    }
    void outputSetterDialogValidation() {
        using namespace flora;
        using Json = nlohmann::json;
        QTemporaryDir dir;
        auto capture = testing::stateCapture();
        capture.save(dir.path() + "/outputs.gpa_frame");
        Frame frame((dir.path() + "/outputs.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("outputSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto count = dialog->findChild<QSpinBox *>("uav_count");
            count->setValue(2);
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("setterError")->isVisible());
            count->setValue(1);
            auto initialCounts = dialog->findChild<QCheckBox *>("initial_counts_provided");
            initialCounts->setChecked(true);
            auto table = dialog->findChild<QTableWidget *>("uavBindings");
            auto value = qobject_cast<QLineEdit *>(table->cellWidget(0, 2));
            value->setText("4294967296");
            apply->click();
            QCOMPARE(commits, 0);
            value->setText("0xffffffff");
            snapshot(*dialog, "output-uav-editor");
            apply->click();
        });
        QVERIFY(
            editOutputSetterDialog(&parent, frame, 140, capturedOutputSetter(frame, 140), [&](auto &values) {
                ++commits;
                QCOMPARE(values, Json({{"start_slot", 63},
                                       {"uav_count", 1},
                                       {"uavs", {22}},
                                       {"initial_counts", {UINT32_MAX}}}));
            }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("outputSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QCheckBox *>("rtv_keep")->setChecked(true);
            snapshot(*dialog, "output-keep-editor");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(
            editOutputSetterDialog(&parent, frame, 138, capturedOutputSetter(frame, 138), [&](auto &values) {
                ++commits;
                QCOMPARE(values, Json({{"rtv_count", UINT32_MAX},
                                       {"rtvs", nullptr},
                                       {"dsv", 0},
                                       {"start_slot", 0},
                                       {"uav_count", UINT32_MAX},
                                       {"uavs", nullptr},
                                       {"initial_counts", nullptr}}));
            }));
        QCOMPARE(commits, 2);
        // Null and provided empty arrays round-trip independently.
        for (bool present : {false, true}) {
            Json initial{{"rtv_count", 0}, {"rtvs", present ? Json::array() : Json(nullptr)}, {"dsv", 0}};
            QTimer::singleShot(0, &parent, [&] {
                auto dialog = parent.findChild<QDialog *>("outputSetterDialog");
                QVERIFY(dialog);
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            QVERIFY(!editOutputSetterDialog(&parent, frame, 114, initial, [&](auto &) { ++commits; }));
        }
        QCOMPARE(commits, 2);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("outputSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QSpinBox *>("rtv_count")->setValue(0);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->findChild<QLabel *>("setterError")->text().contains("changed"));
            dialog->reject();
        });
        QVERIFY(!editOutputSetterDialog(&parent, frame, 111, capturedOutputSetter(frame, 111),
                                        [&](auto &) { throw std::runtime_error("Experiment changed"); }));
        QVERIFY(isEditableSetter(0x34ff));
        QVERIFY(isEditableSetter(0x3500));
        QVERIFY(isEditableSetter(0x25e));
        QVERIFY(isEditableSetter(0x3522));
    }
    void outputSetterWorkerHistory() {
        using namespace flora;
        QTemporaryDir dir;
        auto capture = testing::srvBindingCapture();
        capture.add(905, 7, 0x34ff, testing::statePack(Id(0), Id(1), 1u, uint8_t(1), Id(21), Id(0)));
        capture.save(dir.path() + "/output.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/output.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 905) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("outputSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto count = dialog->findChild<QSpinBox *>("rtv_count");
            QCOMPARE(count->value(), 1);
            count->setValue(0);
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QAction *undo = nullptr, *redo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
            if (action->shortcut() == QKeySequence::Redo)
                redo = action;
        }
        QVERIFY(undo && redo);
        for (auto action : {undo, redo}) {
            done.clear();
            action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY(done.takeLast()[0].toBool());
            bool inspected = false;
            QTimer::singleShot(0, &window, [&] {
                auto dialog = window.findChild<QDialog *>("outputSetterDialog");
                QVERIFY(dialog);
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                QCOMPARE(dialog->findChild<QSpinBox *>("rtv_count")->value(), action == undo ? 1 : 0);
                inspected = true;
                dialog->reject();
            });
            edit->trigger();
            QVERIFY(inspected);
        }
        snapshot(window, "output-setter-workspace");
    }
    void streamOutputSetterDialogValidation() {
        using namespace flora;
        using Json = nlohmann::json;
        QTemporaryDir dir;
        auto capture = testing::streamCapture();
        capture.add(120, 7, 0x3503, testing::statePack(Id(0), Id(1), 1u, uint8_t(1), Id(70), uint8_t(0)));
        capture.save(dir.path() + "/so.gpa_frame");
        Frame frame((dir.path() + "/so.gpa_frame").toStdWString());
        QWidget parent;
        Json changed;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("outputSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto table = dialog->findChild<QTableWidget *>("soBindings");
            dialog->findChild<QCheckBox *>("offsets_provided")->setChecked(true);
            auto offset = qobject_cast<QLineEdit *>(table->cellWidget(0, 2));
            offset->setText("3");
            apply->click();
            QVERIFY(changed.is_null());
            QVERIFY(dialog->findChild<QLabel *>("setterError")->isVisible());
            offset->setText("64");
            snapshot(*dialog, "stream-output-editor");
            apply->click();
        });
        QVERIFY(editOutputSetterDialog(&parent, frame, 120, capturedOutputSetter(frame, 120),
                                       [&](auto &values) { changed = values; }));
        QCOMPARE(changed, Json({{"count", 1}, {"buffers", {70}}, {"offsets", {64}}}));
        QVERIFY(isEditableSetter(0x3503));
    }
    void viewDialogValidation() {
        using namespace flora;
        using Json = nlohmann::json;
        QTemporaryDir dir;
        testing::stateCapture().save(dir.path() + "/views.gpa_frame");
        Frame frame((dir.path() + "/views.gpa_frame").toStdWString());
        QWidget parent;
        applyAppearance(*qApp);
        int commits = 0;
        for (Id id : {Id(6), Id(8), Id(9), Id(22)}) {
            QTimer::singleShot(0, &parent, [&] {
                auto dialog = parent.findChild<QDialog *>("viewDialog");
                QVERIFY(dialog);
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                auto box = dialog->findChild<QComboBox *>("viewResource");
                QCOMPARE(box->currentData().toULongLong(), id);
                auto dimension = dialog->findChild<QComboBox *>("dimension");
                QCOMPARE(dimension->count(), id == 6 ? 11 : id == 8 ? 8 : 6);
                if (id == 9)
                    QCOMPARE(dimension->currentText(), QString("Texture2DArray"));
                dialog->findChild<QComboBox *>("format")->setEditText("42");
                snapshot(*dialog, QString("view-editor-%1").arg(id));
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            QVERIFY(editViewDialog(
                &parent, frame, id, [&](Id view) { return describeView(frame, view); },
                [&](Id view, const Json &patch) {
                    QCOMPARE(view, id);
                    QCOMPARE(patch, Json({{"format", 42}}));
                    ++commits;
                }));
        }
        QCOMPARE(commits, 4);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("viewDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto box = dialog->findChild<QComboBox *>("viewResource");
            box->setCurrentIndex(box->findData(QVariant::fromValue<qulonglong>(22)));
            apply->click();
            QVERIFY(dialog->findChild<QLabel *>("viewError")->text().contains("Load"));
            dialog->findChild<QPushButton *>("viewLoad")->click();
            auto flags = dialog->findChild<QLineEdit *>("flags");
            QVERIFY(flags);
            flags->setText("3");
            apply->click();
            QVERIFY(dialog->findChild<QLabel *>("viewError")->text().contains("flags"));
            flags->setText("4");
            apply->click();
            QVERIFY(dialog->findChild<QLabel *>("viewError")->text().contains("changed"));
            dialog->reject();
        });
        QVERIFY(!editViewDialog(
            &parent, frame, 6, [&](Id view) { return describeView(frame, view); },
            [&](Id, const Json &) { throw std::runtime_error("Experiment changed"); }));
    }
    void srvDialogValidation() {
        using namespace flora;
        using Json = nlohmann::json;
        QWidget parent;
        int commits = 0;
        Json initial{{"format", 41}, {"dimension", 4}, {"most_detailed_mip", 0}, {"mip_levels", 2}};
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("srvDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            dialog->findChild<QSpinBox *>("srvSlot")->setValue(127);
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("srvError")->text().contains("Load"));
            dialog->findChild<QPushButton *>("srvLoad")->click();
            auto mips = dialog->findChild<QLineEdit *>("mip_levels");
            mips->setText("4294967296");
            apply->click();
            QCOMPARE(commits, 0);
            mips->setText("0xffffffff");
            apply->click();
        });
        QVERIFY(editSrvDialog(
            &parent, [&](auto &, auto) { return initial; },
            [&](auto &stage, unsigned slot, auto &patch) {
                ++commits;
                QCOMPARE(stage, std::string("ps"));
                QCOMPARE(slot, 127u);
                QCOMPARE(patch, Json({{"mip_levels", UINT32_MAX}}));
            }));
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("srvDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto dimension = dialog->findChild<QComboBox *>("dimension");
            dimension->setCurrentIndex(dimension->findData(5u));
            QVERIFY(dialog->findChild<QLineEdit *>("first_array_slice"));
            dialog->findChild<QLineEdit *>("first_array_slice")->setText("2");
            dialog->findChild<QComboBox *>("format")->setEditText("0x29");
            snapshot(*dialog, "srv-array-editor");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(editSrvDialog(
            &parent, [&](auto &, auto) { return initial; },
            [&](auto &, auto, auto &patch) {
                ++commits;
                QCOMPARE(patch, Json({{"format", 41},
                                      {"dimension", 5},
                                      {"first_array_slice", 2},
                                      {"array_size", 1},
                                      {"most_detailed_mip", 0},
                                      {"mip_levels", 2}}));
            }));
        QCOMPARE(commits, 2);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("srvDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(!editSrvDialog(
            &parent, [&](auto &, auto) { return initial; }, [&](auto &, auto, auto &) { ++commits; }));
        QCOMPARE(commits, 2);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("srvDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("mip_levels")->setText("1");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->findChild<QLabel *>("srvError")->text().contains("changed"));
            dialog->reject();
        });
        QVERIFY(!editSrvDialog(
            &parent, [&](auto &, auto) { return initial; },
            [&](auto &, auto, auto &) { throw std::runtime_error("Experiment changed"); }));
    }
    void samplerDialogValidation() {
        using namespace flora;
        QTemporaryDir dir;
        testing::samplerCapture().save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        QWidget parent;
        auto initial = samplerDescriptor(frame, 0);
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("samplerDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            dialog->findChild<QSpinBox *>("samplerSlot")->setValue(15);
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("samplerError")->text().contains("Load"));
            dialog->findChild<QPushButton *>("samplerLoad")->click();
            dialog->findChild<QLineEdit *>("mip_lod_bias")->setText("16");
            apply->click();
            QCOMPARE(commits, 0);
            dialog->findChild<QLineEdit *>("mip_lod_bias")->setText("-2.5");
            apply->click();
        });
        QVERIFY(editSamplerDialog(
            &parent, [&](auto &, auto) { return initial; },
            [&](const auto &stage, unsigned slot, const auto &patch) {
                ++commits;
                QCOMPARE(stage, std::string("ps"));
                QCOMPARE(slot, 15u);
                QCOMPARE(patch, nlohmann::json({{"mip_lod_bias", -2.5}}));
            }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("samplerDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(!editSamplerDialog(
            &parent, [&](auto &, auto) { return initial; }, [&](auto &, auto, auto &) { ++commits; }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("samplerDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("mip_lod_bias")->setText("2");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->findChild<QLabel *>("samplerError")->text().contains("changed"));
            dialog->reject();
        });
        QVERIFY(!editSamplerDialog(
            &parent, [&](auto &, auto) { return initial; },
            [&](auto &, auto, auto &) { throw std::runtime_error("Experiment changed"); }));
    }
    void samplerSetterDialogValidation() {
        using namespace flora;
        QTemporaryDir dir;
        testing::samplerCapture().save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("samplerSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            dialog->findChild<QSpinBox *>("start_slot")->setValue(15);
            dialog->findChild<QSpinBox *>("samplerCount")->setValue(2);
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("setterError")->isVisible());
            dialog->findChild<QSpinBox *>("samplerCount")->setValue(1);
            auto table = dialog->findChild<QTableWidget *>("samplerBindings");
            auto box = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(741))));
            snapshot(*dialog, "sampler-setter-editor");
            apply->click();
        });
        QVERIFY(editResourceSetterDialog(
            &parent, frame, 900, {{"start_slot", 2}, {"samplers", {0}}}, [&](auto &v) {
                ++commits;
                QCOMPARE(v, nlohmann::json({{"start_slot", 15}, {"samplers", {741}}}));
            }));
        QCOMPARE(commits, 1);
    }
    void srvSetterDialogValidation() {
        using namespace flora;
        QTemporaryDir dir;
        testing::srvBindingCapture().save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        QWidget parent;
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("srvSetterDialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            dialog->findChild<QSpinBox *>("start_slot")->setValue(127);
            dialog->findChild<QSpinBox *>("srvCount")->setValue(2);
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("setterError")->isVisible());
            dialog->findChild<QSpinBox *>("srvCount")->setValue(1);
            auto table = dialog->findChild<QTableWidget *>("srvBindings");
            auto box = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(733))));
            snapshot(*dialog, "srv-setter-editor");
            apply->click();
        });
        QVERIFY(editResourceSetterDialog(
            &parent, frame, 910, {{"start_slot", 0}, {"views", {732}}}, [&](auto &v) {
                ++commits;
                QCOMPARE(v, nlohmann::json({{"start_slot", 127}, {"views", {733}}}));
            }));
        QCOMPARE(commits, 1);
    }
    void samplerSetterHistory() {
        using namespace flora;
        QTemporaryDir dir;
        auto capture = testing::samplerCapture();
        // Establish the synthetic frame's output target before selecting a setter.
        capture.add(840, 7, 0x37, testing::statePack(Id(990), Id(0), Id(1), 0u, 0u));
        capture.save(dir.path() + "/sampler.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/sampler.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 900) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("samplerSetterDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto table = dialog->findChild<QTableWidget *>("samplerBindings");
            auto box = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(741))));
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 96, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 0));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 96, 0, 255));
    }
    void srvSetterHistory() {
        using namespace flora;
        QTemporaryDir dir;
        auto capture = testing::srvBindingCapture();
        capture.save(dir.path() + "/srv-setter.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/srv-setter.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 910) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("srvSetterDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto table = dialog->findChild<QTableWidget *>("srvBindings");
            auto box = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
            box->setCurrentIndex(box->findData(QVariant::fromValue(qulonglong(733))));
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(191, 0, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(32, 0, 0, 255));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(191, 0, 0, 255));
    }
    void outputSelectionControls() {
        using namespace flora;
        QTemporaryDir dir;
        testing::srvCapture().save(dir.path() + "/frame.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/frame.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto target = window.findChild<QComboBox *>("outputTarget");
        auto output = window.findChild<ImageView *>("frameOutput");
        auto rawExport = window.findChild<QAction *>("exportOutputStorage");
        QVERIFY(target && output && rawExport);
        QVERIFY(!output->image().isNull());
        QVERIFY(rawExport->isEnabled());
        done.clear();
        target->setCurrentIndex(target->findData("rt7"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(output->image().isNull());
        QVERIFY(!rawExport->isEnabled());
        done.clear();
        target->setCurrentIndex(target->findData("rt0"));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!output->image().isNull());
        auto high = window.findChild<QLineEdit *>("outputHigh");
        high->setText("0.25");
        done.clear();
        window.findChild<QComboBox *>("outputChannel")->setCurrentText("R");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(128, 128, 128, 255));
        snapshot(window, "output-controls");
        testing::msaaOutputCapture(false).save(dir.path() + "/msaa.gpa_frame");
        done.clear();
        window.openCapture(dir.path() + "/msaa.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(window.findChild<QComboBox *>("outputChannel")->currentText(), QString("RGBA"));
        QCOMPARE(high->text(), QString("1"));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(30, 0, 0, 255));
        auto sample = window.findChild<QSpinBox *>("outputSample");
        QVERIFY(sample);
        done.clear();
        sample->setValue(3);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(36, 0, 0, 255));
        snapshot(window, "output-msaa-sample");
    }
    void viewHistory() {
        using namespace flora;
        QTemporaryDir dir;
        testing::srvCapture().save(dir.path() + "/srv.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/srv.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editView");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("viewDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto views = dialog->findChild<QComboBox *>("viewResource");
            views->setCurrentIndex(views->findData(QVariant::fromValue<qulonglong>(732)));
            dialog->findChild<QPushButton *>("viewLoad")->click();
            dialog->findChild<QLineEdit *>("most_detailed_mip")->setText("1");
            dialog->findChild<QLineEdit *>("mip_levels")->setText("1");
            snapshot(*dialog, "view-history-editor");
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(191, 0, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(32, 0, 0, 255));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(191, 0, 0, 255));
    }
    void srvHistory() {
        using namespace flora;
        QTemporaryDir dir;
        testing::srvCapture().save(dir.path() + "/srv.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/srv.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSrv");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("srvDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("most_detailed_mip")->setText("1");
            dialog->findChild<QLineEdit *>("mip_levels")->setText("1");
            snapshot(*dialog, "srv-editor");
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(191, 0, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(32, 0, 0, 255));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(191, 0, 0, 255));
    }
    void samplerHistory() {
        using namespace flora;
        QTemporaryDir dir;
        testing::samplerCapture().save(dir.path() + "/sampler.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/sampler.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSampler");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("samplerDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QSpinBox *>("samplerSlot")->setValue(2);
            dialog->findChild<QPushButton *>("samplerLoad")->click();
            for (const auto *axis : {"address_u", "address_v", "address_w"})
                dialog->findChild<QComboBox *>(axis)->setCurrentIndex(3);
            dialog->findChild<QLineEdit *>("border_color/0")->setText("0.25");
            dialog->findChild<QLineEdit *>("border_color/1")->setText("0");
            dialog->findChild<QLineEdit *>("border_color/2")->setText("0");
            snapshot(*dialog, "sampler-editor");
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(64, 0, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 0));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(64, 0, 0, 255));
    }
    void blendHistory() {
        using namespace flora;
        QTemporaryDir dir;
        testing::depthStencilCapture().save(dir.path() + "/blend.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/blend.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        // Event selection starts a debounced preview; idle alone does not await it.
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto edit = window.findChild<QAction *>("editBlend");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("blendDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("/sample_mask")->setText("0x00000000");
            snapshot(*dialog, "blend-general-editor");
            dialog->findChild<QTabWidget *>()->setCurrentIndex(1);
            snapshot(*dialog, "blend-target-editor");
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
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
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
    }
    void blendDialogValidation() {
        using namespace flora;
        QTemporaryDir dir;
        testing::depthStencilCapture().save(dir.path() + "/blend.gpa_frame");
        Frame frame((dir.path() + "/blend.gpa_frame").toStdWString());
        QWidget parent;
        auto initial = capturedBlend(frame, 1000);
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("blendDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto mask = dialog->findChild<QLineEdit *>("/sample_mask");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            mask->setText("4294967296");
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(dialog->findChild<QLabel *>("blendError")->isVisible());
            mask->setText("0xf5f5f5f5");
            dialog->findChild<QComboBox *>("/blend_state/targets/3/write_mask")->setCurrentIndex(5);
            apply->click();
        });
        QVERIFY(editBlendDialog(&parent, initial, [&](const auto &patch) {
            ++commits;
            QCOMPARE(patch, nlohmann::json({{"sample_mask", 0xf5f5f5f5u},
                                            {"blend_state", {{"targets", {{"3", {{"write_mask", 5}}}}}}}}));
        }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("blendDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(!editBlendDialog(&parent, initial, [&](const auto &) { ++commits; }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("blendDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("/sample_mask")->setText("0");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->findChild<QLabel *>("blendError")->text().contains("changed"));
            dialog->reject();
        });
        QVERIFY(!editBlendDialog(&parent, initial,
                                 [&](const auto &) { throw std::runtime_error("Experiment changed"); }));
    }
    void rasterizerHistory() {
        using namespace flora;
        QTemporaryDir dir;
        testing::depthStencilCapture().save(dir.path() + "/rasterizer.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/rasterizer.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editRasterizer");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("rasterizerDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QComboBox *>("rs_scissor_enable")->setCurrentIndex(1);
            dialog->findChild<QPushButton *>("add_scissors")->click();
            auto table = dialog->findChild<QTableWidget *>("scissors");
            table->item(0, 2)->setText("0");
            table->item(0, 3)->setText("0");
            snapshot(*dialog, "rasterizer-state-editor");
            dialog->findChild<QTabWidget *>()->setCurrentIndex(2);
            snapshot(*dialog, "rasterizer-scissors-editor");
            entered = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
    }
    void rasterizerDialogValidation() {
        using namespace flora;
        QTemporaryDir dir;
        testing::depthStencilCapture().save(dir.path() + "/dialog.gpa_frame");
        Frame frame((dir.path() + "/dialog.gpa_frame").toStdWString());
        QWidget parent;
        auto initial = capturedRasterizer(frame, 1000);
        int commits = 0;
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("rasterizerDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto table = dialog->findChild<QTableWidget *>("viewports");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            auto error = dialog->findChild<QLabel *>("rasterizerError");
            table->item(0, 2)->setText("-1");
            apply->click();
            QCOMPARE(commits, 0);
            QVERIFY(error->isVisible());
            table->item(0, 2)->setText("1");
            auto add = dialog->findChild<QPushButton *>("add_viewports");
            for (int i = 0; i < 15; ++i)
                add->click();
            QCOMPARE(table->rowCount(), 16);
            QVERIFY(!add->isEnabled());
            auto remove = dialog->findChild<QPushButton *>("remove_viewports");
            for (int i = 0; i < 16; ++i)
                remove->click();
            QCOMPARE(table->rowCount(), 0);
            QVERIFY(!remove->isEnabled());
            apply->click();
        });
        QVERIFY(editRasterizerDialog(&parent, initial, [&](const auto &patch) {
            ++commits;
            QCOMPARE(patch, nlohmann::json({{"viewports", nlohmann::json::array()}}));
        }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("rasterizerDialog");
            if (dialog)
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QVERIFY(!editRasterizerDialog(&parent, initial, [&](const auto &) { ++commits; }));
        QCOMPARE(commits, 1);
        QTimer::singleShot(0, &parent, [&] {
            auto dialog = parent.findChild<QDialog *>("rasterizerDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QComboBox *>("rs_fill_mode")->setCurrentIndex(0);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->findChild<QLabel *>("rasterizerError")->text().contains("changed"));
            dialog->reject();
        });
        QVERIFY(!editRasterizerDialog(&parent, initial,
                                      [&](const auto &) { throw std::runtime_error("Experiment changed"); }));
    }
    void depthStencilHistory() {
        using namespace flora;
        auto capture = testing::depthStencilCapture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/depth.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/depth.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editDepthStencil");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("depthStencilDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QComboBox *>("/depth_stencil/depth_func")->setCurrentIndex(0);
            snapshot(*dialog, "depth-stencil-editor");
            entered = true;
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok),
                              Qt::LeftButton);
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto output = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
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
        QCOMPARE(output->image().pixelColor(0, 0), QColor(255, 0, 0, 255));
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(0, 0, 0, 255));
    }
    void predicateSetterHistory() {
        using namespace flora;
        auto capture = testing::predicateCapture(true, 0);
        QTemporaryDir dir;
        capture.save(dir.path() + "/setter.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/setter.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1200) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto edit = window.findChild<QAction *>("editSetter");
        QVERIFY(edit && edit->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("setterDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto value = dialog->findChild<QLineEdit *>("setterPredicateValue");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            value->setText("4294967296");
            QTest::mouseClick(apply, Qt::LeftButton);
            QVERIFY(dialog->findChild<QLabel *>("setterError")->isVisible());
            value->setText("0x1");
            snapshot(*dialog, "predicate-setter-dialog");
            entered = true;
            QTest::mouseClick(apply, Qt::LeftButton);
        });
        done.clear();
        edit->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto view = window.findChild<PredicateView *>("predicatePane");
        auto frame = std::make_shared<Frame>((dir.path() + "/setter.gpa_frame").toStdWString());
        auto checkValue = [&](const QString &expected) {
            view->setSelection(frame, 3000);
            view->findChild<QComboBox *>("predicateBoundary")->setCurrentIndex(1);
            QSignalSpy read(view, &PredicateView::inspectionFinished);
            view->findChild<QAction *>("readPredicate")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!read.empty(), 30000);
            QVERIFY(read.takeLast()[0].toBool());
            QCOMPARE(view->findChild<QTreeWidget *>("predicateFields")->topLevelItem(3)->text(1), expected);
        };
        checkValue("1");
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
        checkValue("0");
        done.clear();
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        checkValue("1");
    }
    void predicateInspector() {
        using namespace flora;
        auto capture = testing::predicateCapture(false, 0);
        QTemporaryDir dir;
        capture.save(dir.path() + "/predicate.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/predicate.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto view = window.findChild<PredicateView *>("predicatePane");
        QVERIFY(view);
        auto tabs = window.findChild<QTabWidget *>("pipelineTabs");
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(tabs);
        tabs->setCurrentWidget(view);
        auto read = view->findChild<QAction *>("readPredicate");
        auto boundary = view->findChild<QComboBox *>("predicateBoundary");
        auto fields = view->findChild<QTreeWidget *>("predicateFields");
        QSignalSpy inspected(view, &PredicateView::inspectionFinished);
        auto frame = std::make_shared<Frame>((dir.path() + "/predicate.gpa_frame").toStdWString());
        // Drive explicit boundaries through the real isolated worker.
        for (auto event : {1000u, 1100u, 1200u}) {
            view->setSelection(frame, event);
            boundary->setCurrentIndex(1);
            QTRY_VERIFY_WITH_TIMEOUT(read->isEnabled(), 30000);
            read->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!inspected.empty(), 30000);
            QVERIFY(inspected.takeLast()[0].toBool());
            QCOMPARE(fields->topLevelItem(0)->text(1), event == 1000 ? QString("active") : QString("ready"));
            QCOMPARE(fields->topLevelItem(1)->text(1), event == 1000 ? QString("—") : QString("false"));
            QCOMPARE(fields->topLevelItem(2)->text(1), event == 1200 ? QString("true") : QString("false"));
        }
        snapshot(window, "predicate-inspector");
        read->trigger();
        boundary->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(read->isEnabled(), 30000);
        QCOMPARE(fields->topLevelItemCount(), 0);
        QVERIFY(inspected.empty());
    }
    void predicateStaleResult() {
        using namespace flora;
        auto capture = testing::predicateCapture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/predicate.gpa_frame");
        auto frame = std::make_shared<Frame>((dir.path() + "/predicate.gpa_frame").toStdWString());
        PredicateView view;
        view.setSelection(frame, 1000);
        QSignalSpy requests(&view, &PredicateView::readRequested);
        auto read = view.findChild<QAction *>("readPredicate");
        read->trigger();
        QCOMPARE(requests.size(), 1);
        auto token = requests.takeFirst()[3].toULongLong();
        view.setSelection(frame, 1100);
        QVERIFY(!view.finish(token, {{"error", "obsolete"}}));
        QCOMPARE(view.findChild<QTreeWidget *>("predicateFields")->topLevelItemCount(), 0);
    }
    void streamOutputInspector() {
        using namespace flora;
        auto capture = testing::streamCapture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/so.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/so.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto resources = window.findChild<QTableView *>("resources");
        bool selected = false;
        for (int i = 0; i < resources->model()->rowCount(); ++i) {
            auto index = resources->model()->index(i, 0);
            if (index.data(Qt::UserRole).toULongLong() == 60) {
                resources->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        auto properties = window.findChild<QTreeWidget *>("properties");
        bool declaration = false;
        for (QTreeWidgetItemIterator it(properties); *it; ++it)
            if ((*it)->text(0) == "SO declaration")
                declaration = (*it)->text(1) == "62";
        QVERIFY(declaration);
        auto reflection = window.findChild<QTreeWidget *>("shaderReflection");
        QCOMPARE(reflection->topLevelItem(0)->text(0), QString("Stream Output"));
        QCOMPARE(reflection->topLevelItem(0)->child(0)->text(0), QString("SV_Position0"));
        snapshot(window, "stream-output-shader");
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(2, 0));
        QAction *inspect = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Inspect IA")
                inspect = action;
        QVERIFY(inspect);
        done.clear();
        inspect->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto geometry = window.findChild<QTableView *>("geometryTable");
        QCOMPARE(geometry->model()->rowCount(), 6);
        snapshot(window, "stream-output-geometry");
    }
    void classLinkageInspector() {
        using namespace flora;
        auto capture = testing::graphicsClassCapture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/classes.gpa_frame");
        auto artifacts = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            capture.save(artifacts + "/classes.gpa_frame");
        }
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/classes.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        auto output = window.findChild<ImageView *>("frameOutput");
        QVERIFY(output);
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        auto resources = window.findChild<QTableView *>("resources");
        auto select = [&](qulonglong id) {
            for (int i = 0; i < resources->model()->rowCount(); ++i) {
                auto index = resources->model()->index(i, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) {
                    resources->setCurrentIndex(index);
                    return true;
                }
            }
            return false;
        };
        auto properties = window.findChild<QTreeWidget *>("properties");
        auto property = [&](const QString &name) {
            for (QTreeWidgetItemIterator it(properties); *it; ++it)
                if ((*it)->text(0) == name)
                    return (*it)->text(1);
            return QString{};
        };
        QVERIFY(select(62));
        QCOMPARE(properties->topLevelItem(0)->text(0), QString("Class instance"));
        QCOMPARE(property("instance_name"), QString("first"));
        QCOMPARE(property("instance_index"), QString("1"));
        QCOMPARE(property("class_linkage_id"), QString("60"));
        QCOMPARE(property("creation_method"), QString("GetClassInstance"));
        snapshot(window, "class-instance");
        QVERIFY(select(60));
        QCOMPARE(properties->topLevelItem(0)->text(0), QString("Class linkage"));
        QVERIFY(select(32));
        QCOMPARE(property("Interface slots"), QString("1"));
        QCOMPARE(property("Class linkage"), QString("60"));
        auto source = window.findChild<QPlainTextEdit *>("shaderSource");
        QVERIFY(source);
        source->setPlainText("float4 main():SV_Target{return float4(0,1,0,1);}");
        QAction *compile = nullptr, *undo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->text() == "Compile && Apply")
                compile = action;
            if (action->shortcut() == QKeySequence::Undo)
                undo = action;
        }
        QVERIFY(compile && undo);
        done.clear();
        compile->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(done.size() >= 2, 30000);
        for (const auto &result : done)
            QVERIFY(result[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(property("Interface slots"), QString("0"));
        done.clear();
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(output->image().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(property("Interface slots"), QString("1"));
        snapshot(window, "class-shader-restored");
    }
    void replayStateStaleResults() {
        using namespace flora;
        using namespace flora::testing;
        auto capture = computeCapture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/state.gpa_frame");
        auto frame = std::make_shared<Frame>((dir.path() + "/state.gpa_frame").toStdWString());
        CommandStateView view(CommandStateView::Source::Replayed);
        view.setSelection(frame, 100);
        auto read = view.findChild<QAction *>("readReplayState");
        auto fields = view.findChild<QTreeWidget *>("replayStateFields");
        auto boundary = view.findChild<QComboBox *>("stateBoundary");
        QSignalSpy requested(&view, &CommandStateView::replayRequested);
        QSignalSpy finished(&view, &CommandStateView::inspectionFinished);
        view.setWorkerBusy(true);
        QVERIFY(!read->isEnabled());
        view.setWorkerBusy(false);
        read->trigger();
        QCOMPARE(requested.size(), 1);
        QVERIFY(!read->isEnabled());
        auto token = requested.takeFirst()[2].toULongLong();
        boundary->setCurrentIndex(1);
        QVERIFY(!view.finishReplay(token, {{"error", "Old error"}}));
        QVERIFY(read->isEnabled());
        QCOMPARE(fields->topLevelItemCount(), 0);
        QVERIFY(view.findChild<QLabel *>("stateSummary")->toolTip().isEmpty());
        read->trigger();
        token = requested.takeFirst()[2].toULongLong();
        QVERIFY(!view.finishReplay(token - 1, {{"error", "Wrong request"}}));
        QVERIFY(!read->isEnabled());
        QVERIFY(!view.finishReplay(token, {{"error", "Current error"}}));
        QVERIFY(read->isEnabled());
        QCOMPARE(view.findChild<QLabel *>("stateSummary")->toolTip(), QString("Current error"));
        read->trigger();
        token = requested.takeFirst()[2].toULongLong();
        view.invalidate();
        QVERIFY(!view.finishReplay(token, {{"bad", "stale result"}}));
        QVERIFY(read->isEnabled());
        read->trigger();
        token = requested.takeFirst()[2].toULongLong();
        QVERIFY(!view.finishReplay(token, {{"bad", "malformed result"}}));
        QCOMPARE(fields->topLevelItemCount(), 0);
        QVERIFY(!view.findChild<QAction *>("exportReplayState")->isEnabled());
        QCOMPARE(finished.size(), 4);
    }
    void replayStateInspector() {
        using namespace flora;
        auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External captures not configured");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int i = 0; i < api->model()->rowCount(); ++i) {
            auto index = api->model()->index(i, 0);
            if (index.data(Qt::UserRole).toULongLong() == 430) {
                api->setCurrentIndex(index);
                selected = true;
                break;
            }
        }
        QVERIFY(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto view = window.findChild<CommandStateView *>("replayStateView");
        QVERIFY(view);
        auto tabs = window.findChild<QTabWidget *>("pipelineTabs");
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(tabs);
        tabs->setCurrentWidget(view);
        auto read = view->findChild<QAction *>("readReplayState");
        auto exportAction = view->findChild<QAction *>("exportReplayState");
        auto fields = view->findChild<QTreeWidget *>("replayStateFields");
        auto boundary = view->findChild<QComboBox *>("stateBoundary");
        auto search = view->findChild<QLineEdit *>("stateSearch");
        auto summary = view->findChild<QLabel *>("stateSummary");
        auto field = [&](const QString &name) -> QTreeWidgetItem * {
            for (int i = 0; i < fields->topLevelItemCount(); ++i)
                if (fields->topLevelItem(i)->text(0) == name)
                    return fields->topLevelItem(i);
            return nullptr;
        };
        QSignalSpy stateDone(view, &CommandStateView::inspectionFinished);
        QVERIFY(read->isEnabled());
        read->trigger();
        QVERIFY(!read->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!stateDone.empty(), 30000);
        QVERIFY(stateDone.takeLast()[0].toBool());
        done.clear();
        QCOMPARE(fields->topLevelItemCount(), 1306);
        QCOMPARE(field("so.offsets.0")->text(1), QString("Unknown"));
        QVERIFY(field("so.offsets.0")->toolTip(2).contains("no getter"));
        QVERIFY(field("vs.shader")->data(1, Qt::UserRole).toULongLong() != 0);
        QVERIFY(field("vs.shader")->toolTip(1).contains("runtime_object"));
        QVERIFY(summary->text().contains("Replay"));
        search->setText("ps.");
        snapshot(window, "replay-state-gf2");
        QTemporaryDir exportDir;
        const auto exportPath = exportDir.path() + "/pipeline.json";
        const bool nativeDialogs = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        auto restore =
            qScopeGuard([&] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogs); });
        bool exported = false;
        QTimer::singleShot(0, &view[0], [&] {
            auto dialog = view->findChild<QFileDialog *>();
            if (!dialog)
                return;
            dialog->selectFile(exportPath);
            exported = true;
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        exportAction->trigger();
        QVERIFY(exported);
        QFile saved(exportPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        auto bytes = saved.readAll();
        auto result = nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
        QCOMPARE(result["fields"].size(), size_t(1306));
        QCOMPARE(result["source"], nlohmann::json("native_replay"));
        read->trigger();
        boundary->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!stateDone.empty(), 30000);
        QVERIFY(!stateDone.takeLast()[0].toBool());
        QCOMPARE(fields->topLevelItemCount(), 0);
        QVERIFY(!exportAction->isEnabled());
        done.clear();
        QAction *toggle = nullptr, *undo = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->text() == "Disable Event")
                toggle = action;
            if (action->text() == "Undo")
                undo = action;
        }
        QVERIFY(toggle && undo);
        toggle->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!stateDone.empty(), 30000);
        QVERIFY(stateDone.takeLast()[0].toBool());
        done.clear();
        QVERIFY(summary->text().contains("Experiment"));
        QVERIFY(summary->text().contains("Disabled"));
        snapshot(window, "replay-state-disabled");
        undo->trigger();
        QCOMPARE(fields->topLevelItemCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!stateDone.empty(), 30000);
        QVERIFY(stateDone.takeLast()[0].toBool());
        QVERIFY(!summary->text().contains("Disabled"));
        search->clear();
        auto shader = field("vs.shader");
        QSignalSpy resource(view, &CommandStateView::resourceRequested);
        QVERIFY(QMetaObject::invokeMethod(fields, "itemDoubleClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, shader), Q_ARG(int, 1)));
        QCOMPARE(resource.size(), 1);
    }
    void capturedStateInspector() {
        QTemporaryDir dir;
        auto capture = flora::testing::stateCapture();
        capture.save(dir.path() + "/state.gpa_frame");
        auto frame = std::make_shared<flora::Frame>((dir.path() + "/state.gpa_frame").toStdWString());
        flora::CommandStateView view;
        view.resize(1000, 640);
        view.show();
        auto read = view.findChild<QAction *>("readCapturedState");
        auto exportAction = view.findChild<QAction *>("exportCapturedState");
        auto fields = view.findChild<QTreeWidget *>("capturedStateFields");
        auto boundary = view.findChild<QComboBox *>("stateBoundary");
        auto knowledge = view.findChild<QComboBox *>("stateKnowledge");
        auto search = view.findChild<QLineEdit *>("stateSearch");
        QSignalSpy done(&view, &flora::CommandStateView::inspectionFinished);
        auto field = [&](const QString &name) -> QTreeWidgetItem * {
            for (int i = 0; i < fields->topLevelItemCount(); ++i)
                if (fields->topLevelItem(i)->text(0) == name)
                    return fields->topLevelItem(i);
            return nullptr;
        };
        view.setSelection(frame, 101);
        QVERIFY(read->isEnabled());
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(field("vs.shader"));
        QCOMPARE(field("vs.shader")->text(1), QString("0"));
        boundary->setCurrentIndex(1);
        QCOMPARE(fields->topLevelItemCount(), 0);
        QVERIFY(!exportAction->isEnabled());
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(field("vs.shader")->text(1), QString("50"));
        QCOMPARE(field("vs.shader")->text(3), QString("101"));
        QVERIFY(exportAction->isEnabled());
        view.setSelection(frame, 116);
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(done.takeLast()[0].toBool());
        search->setText("ps.srv.0");
        knowledge->setCurrentIndex(2);
        QVERIFY(!field("ps.srv.0")->isHidden());
        QCOMPARE(field("ps.srv.0")->text(1), QString("Unknown"));
        QCOMPARE(field("ps.srv.0")->text(2), QString("unknown"));
        QVERIFY(field("ps.srv.0")->toolTip(2).contains("Output binding"));
        snapshot(view, "state-unknown-hazard");
        knowledge->setCurrentIndex(1);
        QVERIFY(field("ps.srv.0")->isHidden());
        view.setSelection(frame, 115);
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(done.takeLast()[0].toBool());
        QSignalSpy resource(&view, &flora::CommandStateView::resourceRequested);
        QSignalSpy event(&view, &flora::CommandStateView::eventRequested);
        auto srv = field("ps.srv.0");
        QVERIFY(srv);
        QCOMPARE(srv->text(1), QString("6"));
        QVERIFY(QMetaObject::invokeMethod(fields, "itemDoubleClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, srv), Q_ARG(int, 1)));
        QCOMPARE(resource.takeLast()[0].toULongLong(), qulonglong(6));
        QVERIFY(QMetaObject::invokeMethod(fields, "itemDoubleClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, srv), Q_ARG(int, 3)));
        QCOMPARE(event.takeLast()[0].toULongLong(), qulonglong(115));
        snapshot(view, "state-known-source");
        read->trigger();
        view.setSelection(frame, 120);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(!done.takeLast()[0].toBool());
        QCOMPARE(fields->topLevelItemCount(), 0);
        QVERIFY(!exportAction->isEnabled());
        QVERIFY(read->isEnabled());
        view.setSelection(frame, 135);
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(fields->topLevelItemCount(), 0);
        QCOMPARE(view.findChild<QLabel *>("stateSummary")->text(), QString("State unavailable"));
    }
    void inferredContextNavigation() {
        using namespace flora;
        using namespace flora::testing;
        auto capture = graphicsCounterCapture(false);
        capture.entries.erase(std::remove_if(capture.entries.begin(), capture.entries.end(),
                                             [](const auto &e) { return e.id == 1; }),
                              capture.entries.end());
        capture.add(80, 5, 0x81, std::vector<uint8_t>(28));
        std::vector<uint8_t> texture(16);
        put(texture, 8, Id(80));
        append(texture, D3D11_TEXTURE2D_DESC{1,
                                             1,
                                             1,
                                             1,
                                             DXGI_FORMAT_R8G8B8A8_UNORM,
                                             {1, 0},
                                             D3D11_USAGE_STAGING,
                                             0,
                                             D3D11_CPU_ACCESS_READ,
                                             0});
        append(texture, Id(0));
        capture.add(81, 5, 0x85, texture);
        std::vector<uint8_t> map(48);
        put(map, 8, Id(1));
        put(map, 20, Id(81));
        put(map, 32, uint32_t(D3D11_MAP_READ));
        put(map, 40, Id(9000));
        capture.add(82, 7, 0x34ec, map);
        std::vector<uint8_t> unmap(28);
        put(unmap, 8, Id(1));
        put(unmap, 16, Id(81));
        capture.add(83, 7, 0x34ed, unmap);
        QTemporaryDir dir;
        capture.save(dir.path() + "/inferred.gpa_frame");
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/inferred.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(window.findChild<QComboBox *>("apiKinds")->currentIndex(), 0);
        window.findChild<QLineEdit *>("apiSearch")->setText("Draw");
        window.findChild<QLineEdit *>("apiResourceFilter")->setText("21");
        auto action = window.findChild<QAction *>("inspectCaptureStructure");
        QVERIFY(action);
        bool navigated = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("captureStructureDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto tree = dialog->findChild<QTreeWidget *>("contextInventory");
            QVERIFY(tree);
            QTreeWidgetItem *target = nullptr;
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                if ((*it)->text(0) == "event" && (*it)->data(1, Qt::UserRole).toULongLong() == 82) {
                    target = *it;
                    break;
                }
            QVERIFY(target);
            tree->expandAll();
            tree->scrollToItem(target);
            snapshot(*dialog, "context-inferred-evidence");
            navigated = QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
                                                  Q_ARG(QTreeWidgetItem *, target), Q_ARG(int, 1));
        });
        action->trigger();
        QVERIFY(navigated);
        QCOMPARE(window.findChild<QComboBox *>("apiKinds")->currentIndex(), 1);
        QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
        QVERIFY(window.findChild<QLineEdit *>("apiResourceFilter")->text().isEmpty());
        auto api = window.findChild<QTableView *>("apiLog");
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(82));
        auto properties = window.findChild<QTreeWidget *>("properties");
        QTreeWidgetItem *context = nullptr;
        for (int i = 0; i < properties->topLevelItemCount(); ++i)
            if (properties->topLevelItem(i)->data(0, Qt::UserRole + 1) == "contextDetails")
                context = properties->topLevelItem(i);
        QVERIFY(context);
        QCOMPARE(context->text(1), QString("Inferred"));
        int missing = 0;
        for (int i = 0; i < context->childCount(); ++i) {
            auto item = context->child(i);
            if (item->text(0) == "interface_version" || item->text(0) == "creation_flags" ||
                item->text(0) == "captured_pointer") {
                QCOMPARE(item->text(1), QString("null"));
                ++missing;
            }
        }
        QCOMPARE(missing, 3);
        context->setExpanded(true);
        properties->scrollToItem(context, QAbstractItemView::PositionAtTop);
        snapshot(window, "context-inferred-inspector");
    }
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
        QTreeWidgetItem *fields = nullptr, *references = nullptr, *context = nullptr;
        for (int i = 0; i < props->topLevelItemCount(); ++i) {
            auto item = props->topLevelItem(i);
            if (item->data(0, Qt::UserRole + 1) == "apiFields")
                fields = item;
            if (item->data(0, Qt::UserRole + 1) == "apiReferences")
                references = item;
            if (item->data(0, Qt::UserRole + 1) == "contextDetails")
                context = item;
        }
        QVERIFY(fields && references);
        QVERIFY(context);
        QCOMPARE(context->text(1), QString("Captured"));
        QVERIFY(!context->isExpanded());
        auto fieldNamed = [](QTreeWidgetItem *parent, const QString &name) -> QTreeWidgetItem * {
            for (int i = 0; i < parent->childCount(); ++i)
                if (parent->child(i)->text(0) == name)
                    return parent->child(i);
            return nullptr;
        };
        auto kind = fieldNamed(context, "context_type");
        QVERIFY(kind);
        QCOMPARE(kind->text(1), QString("immediate"));
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
        auto structure = window.findChild<QAction *>("inspectCaptureStructure");
        QVERIFY(structure);
        bool inspected = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("captureStructureDialog");
            if (!dialog)
                return;
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto tabs = dialog->findChild<QTabWidget *>();
            QVERIFY(tabs);
            QCOMPARE(tabs->count(), 2);
            auto inventory = dialog->findChild<QTreeWidget *>("contextInventory");
            auto lists = dialog->findChild<QTreeWidget *>("commandListInventory");
            QVERIFY(inventory && lists);
            auto contexts = fieldNamed(inventory->invisibleRootItem(), "contexts");
            QVERIFY(contexts);
            QCOMPARE(contexts->childCount(), 1);
            auto contextKind = fieldNamed(contexts->child(0), "context_type");
            QVERIFY(contextKind);
            QCOMPARE(contextKind->text(1), QString("immediate"));
            auto recovery = fieldNamed(inventory->invisibleRootItem(), "recovery");
            QVERIFY(recovery);
            QVERIFY(!fieldNamed(recovery, "assumption"));
            QVERIFY(!recovery->toolTip(0).isEmpty());
            QVERIFY(dialog->findChild<QPushButton *>("exportCaptureStructure"));
            contexts->child(0)->setExpanded(true);
            snapshot(*dialog, "context-inventory");
            tabs->setCurrentIndex(1);
            auto execution = fieldNamed(lists->invisibleRootItem(), "execution_supported");
            QVERIFY(execution);
            QCOMPARE(execution->text(1), QString("false"));
            QVERIFY(!fieldNamed(lists->invisibleRootItem(), "limits"));
            QVERIFY(!lists->toolTip().isEmpty());
            snapshot(*dialog, "command-list-inventory");
            inspected = true;
            dialog->reject();
        });
        structure->trigger();
        QVERIFY(inspected);
        auto stateView = window.findChild<flora::CommandStateView *>("capturedStateView");
        QVERIFY(stateView);
        QVERIFY(select(430));
        auto analysisTabs = window.findChild<QTabWidget *>("analysisTabs");
        auto pipelineTabs = window.findChild<QTabWidget *>("pipelineTabs");
        QVERIFY(analysisTabs && pipelineTabs);
        analysisTabs->setCurrentWidget(pipelineTabs);
        pipelineTabs->setCurrentWidget(stateView);
        QSignalSpy stateDone(stateView, &flora::CommandStateView::inspectionFinished);
        auto readState = stateView->findChild<QAction *>("readCapturedState");
        QVERIFY(readState);
        readState->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!stateDone.empty(), 30000);
        QVERIFY(stateDone.takeLast()[0].toBool());
        QVERIFY(stateView->findChild<QTreeWidget *>("capturedStateFields")->topLevelItemCount() > 1000);
        QVERIFY(stateView->findChild<QLabel *>("stateSummary")->text().contains("Event 430"));
        stateView->findChild<QLineEdit *>("stateSearch")->setText("ps.");
        snapshot(window, "state-gf2-pipeline");
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
    void planarTextureWrites() {
        using namespace flora;
        using namespace flora::testing;
        QTemporaryDir dir;
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        c.add(20, 5, 0x85,
              statePack(Id(0), Id(0), 10u, 6u, 1u, 1u, 103u, 1u, 0u, 3u, 0u, 0x30000u, 0u, Id(21)));
        auto initial = word(90);
        initial.insert(initial.end(), 60, 0x31);
        initial.insert(initial.end(), 30, 0x7b);
        c.add(21, 9, 1, initial);
        auto writes = word(90);
        writes.insert(writes.end(), 90, 0x65);
        c.add(22, 9, 1, writes);
        c.add(80, 7, 0x246, statePack(Id(0), Id(1), int32_t(0), Id(20), 0u, 2u, 0u, Id(22)));
        const auto path = dir.path() + "/planar.gpa_frame";
        c.save(path);
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        for (auto combo : window.findChildren<QComboBox *>())
            if (combo->findText("All API calls") >= 0)
                combo->setCurrentText("All API calls");
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(2);
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 20) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto image = window.findChild<ImageView *>("textureOutput");
        auto label = window.findChild<QLabel *>("textureLabel");
        QCOMPARE(image->image().pixelColor(0, 0).red(), 0x65);
        QVERIFY(label->toolTip().contains("Map writes Y only"));
        QVERIFY(label->toolTip().contains("WRITE/READ_WRITE retain UV"));
        auto plane = window.findChild<QComboBox *>("texturePlane");
        plane->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QCOMPARE(image->image().pixelColor(0, 0), QColor(0x7b, 0x7b, 0, 255));
        snapshot(window, "planar-retained-uv");
        plane->setCurrentIndex(1);
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image().pixelColor(0, 0).red(), 0x31);
        QVERIFY(!label->toolTip().contains("Map writes Y only"));
    }
    void textureInspectorControls() {
        using namespace flora;
        using namespace flora::testing;
        QTemporaryDir dir;
        msaaOutputCapture(false).save(dir.path() + "/msaa.gpa_frame");
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(dir.path() + "/msaa.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(1, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(2);
        const auto selectResource = [&](Id id) {
            auto view = window.findChild<QTableView *>("resources");
            for (int row = 0; row < view->model()->rowCount(); ++row) {
                auto index = view->model()->index(row, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) {
                    view->setCurrentIndex(index);
                    return true;
                }
            }
            return false;
        };
        QVERIFY(selectResource(20));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto image = window.findChild<ImageView *>("textureOutput");
        QCOMPARE(image->image().pixelColor(0, 0), QColor(10, 0, 0, 255));
        auto sample = window.findChild<QSpinBox *>("textureSample");
        QVERIFY(sample->isEnabled());
        sample->setValue(3);
        for (auto spin : window.findChildren<QSpinBox *>())
            if (spin->prefix() == "Layer ")
                spin->setValue(1);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QCOMPARE(image->image().pixelColor(0, 0), QColor(36, 0, 0, 255));
        window.findChild<QLineEdit *>("textureFormat")->setText("0x1c");
        window.findChild<QLineEdit *>("textureHigh")->setText("0.5");
        window.findChild<QComboBox *>("textureChannels")->setCurrentText("R");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QCOMPARE(image->image().pixelColor(0, 0), QColor(72, 72, 72, 255));
        auto exportAction = window.findChild<QAction *>("exportTexture");
        QVERIFY(exportAction->isEnabled());
        projectFile(window, "exportTexture", dir.path() + "/sample.bin");
        QFile raw(dir.path() + "/sample.bin");
        QVERIFY(raw.open(QIODevice::ReadOnly));
        auto bytes = raw.readAll();
        QCOMPARE(bytes.size(), qsizetype(140));
        QCOMPARE(bytes.left(4), QByteArray::fromHex("240000ff"));
        projectFile(window, "exportTexture", dir.path() + "/sample.dds");
        QFile dds(dir.path() + "/sample.dds");
        QVERIFY(dds.open(QIODevice::ReadOnly));
        QCOMPARE(dds.size(), qint64(428));
        QCOMPARE(dds.read(4), QByteArray("DDS "));
        projectFile(window, "exportTexture", dir.path() + "/sample.png");
        QImage png(dir.path() + "/sample.png");
        QCOMPARE(png.pixelColor(0, 0), QColor(72, 72, 72, 255));
        snapshot(window, "texture-msaa-inspection");
        bool inherited = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("textureEditDialog");
            if (!dialog)
                return;
            inherited = dialog->findChild<QSpinBox *>("textureEditSample")->value() == 3 &&
                        dialog->findChild<QLineEdit *>("textureEditFormat")->text() == "0x1c";
            dialog->reject();
        });
        window.findChild<QAction *>("importTextureOutput")->trigger();
        QVERIFY(inherited);
        Capture luma;
        luma.add(20, 5, 0x85, statePack(Id(0), Id(0), 2u, 2u, 1u, 1u, 104u, 1u, 0u, 0u, 8u, 0u, 0u, Id(21)));
        auto storage = word(12);
        for (uint8_t i = 0; i < 12; ++i)
            storage.push_back(i);
        luma.add(21, 9, 1, storage);
        luma.save(dir.path() + "/luma.gpa_frame");
        window.openCapture(dir.path() + "/luma.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(0);
        QVERIFY(selectResource(20));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QVERIFY(!sample->isEnabled());
        QVERIFY(exportAction->isEnabled());
        projectFile(window, "exportTexture", dir.path() + "/luma.bin");
        QFile y(dir.path() + "/luma.bin");
        QVERIFY(y.open(QIODevice::ReadOnly));
        QCOMPARE(y.readAll(), QByteArray::fromHex("0001020306070809"));
        snapshot(window, "texture-captured-luma");
        window.findChild<QComboBox *>("texturePlane")->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(!done.takeLast()[0].toBool());
        QVERIFY(!exportAction->isEnabled());
        QVERIFY(image->image().isNull());
    }
    void textureEditorHistory_data() {
        QTest::addColumn<bool>("output");
        QTest::newRow("input") << false;
        QTest::newRow("output") << true;
    }
    void textureEditorHistory() {
        QFETCH(bool, output);
        QTemporaryDir dir;
        auto capture = flora::testing::textureEditCapture(28, 1, output);
        const auto path = dir.path() + "/texture.gpa_frame";
        capture.save(path);
        const auto rawPath = dir.path() + "/patch.raw";
        QFile raw(rawPath);
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(QByteArray::fromHex("ff0000ffff0000ffff0000ffff0000ff")), qint64(16));
        raw.close();
        flora::MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy done(&window, &flora::MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto resources = window.findChild<QTableView *>("resources");
        for (int i = 0; i < resources->model()->rowCount(); ++i) {
            const auto index = resources->model()->index(i, 0);
            if (index.data(Qt::UserRole).toULongLong() == 20) {
                resources->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto boundary = window.findChild<QComboBox *>("textureBoundary");
        boundary->setCurrentIndex(1);
        for (auto spin : window.findChildren<QSpinBox *>()) {
            if (spin->prefix() == "Mip ")
                spin->setValue(1);
            if (spin->prefix() == "Layer ")
                spin->setValue(1);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        auto image = window.findChild<flora::ImageView *>("textureOutput");
        const auto original = image->image();
        auto action = window.findChild<QAction *>(output ? "importTextureOutput" : "importTextureInput");
        QVERIFY(action && action->isEnabled());
        bool entered = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("textureEditDialog");
            if (!dialog)
                return;
            auto file = dialog->findChild<QLineEdit *>("textureEditFile");
            auto apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            file->setText(dir.path() + "/missing.raw");
            QTest::mouseClick(apply, Qt::LeftButton);
            entered = dialog->isVisible() && dialog->findChild<QLabel *>("textureEditError")->isVisible();
            file->setText(rawPath);
            snapshot(*dialog, "texture-edit-dialog");
            QTest::mouseClick(apply, Qt::LeftButton);
        });
        action->trigger();
        QVERIFY(entered);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        const auto edited = image->image();
        QVERIFY(edited != original);
        QCOMPARE(edited.pixelColor(0, 0), QColor(255, 0, 0, 255));
        snapshot(window, output ? "texture-output-edited" : "texture-input-edited");
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
        done.clear();
        QCOMPARE(image->image(), original);
        redo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        done.clear();
        QCOMPARE(image->image(), edited);
        boundary->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image(), output ? edited : original);
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
