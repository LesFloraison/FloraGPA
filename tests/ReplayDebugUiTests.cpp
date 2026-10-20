#include "application/RenderDocLibrary.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include "app/ReplayDebugView.h"
#include "application/NativeDebugConfig.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTreeWidget>
#include <QTextBlock>
#include <QtTest>
#include <cmath>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing shader debug test report");
    const auto bytes = f.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
QAction *action(QWidget &view, const char *name) { return view.findChild<QAction *>(name); }
void selectFile(const QString &path) {
    QTimer::singleShot(50, qApp, [path] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                dialog->setDirectory(QFileInfo(path).absolutePath());
                auto filename = dialog->findChild<QLineEdit *>("fileNameEdit");
                QVERIFY(filename);
                filename->setText(QFileInfo(path).fileName());
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
    });
}
} // namespace
class ReplayDebugUiTests : public QObject {
    Q_OBJECT
    QString output_;
  private slots:
    void talosRegisterWatch() {
        const auto root = qEnvironmentVariable("FLORA_TALOS_FILTER_DEBUG");
        if (root.isEmpty()) QSKIP("Set the Talos filter debug evidence directory");
        for (const auto policy : {QString("native"), QString("diagnostic")}) {
            const auto report = read(root + "/" + policy + "-debug/result.json");
            ReplayDebugView view("ps"); view.resize(1450, 950); view.show();
            view.setContext("talos-register-" + policy, 14684, true); view.selectPixel(1438, 0, 0);
            QVERIFY(view.finish(view.requestId(), report));
            QSignalSpy errors(&view, &ReplayDebugView::error);
            auto step = view.findChild<QSpinBox *>("debugStepIndex"); step->setValue(21);
            auto registers = view.findChild<QTreeWidget *>("debugRegisters");
            const auto r2 = registers->findItems("r2", Qt::MatchExactly);
            QCOMPARE(r2.size(), 1);
            QVERIFY(QMetaObject::invokeMethod(registers, "itemDoubleClicked", Qt::DirectConnection,
                Q_ARG(QTreeWidgetItem *, r2.front()), Q_ARG(int, 1)));
            auto watches = view.findChild<QTreeWidget *>("debugWatchValues");
            QCOMPARE(watches->topLevelItem(0)->text(0), QString("registers.r2.float"));
            QVERIFY(watches->isVisible());
            auto expression = view.findChild<QLineEdit *>("debugWatchExpression");
            for (const auto text : {"registers.r0.float.y", "registers.r0.uint.y", "registers.cb0[2].float.x"}) {
                expression->setText(text); action(view, "debugAddWatch")->trigger();
            }
            QCOMPARE(watches->topLevelItemCount(), 4);
            QCOMPARE(watches->topLevelItem(1)->text(2), QString("0"));
            step->setValue(22);
            const auto expected = policy == "native" ? 1.0 : 0.5000076293945312;
            QVERIFY(std::abs(watches->topLevelItem(1)->text(2).toDouble() - expected) < 1e-7);
            QCOMPARE(watches->topLevelItem(2)->text(2), policy == "native" ? QString("1065353216") : QString("1056964736"));
            QVERIFY(std::abs(watches->topLevelItem(3)->text(2).toDouble() - 0.0005208333604969084) < 1e-9);
            // Register-table presentation changes must not change an existing Watch interpretation.
            view.findChild<QComboBox *>("debugInterpretation")->setCurrentText("hex");
            QCOMPARE(watches->topLevelItem(0)->text(1), QString("float"));
            step->setValue(21); QCOMPARE(watches->topLevelItem(1)->text(2), QString("0"));
            step->setValue(22);
            const auto exported = view.exportReport();
            QCOMPARE(exported.at("watch_snapshot").at("expressions")[1].at("values"), Json::array({expected}));
            if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/talos-register-" + policy + ".png"));
            QVERIFY(errors.isEmpty());
        }
    }
    void talosDiscardSample() {
        const auto root = qEnvironmentVariable("FLORA_TALOS_FILTER_DEBUG");
        if (root.isEmpty()) QSKIP("Set the Talos filter debug evidence directory");
        const auto report = read(root + "/native-debug/result.json");
        QCOMPARE(report.at("execution"), Json("14684"));
        QCOMPARE(report.at("step_count"), Json(46));
        QCOMPARE(report.at("replay_policy").at("optimisation"), Json("balanced"));
        QCOMPARE(report.at("replay_policy").at("dx12_discard_policy"), Json("native_api"));
        QCOMPARE(report.at("gpu_reference").at("value").at("floatValue").at(3), Json(1.0));
        ReplayDebugView view("ps"); view.resize(1450, 950); view.show();
        view.setContext("talos-filter-native", 14684, true); view.selectPixel(1438, 0, 0);
        QVERIFY(view.finish(view.requestId(), report));
        QSignalSpy errors(&view, &ReplayDebugView::error);
        auto instruction = view.findChild<QLineEdit *>("debugInstruction");
        instruction->setText("21"); action(view, "debugToggleInstructionBreakpoint")->trigger();
        action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(21));
        action(view, "debugStep")->trigger();
        QCOMPARE(view.model()->position(), int64_t(22));
        auto watch = view.findChild<QLineEdit *>("debugWatchExpression");
        watch->setText("TEXCOORD.x"); action(view, "debugAddWatch")->trigger();
        auto values = view.findChild<QTreeWidget *>("debugWatchValues");
        QCOMPARE(values->topLevelItemCount(), 1);
        QVERIFY(std::abs(values->topLevelItem(0)->text(2).toDouble() - 0.7492187023162842) < 0.0000001);
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(43);
        watch->setText("SV_Target.w"); action(view, "debugAddWatch")->trigger();
        QCOMPARE(values->topLevelItemCount(), 2);
        QVERIFY(std::abs(values->topLevelItem(1)->text(2).toDouble() - 1.0000001192092896) < 0.000001);
        QCOMPARE(view.exportReport().at("replay_policy"), report.at("replay_policy"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/talos-filter-native.png"));
        const auto diagnostic = read(root + "/diagnostic-debug/result.json");
        QVERIFY(view.finish(view.requestId(), diagnostic));
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(43);
        bool notice{};
        for (auto label : view.findChildren<QLabel *>())
            notice |= label->isVisible() && label->text().contains("Diagnostic discard patterns are enabled");
        QVERIFY(notice);
        QCOMPARE(view.exportReport().at("replay_policy"), diagnostic.at("replay_policy"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/talos-filter-diagnostic.png"));
        QVERIFY(errors.isEmpty());
    }
    void preparedComputeInputs() {
        const auto path = qEnvironmentVariable("FLORA_PREPARED_COMPUTE_REPORT");
        if (path.isEmpty()) QSKIP("Set the Hellblade2 36775 prepared compute report");
        const auto result = read(path);
        QCOMPARE(result.at("execution"), Json("36775"));
        QCOMPARE(result.at("step_count"), Json(413));
        QCOMPARE(result.at("compute_preparation").at("consumed"), Json("1"));
        QCOMPARE(result.at("compute_preparation").at("status"), Json("single_pre_dispatch_preparation"));
        QCOMPARE(result.at("indirect_inputs").at("groups"), Json::array({44,1,1}));
        QCOMPARE(result.at("indirect_inputs").at("argument_offset"), Json("24"));
        const auto &inputs = result.at("compute_buffer_inputs");
        QCOMPARE(inputs.at("before_debug"), inputs.at("after_trace_creation"));
        QCOMPARE(inputs.at("after_trace_creation"), inputs.at("after_steps"));
        QCOMPARE(inputs.at("after_steps"), inputs.at("after_dispatch_restoration"));
        QCOMPARE(result.at("gpu_reference").at("consistency"), Json("single_post_dispatch_observation"));
        QVERIFY(!result.at("gpu_reference").contains("after_debug_value"));
        QCOMPARE(result.at("texture_stores").at("rows").size(), size_t(6));
        QVERIFY(result.at("gpa_modules_loaded").empty());
        ReplayDebugView view("cs"); view.resize(1650, 1000); view.show();
        view.setContext("prepared-36775", 36775, true);
        QVERIFY(view.finish(view.requestId(), result));
        const auto reference = view.findChild<QLabel *>("debugGpuReference"); QVERIFY(reference);
        QVERIFY(reference->text().contains("after trace release"));
        QVERIFY(!reference->text().contains("Read before debugger replay"));
        QVERIFY(!reference->text().contains("unchanged"));
        const auto tree = view.findChild<QTreeWidget *>("debugTextureStores"); QVERIFY(tree);
        for (auto tabs : view.findChildren<QTabWidget *>())
            if (tabs->indexOf(tree) >= 0) tabs->setCurrentWidget(tree);
        QSignalSpy texel(&view, &ReplayDebugView::texturePixelRequested);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, tree->topLevelItem(4)), Q_ARG(int, 2)));
        QCOMPARE(texel.size(), 1); QCOMPARE(texel[0][1].toULongLong(), 36775ULL);
        QCOMPARE(texel[0][2].toULongLong(), 570673ULL);
        QCOMPARE(texel[0][4].toUInt(), 704U); QCOMPARE(texel[0][5].toUInt(), 128U);
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/prepared-compute.png"));
    }
    void textureStoreNavigation() {
        const auto path = qEnvironmentVariable("FLORA_TEXTURE_STORE_REPORT");
        if (path.isEmpty()) QSKIP("Set the Hellblade2 texture-store observer report");
        const auto result = read(path);
        const auto &rows = result.at("texture_stores").at("rows");
        QCOMPARE(rows.size(), size_t(3));
        QCOMPARE(rows[0].at("status"), Json("observed_store"));
        QCOMPARE(rows[1].at("status"), Json("observed_store"));
        QCOMPARE(rows[2].at("status"), Json("out_of_bounds"));
        ReplayDebugView view("cs"); view.resize(1650, 1100); view.show();
        view.setContext("hellblade-writes", 36779, true);
        QVERIFY(view.finish(view.requestId(), result));
        auto tree = view.findChild<QTreeWidget *>("debugTextureStores"); QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 3);
        auto item = tree->topLevelItem(1);
        for (auto tabs : view.findChildren<QTabWidget *>())
            if (tabs->indexOf(tree) >= 0) tabs->setCurrentWidget(tree);
        QSignalSpy selected(&view, &ReplayDebugView::resourceRequested);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 0)));
        const auto position = view.model()->position(); QVERIFY(position >= 0);
        QCOMPARE(result.at("steps").at(size_t(position)).at("nextInstruction"), rows[1].at("instruction"));
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 1)));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected[0][0].toULongLong(), std::stoull(rows[1].at("resource_creation").get<std::string>()));
        QSignalSpy texel(&view, &ReplayDebugView::texturePixelRequested), errors(&view, &ReplayDebugView::error);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 2)));
        QCOMPARE(texel.size(), 1); QCOMPARE(texel[0][0].toString(), QString("hellblade-writes"));
        QCOMPARE(texel[0][1].toULongLong(), 36779ULL);
        QCOMPARE(texel[0][2].toULongLong(), 570673ULL);
        QCOMPARE(texel[0][4].toUInt(), 416U); QCOMPARE(texel[0][5].toUInt(), 296U);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, tree->topLevelItem(2)), Q_ARG(int, 2)));
        QCOMPARE(texel.size(), 1); QCOMPARE(errors.size(), 1);
        view.setWorkerBusy(true);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 1)));
        QCOMPARE(selected.size(), 1); view.setWorkerBusy(false);
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/texture-stores.png"));
        const auto serial = view.requestId(); view.setContext("new-experiment", 36779, true);
        QVERIFY(!view.finish(serial, result)); QCOMPARE(tree->topLevelItemCount(), 0);
    }
    void dxilFastContraction() {
        const auto path = qEnvironmentVariable("FLORA_DXIL_CONTRACTION_REPORT");
        if (path.isEmpty()) QSKIP("Set the Hellblade2 experimental contraction report");
        const auto result = read(path);
        QCOMPARE(result.at("dxil_fast_contraction").at("operations"), Json("24"));
        QCOMPARE(result.at("dxil_fast_contraction").at("failures"), Json("0"));
        QCOMPARE(result.at("gpu_invocation_reference").at("trace_output_comparison").at("status"), Json("exact_output_match"));
        QVERIFY(result.at("partial").get<bool>());
        ReplayDebugView view("ps"); view.resize(1500, 1000); view.show(); view.setContext("contract", 37003, true);
        view.selectPixel(541, 451, 0); view.selectPrimitive(0);
        view.findChild<QCheckBox *>("debugPixelHistoryReference")->setChecked(true);
        auto option = view.findChild<QCheckBox *>("debugFastContraction");
        QVERIFY(!option->isChecked()); QVERIFY(!view.request().contains("dxil_fast_contraction"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), result));
        option->setChecked(true); QCOMPARE(view.request().at("dxil_fast_contraction"), Json(true));
        QVERIFY(view.finish(view.requestId(), result)); QVERIFY(view.model());
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(int(result.at("steps").size() - 1));
        QVERIFY(view.findChild<QLabel *>("debugBackendNotice")->text().contains("Original driver intermediates are not observed"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/hellblade-contraction.png"));
        const auto serial = view.requestId(); option->setChecked(false);
        QVERIFY(!view.model()); QVERIFY(!view.finish(serial, result));
        option->setChecked(true); view.setContext("new-session", 37003, true);
        QVERIFY(!option->isChecked()); view.setContext("dx11", 1, false); QVERIFY(option->isHidden());
    }
    void initTestCase() {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-ReplayDebugTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty()) {
            output_ += "/replay-debug";
            QVERIFY(QDir().mkpath(output_));
        }
    }
    void selectionsAndInvalidation() {
        ReplayDebugView ps("ps"), vs("vs"), cs("cs");
        QVERIFY(!action(ps, "replayDebugRead")->isEnabled());
        ps.setContext("one", 105);
        vs.setContext("one", 105);
        cs.setContext("one", 51);
        ps.selectPixel(48, 32, 2);
        QCOMPARE(ps.request(),
                 (Json{{"action", "debug-pixel"}, {"gpa_event", 105}, {"x", 48}, {"y", 32}, {"sample", 2}}));
        ps.selectPrimitive(1);
        QCOMPARE(ps.request().at("primitive"), Json(1));
        ps.findChild<QLineEdit *>("debugPrimitive")->setText("4294967296");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ps.request());
        ps.selectPixel(48, 32, 2);
        QVERIFY(!ps.request().contains("primitive"));
        ps.selectPrimitive(2);
        ps.setContext("one", 106);
        QVERIFY(!ps.request().contains("primitive"));
        vs.findChild<QLineEdit *>("debugVertex")->setText("2");
        QCOMPARE(vs.request().at("vertex"), Json(2));
        vs.findChild<QLineEdit *>("debugInstance")->setText("4294967295");
        QCOMPARE(vs.request().at("instance"), Json(UINT32_MAX));
        vs.findChild<QLineEdit *>("debugInstance")->setText("4294967296");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vs.request());
        cs.findChild<QLineEdit *>("debugGroup")->setText("4294967295 0 7");
        QCOMPARE(cs.request().at("group"), Json({UINT32_MAX, 0, 7}));
        for (const auto &bad : {"-1 0 0", "+1 0 0", "4294967296 0 0", "1 2", "x 0 0"}) {
            cs.findChild<QLineEdit *>("debugGroup")->setText(bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, cs.request());
        }
        const auto token = ps.requestId();
        ps.setContext("two", 105);
        QSignalSpy completed(&ps, &ReplayDebugView::inspectionFinished);
        QVERIFY(!ps.finish(token, {{"ok", false}, {"error", "stale"}}));
        QCOMPARE(completed.size(), 0);
        ps.setWorkerBusy(true);
        QVERIFY(!action(ps, "replayDebugRead")->isEnabled());
        QVERIFY(action(ps, "replayDebugCancel")->isEnabled());
        ps.setWorkerBusy(false);
        QVERIFY(action(ps, "replayDebugRead")->isEnabled());
    }
    void pixelOutputMismatchReport() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PIXEL_DEBUG_REPORT");
        if (path.isEmpty()) QSKIP("Set the saved DX12 debug report with output comparison");
        auto result = read(path);
        QCOMPARE(result.at("gpu_invocation_reference").at("trace_output_comparison").at("status"),
                 Json("output_mismatch"));
        ReplayDebugView view("ps");
        view.setContext("hellblade-main-ps", 37003, true);
        QVERIFY(view.finish(view.requestId(), result));
        const auto label = view.findChild<QLabel *>("debugGpuInvocationReference");
        QVERIFY(label); QVERIFY(!label->isHidden());
        QVERIFY(label->text().contains("Final trace output differs"));
        QVERIFY(label->text().contains("not validated"));
        auto step = view.findChild<QSpinBox *>("debugStepIndex");
        step->setValue(int(result.at("steps").size() - 1));
        QVERIFY(label->text().contains("Final trace output differs"));
        view.setContext("new-session", 1, true);
        QVERIFY(label->isHidden());
    }
    void indirectActionSelection() {
        const auto path = qEnvironmentVariable("FLORA_DX12_INDIRECT_SELECTION_REPORT");
        if (path.isEmpty()) QSKIP("Set the BF1 explicit indirect-action debug report");
        auto result = read(path);
        QCOMPARE(result.at("execution"), Json("35808"));
        QCOMPARE(result.at("indirect_selection").at("indirect_action"), Json(0));
        QCOMPARE(result.at("indirect_selection").at("indirect_action_count"), Json(1));
        ReplayDebugView view("cs"); view.resize(1380, 1000); view.show(); view.setContext("bf1-indirect", 35808, true);
        auto edit = view.findChild<QLineEdit *>("debugIndirectAction");
        QVERIFY(!edit->isHidden()); QVERIFY(!view.request().contains("indirect_action"));
        edit->setText("0"); QCOMPARE(view.request().at("indirect_action"), Json(0));
        QVERIFY(view.finish(view.requestId(), result));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/bf1-indirect-action.png"));
        const auto previous = view.requestId(); edit->setText("1");
        QVERIFY(!view.model()); QVERIFY(!view.finish(previous, result));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), result));
        auto multiple = result; multiple["indirect_selection"]["indirect_action_count"] = 2;
        multiple["indirect_selection"]["indirect_action"] = 1;
        multiple["indirect_selection"]["event_ids"] = {42908, 42909};
        QVERIFY(view.finish(view.requestId(), multiple));
        auto wrongEvent = multiple; wrongEvent["indirect_selection"]["eid"] = 42908;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), wrongEvent));
        edit->clear(); QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), multiple));
        edit->setText("-1"); QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        view.setContext("dx11", 42, false); QVERIFY(edit->isHidden()); QVERIFY(edit->text().isEmpty());
        QVERIFY(!view.request().contains("indirect_action"));
    }
    void vertexOutputReferences() {
        const auto path = qEnvironmentVariable("FLORA_DX12_VERTEX_OUTPUT_DEBUG_REPORT");
        if (path.isEmpty()) QSKIP("Set the saved DX12 VS output/input comparison report");
        auto result = read(path);
        const auto &outputs = result.at("gpu_vertex_reference").at("output_comparison");
        QCOMPARE(outputs.at("outputs").size(), size_t(7));
        QCOMPARE(result.at("vertex_input_snapshots").at("before_debug").at("attributes").size(), size_t(9));
        ReplayDebugView view("vs"); view.resize(1380, 1000); view.show();
        view.setContext("bf1-vs-output", 36560, true); view.selectVertex(3966, 0);
        QVERIFY(view.finish(view.requestId(), result));
        auto label = view.findChild<QLabel *>("debugGpuInvocationReference");
        QVERIFY(label->text().contains("VS outputs:")); QVERIFY(label->text().contains("Selected vertex input"));
        QCOMPARE(Json::parse(label->toolTip().toStdString()).at("vertex_input_snapshots"), result.at("vertex_input_snapshots"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/bf1-vs-outputs.png"));
        auto changed = result;
        auto &comparison = changed["gpu_vertex_reference"]["output_comparison"];
        comparison.update({{"status", "output_mismatch"}, {"matched", 6}, {"mismatched", 1}, {"unavailable", 0}});
        comparison["outputs"][3].update({{"status", "output_mismatch"}, {"mismatched_components", Json::array({0,1,2})}});
        changed["vertex_input_snapshots"]["status"] = "observed_bytes_changed";
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("6 match")); QVERIFY(label->text().contains("TEXCOORD2"));
        QVERIFY(label->text().contains("input bytes changed")); QVERIFY(label->text().contains("not validated"));
        comparison.update({{"status", "partial_comparison"}, {"matched", 6}, {"mismatched", 0}, {"unavailable", 1}});
        comparison["outputs"][3]["status"] = "unavailable";
        changed["vertex_input_snapshots"]["status"] = "unavailable";
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("1 unavailable")); QVERIFY(!label->text().contains("Output differences:"));
        const auto request = view.requestId(); view.setContext("new-session", 36560, true);
        QVERIFY(!view.finish(request, result)); QVERIFY(label->isHidden());
    }
    void vertexPositionReference() {
        const auto path = qEnvironmentVariable("FLORA_DX12_VERTEX_DEBUG_REPORT");
        if (path.isEmpty()) QSKIP("Set the DX12 VS report with GPU position comparison");
        auto result = read(path);
        QCOMPARE(result.at("execution"), Json("36560"));
        const auto &reference = result.at("gpu_vertex_reference");
        QCOMPARE(reference.at("post_vertex"), Json("1428"));
        QCOMPARE(reference.at("vertex_ordinal"), Json(3966));
        QCOMPARE(reference.at("trace_output_comparison").at("status"), Json("exact_output_match"));
        QCOMPARE(reference.at("value").at("uintValue")[3], Json(1085587053));
        ReplayDebugView view("vs"); view.resize(1380, 1000); view.show();
        view.setContext("bf1-vs-position", 36560, true); view.selectVertex(3966, 0);
        QVERIFY(view.finish(view.requestId(), result));
        auto label = view.findChild<QLabel *>("debugGpuInvocationReference");
        QVERIFY(!label->isHidden()); QVERIFY(label->text().contains("ordinal 3966"));
        QVERIFY(label->text().contains("actual vertex 1428"));
        QVERIFY(label->text().contains("Final position components match exactly"));
        QVERIFY(label->text().contains("input stability remain unverified"));
        QCOMPARE(Json::parse(label->toolTip().toStdString()).at("gpu_vertex_reference"), reference);
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(int(result.at("steps").size() - 1));
        QVERIFY(label->text().contains("match exactly"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/bf1-vs-position.png"));
        auto changed = result;
        changed["gpu_vertex_reference"]["trace_output_comparison"]["status"] = "output_mismatch";
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("differs from GPU output"));
        QVERIFY(!label->text().contains("match exactly"));
        changed["gpu_vertex_reference"] = {{"status", "unavailable"}, {"reason", "No post-VS index mapping"}};
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("GPU VS position unavailable: No post-VS index mapping"));
        changed.erase("gpu_vertex_reference");
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("ordinal 3966")); QVERIFY(!label->text().contains("GPU VS position"));
        const auto old = view.requestId(); view.setContext("other-session", 36560, true);
        QVERIFY(!view.finish(old, result)); QVERIFY(label->isHidden());
    }
    void computeInputStabilityNotice() {
        const auto path = qEnvironmentVariable("FLORA_COMPUTE_INPUT_REPORT");
        if (path.isEmpty()) QSKIP("Set a recorded compute input-stability report");
        const auto result = read(path);
        QCOMPARE(result.at("compute_buffer_inputs").at("status"), Json("observed_bytes_changed"));
        QVERIFY(result.at("partial").get<bool>());
        ReplayDebugView view("cs"); view.resize(1450, 1000); view.show();
        view.setContext("hellblade-input-stability", 36779, true);
        QVERIFY(view.finish(view.requestId(), result));
        bool found{};
        for (const auto label : view.findChildren<QLabel *>())
            if (label->text().contains("Compute read-only buffer inputs changed")) {
                QVERIFY(!label->isHidden()); found = true;
            }
        QVERIFY(found);
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/compute-input-stability.png"));
        const auto serial = view.requestId();
        view.setContext("new-experiment", 36779, true);
        QVERIFY(!view.finish(serial, result));
        for (const auto label : view.findChildren<QLabel *>())
            QVERIFY(label->isHidden() || !label->text().contains("Compute read-only buffer inputs changed"));
    }
    void retainedComputeInputs() {
        const auto path = qEnvironmentVariable("FLORA_RETAINED_COMPUTE_REPORT");
        if (path.isEmpty()) QSKIP("Set the recorded Hellblade2 retained compute input report");
        const auto result = read(path);
        QCOMPARE(result.at("compute_input_lifecycle").at("status"), Json("released_dispatch_restored"));
        QCOMPARE(result.at("compute_input_lifecycle").at("retained_traces"), Json("1"));
        QCOMPARE(result.at("compute_input_lifecycle").at("patch_version"), Json(2));
        const auto &inputs = result.at("compute_buffer_inputs");
        QCOMPARE(inputs.at("status"), Json("observed_bytes_unchanged"));
        QCOMPARE(inputs.at("after_trace_creation"), inputs.at("after_steps"));
        QCOMPARE(inputs.at("after_steps"), inputs.at("after_dispatch_restoration"));
        QVERIFY(inputs.at("before_debug") != inputs.at("after_trace_creation"));
        QVERIFY(!result.at("gpu_reference").contains("after_trace_creation_value"));
        QCOMPARE(result.at("gpu_reference").at("consistency"), Json("selected_texel_changed"));
        QVERIFY(result.at("partial").get<bool>());
        ReplayDebugView view("cs"); view.resize(1450, 1000); view.show();
        view.setContext("hellblade-retained", 36779, true);
        QVERIFY(view.finish(view.requestId(), result));
        auto notice = view.findChild<QLabel *>("debugBackendNotice");
        QVERIFY(notice && !notice->isHidden());
        QVERIFY(notice->text().contains("inputs retained during stepping"));
        QVERIFY(notice->text().contains("earlier GPU reference cannot validate", Qt::CaseInsensitive));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/retained-compute.png"));
        auto obsolete = result;
        obsolete["compute_input_lifecycle"]["patch_version"] = 1;
        QVERIFY(view.finish(view.requestId(), obsolete));
        QVERIFY(notice->text().contains("Obsolete DXIL input-retention report"));
        QVERIFY(!notice->text().contains("inputs retained during stepping"));
        const auto serial = view.requestId(); view.setContext("new-experiment", 36779, true);
        QVERIFY(!view.finish(serial, result)); QVERIFY(notice->isHidden());
    }
    void screenProbeRayInstructionDebug() {
        const auto path = qEnvironmentVariable("FLORA_PROBE_RAY_DEBUG_REPORT");
        if (path.isEmpty()) QSKIP("Set the recorded Hellblade2 screen-probe ray debug report");
        const auto result = read(path);
        QCOMPARE(result.at("execution"), Json("30752"));
        QCOMPARE(result.at("step_count"), Json(1252));
        QCOMPARE(result.at("compute_invocation").at("dispatch_thread"), Json::array({448, 6, 0}));
        QCOMPARE(result.at("compute_input_lifecycle").at("status"), Json("released_dispatch_restored"));
        const auto &stores = result.at("texture_stores");
        QCOMPARE(stores.at("validation"), Json("not_validated"));
        QCOMPARE(stores.at("rows").size(), size_t(1));
        const auto &store = stores.at("rows")[0];
        QCOMPARE(store.at("resource_creation"), Json("550915"));
        QCOMPARE(store.at("view_format"), Json(57)); // R16_UINT.
        QCOMPARE(store.at("value")[0], Json(4481));
        QCOMPARE(store.at("gpu_reference").at("value").at("uintValue")[0], Json(4481));
        QCOMPARE(result.at("gpu_reference").at("consistency"), Json("single_post_dispatch_observation"));
        ReplayDebugView view("cs"); view.resize(1450, 1000); view.show();
        view.setContext("hellblade-ray-instruction", 30752, true);
        view.selectTextureReference(2149337228608ULL, 550915, 448, 6, 0, 0, 0);
        view.findChild<QLineEdit *>("debugGroup")->setText("56 0 0");
        view.findChild<QLineEdit *>("debugThread")->setText("0 6 0");
        QVERIFY(view.finish(view.requestId(), result));
        QVERIFY(view.model()->files().empty());
        QSignalSpy errors(&view, &ReplayDebugView::error);
        auto input = view.findChild<QLineEdit *>("debugInstruction"); QVERIFY(input);
        input->setText("351"); action(view, "debugToggleInstructionBreakpoint")->trigger();
        action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(1248));
        view.findChild<QLineEdit *>("debugWatchExpression")->setText("_383");
        action(view, "debugAddWatch")->trigger();
        auto watches = view.findChild<QTreeWidget *>("debugWatchValues"); QVERIFY(watches);
        QCOMPARE(watches->topLevelItemCount(), 1);
        QCOMPARE(watches->topLevelItem(0)->text(2), QString("4481"));
        action(view, "debugStep")->trigger();
        QCOMPARE(view.model()->position(), int64_t(1249));
        action(view, "debugPrevious")->trigger();
        QCOMPARE(view.model()->position(), int64_t(1248));
        QCOMPARE(watches->topLevelItem(0)->text(2), QString("4481"));
        auto tree = view.findChild<QTreeWidget *>("debugTextureStores"); QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 1);
        QSignalSpy texel(&view, &ReplayDebugView::texturePixelRequested);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem *, tree->topLevelItem(0)), Q_ARG(int, 2)));
        QCOMPARE(texel.size(), 1);
        QCOMPARE(texel[0][1].toULongLong(), 30752ULL);
        QCOMPARE(texel[0][2].toULongLong(), 550915ULL);
        QCOMPARE(texel[0][4].toUInt(), 448U); QCOMPARE(texel[0][5].toUInt(), 6U);
        auto reference = view.findChild<QLabel *>("debugGpuReference"); QVERIFY(reference);
        QVERIFY(reference->text().contains("after trace release"));
        QCOMPARE(view.exportReport().at("texture_stores"), stores);
        for (auto tabs : view.findChildren<QTabWidget *>())
            if (tabs->indexOf(watches) >= 0) tabs->setCurrentWidget(watches);
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/screen-probe-ray.png"));
        const auto serial = view.requestId();
        view.setContext("changed-ray-experiment", 30752, true);
        QVERIFY(!view.finish(serial, result));
        QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(reference->isHidden()); QVERIFY(errors.isEmpty());
    }
    void indirectComputeInvocation() {
        const auto root = qEnvironmentVariable("FLORA_DX12_INDIRECT_DEBUG_ROOT");
        if (root.isEmpty()) QSKIP("Set the Hellblade2 indirect compute evidence directory");
        const auto result = read(root + "/hellblade-indirect-reference1/result.json");
        QCOMPARE(result.at("step_count"), Json(332));
        QCOMPARE(result.at("gpu_reference").at("consistency"), Json("selected_texel_unchanged"));
        QCOMPARE(result.at("compute_invocation").at("dispatch_thread"), Json::array({0, 0, 0}));
        ReplayDebugView view("cs");
        view.resize(1450, 1000); view.show();
        view.setContext("hellblade-indirect-cs", 36779, true);
        QVERIFY(view.finish(view.requestId(), result));
        auto invocation = view.findChild<QLabel *>("debugGpuInvocationReference");
        QVERIFY(!invocation->isHidden());
        QVERIFY(invocation->text().contains("1174"));
        QVERIFY(invocation->text().contains("shader group size"));
        QVERIFY(invocation->text().contains("Texture coordinates are computed by the shader"));
        QCOMPARE(Json::parse(invocation->toolTip().toStdString()), result.at("compute_invocation"));
        QVERIFY(view.model()->files().empty());
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(325);
        QCOMPARE(result.at("steps")[325].at("nextInstruction"), Json(612));
        auto watch = view.findChild<QLineEdit *>("debugWatchExpression");
        for (const auto &name : {"_643", "_644", "_542", "_543", "_544"}) {
            watch->setText(name); action(view, "debugAddWatch")->trigger();
        }
        auto watches = view.findChild<QTreeWidget *>("debugWatchValues");
        QCOMPARE(watches->topLevelItemCount(), 5);
        QCOMPARE(watches->topLevelItem(0)->text(2), QString("832"));
        QCOMPARE(watches->topLevelItem(1)->text(2), QString("592"));
        QCOMPARE(watches->topLevelItem(2)->text(2), QString("0.046875"));
        action(view, "debugStep")->trigger();
        QCOMPARE(view.model()->position(), int64_t(326));
        action(view, "debugPrevious")->trigger();
        QCOMPARE(view.model()->position(), int64_t(325));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/indirect-compute.png"));
        const auto serial = view.requestId();
        view.setContext("hellblade-indirect-changed-experiment", 36779, true);
        QVERIFY(invocation->isHidden());
        QVERIFY(!view.finish(serial, result));
        auto legacy = result;
        legacy.erase("compute_invocation");
        QVERIFY(view.finish(view.requestId(), legacy));
        QVERIFY(invocation->isHidden());
        const auto changed = read(root + "/hellblade-indirect-reference2/result.json");
        QVERIFY(changed.at("partial").get<bool>());
        QVERIFY(view.finish(view.requestId(), changed));
        auto reference = view.findChild<QLabel *>("debugGpuReference");
        QVERIFY(reference->text().contains("changed; reference cannot validate this trace"));
    }
    void dxilInstructionBreakpointPanel() {
        const auto root = qEnvironmentVariable("FLORA_DX12_INDIRECT_DEBUG_ROOT");
        if (root.isEmpty()) QSKIP("Set the Hellblade2 indirect compute evidence directory");
        const auto result = read(root + "/hellblade-indirect-reference1/result.json");
        ReplayDebugView view("cs"); view.resize(1450, 1000); view.show();
        view.setContext("hellblade-breakpoints", 36779, true);
        QVERIFY(view.finish(view.requestId(), result));
        QVERIFY(view.model()->files().empty());
        QSignalSpy errors(&view, &ReplayDebugView::error);
        auto input = view.findChild<QLineEdit *>("debugInstruction");
        auto rules = view.findChild<QTreeWidget *>("debugRules");
        auto assembly = view.findChild<QPlainTextEdit *>("debugAssembly");
        auto step = view.findChild<QSpinBox *>("debugStepIndex");
        for (const auto id : {612, 614}) {
            input->setText(QString::number(id)); action(view, "debugToggleInstructionBreakpoint")->trigger();
        }
        QCOMPARE(rules->topLevelItemCount(), 2);
        QCOMPARE(rules->topLevelItem(0)->text(0), QString("Instruction 612"));
        QCOMPARE(rules->topLevelItem(1)->text(0), QString("Instruction 614"));
        QCOMPARE(rules->topLevelItem(0)->text(3), QString("0"));
        const auto config = view.model()->configuration();
        QTemporaryDir temp;
        const auto path = temp.path() + "/instructions.json";
        selectFile(path); action(view, "debugSave")->trigger();
        QCOMPARE(readNativeDebugConfig(path.toStdWString()), config);
        view.findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(3);
        rules->setCurrentItem(rules->topLevelItem(0));
        QCOMPARE(input->text(), QString("612"));
        QCOMPARE(view.model()->position(), int64_t(0));
        const auto line = view.model()->instructionInfo(612).at("lineInfo").at("disassemblyLine").get<int>();
        QCOMPARE(assembly->textCursor().blockNumber(), line - 1);
        QCOMPARE(assembly->extraSelections().size(), 3);
        QVERIFY(!action(view, "debugApplyRule")->isEnabled());
        QVERIFY(!view.findChild<QLineEdit *>("debugCondition")->isEnabled());
        QVERIFY(action(view, "debugRemoveRule")->isEnabled());
        action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(325));
        QCOMPARE(rules->currentItem()->text(0), QString("Instruction 612"));
        QCOMPARE(rules->currentItem()->text(3), QString("1"));
        view.findChild<QLineEdit *>("debugWatchExpression")->setText("_643");
        action(view, "debugAddWatch")->trigger();
        QCOMPARE(view.findChild<QTreeWidget *>("debugWatchValues")->topLevelItem(0)->text(2), QString("832"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/dxil-instruction-breakpoints.png"));
        action(view, "debugRemoveRule")->trigger();
        QCOMPARE(view.model()->instructionBreakpoints(), Json::array({614}));
        QCOMPARE(rules->topLevelItemCount(), 1);
        QCOMPARE(assembly->extraSelections().size(), 2);
        step->setValue(0); action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->result().at("steps")[size_t(view.model()->position())].at("nextInstruction"), Json(614));
        rules->setCurrentItem(rules->topLevelItem(0));
        view.setWorkerBusy(true); QVERIFY(!action(view, "debugRemoveRule")->isEnabled());
        view.setWorkerBusy(false); QVERIFY(action(view, "debugRemoveRule")->isEnabled());
        selectFile(path); action(view, "debugImport")->trigger();
        QCOMPARE(rules->topLevelItemCount(), 2);
        QCOMPARE(view.model()->configuration(), config);
        QCOMPARE(rules->currentItem()->text(0), QString("Instruction 614"));
        step->setValue(0); action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(325));
        const auto serial = view.requestId(); view.setContext("new-experiment", 36779, true);
        QVERIFY(!view.finish(serial, result));
        QCOMPARE(rules->topLevelItemCount(), 0);
        QVERIFY(!action(view, "debugRemoveRule")->isEnabled());
        QVERIFY(errors.empty());
    }
    void mixedSourceAndInstructionBreakpoints() {
        // Small UI fixture for shared DX11 source rules, not GPU/source-debug evidence.
        Json info = Json::array(), steps = Json::array();
        for (int id = 0; id < 2; ++id)
            info.push_back({{"instruction", id}, {"lineInfo", {{"fileIndex", 0}, {"lineStart", id + 1},
                {"lineEnd", id + 1}, {"disassemblyLine", id + 1}}}});
        for (const auto id : {0, 1, 0, 1}) steps.push_back({{"nextInstruction", id}, {"changes", Json::array()}});
        const Json result{{"ok", true}, {"action", "debug-pixel"}, {"steps", steps},
            {"trace", {{"instInfo", info}}}, {"disassembly", "instruction zero\ninstruction one\n"},
            {"source_debug", {{"files", Json::array({{{"filename", "fixture.hlsl"}, {"contents", "first\nsecond\n"}}})}}}};
        ReplayDebugView view("ps"); view.setContext("source-fixture", 1);
        QVERIFY(view.finish(view.requestId(), result));
        QSignalSpy errors(&view, &ReplayDebugView::error);
        view.findChild<QSpinBox *>("debugSourceLine")->setValue(2);
        view.findChild<QLineEdit *>("debugCondition")->setText("true");
        action(view, "debugApplyRule")->trigger();
        view.findChild<QLineEdit *>("debugInstruction")->setText("0");
        action(view, "debugToggleInstructionBreakpoint")->trigger();
        auto rules = view.findChild<QTreeWidget *>("debugRules");
        QCOMPARE(rules->topLevelItemCount(), 2);
        QCOMPARE(rules->topLevelItem(0)->text(0), QString("fixture.hlsl:2"));
        QCOMPARE(rules->topLevelItem(0)->text(1), QString("true"));
        rules->setCurrentItem(rules->topLevelItem(1));
        QVERIFY(!action(view, "debugApplyRule")->isEnabled());
        rules->setCurrentItem(rules->topLevelItem(0));
        QVERIFY(action(view, "debugApplyRule")->isEnabled());
        QCOMPARE(view.findChild<QTabWidget *>("debugCodeTabs")->currentIndex(), 1);
        action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(1));
        QCOMPARE(rules->currentItem()->text(0), QString("fixture.hlsl:2"));
        action(view, "debugRemoveRule")->trigger();
        QVERIFY(view.model()->breakpoints().empty());
        QCOMPARE(view.model()->instructionBreakpoints(), Json::array({0}));
        action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(2));
        QCOMPARE(rules->topLevelItem(0)->text(3), QString("2"));
        rules->setCurrentItem(rules->topLevelItem(0)); action(view, "debugRemoveRule")->trigger();
        QVERIFY(view.model()->instructionBreakpoints().empty());
        QCOMPARE(rules->topLevelItemCount(), 0);
        QVERIFY(errors.empty());
    }
    void backendReports() {
        const auto root = qEnvironmentVariable("FLORA_REPLAY_DEBUG_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_REPLAY_DEBUG_REPORTS to the native shader debug corpus");
        int verified = 0, positions = 0;
        for (const auto &dir : QDir(root).entryList({"*-native"}, QDir::Dirs)) {
            const auto result = read(root + '/' + dir + "/result.json");
            if (!result.value("ok", false) || !result.contains("steps"))
                continue;
            const auto kind = result.at("action").get<std::string>();
            ReplayDebugView view(kind == "debug-pixel" ? "ps" : kind == "debug-vertex" ? "vs" : "cs");
            view.resize(1250, 660);
            view.show();
            view.setContext("fixture", 105);
            QSignalSpy errors(&view, &ReplayDebugView::error);
            QVERIFY(view.finish(view.requestId(), result));
            QVERIFY(view.findChild<QPlainTextEdit *>("replayDebugDetails")->isHidden());
            action(view, "replayDebugDetailsToggle")->trigger();
            QCOMPARE(Json::parse(
                         view.findChild<QPlainTextEdit *>("replayDebugDetails")->toPlainText().toStdString()),
                     result);
            action(view, "replayDebugDetailsToggle")->trigger();
            QCOMPARE(view.findChild<QPlainTextEdit *>("debugAssembly")->toPlainText().toStdString(),
                     result.at("disassembly").get<std::string>());
            auto step = view.findChild<QSpinBox *>("debugStepIndex");
            auto registers = view.findChild<QTreeWidget *>("debugRegisters");
            auto variables = view.findChild<QTreeWidget *>("debugSourceVariables");
            auto stack = view.findChild<QTreeWidget *>("debugCallstack");
            ReplayDebugModel expected(result);
            auto verify = [&](size_t position) {
                expected.move(position);
                QCOMPARE(view.model()->position(), int64_t(position));
                QCOMPARE(variables->topLevelItemCount(), int(expected.sourceValues().size()));
                QCOMPARE(stack->topLevelItemCount(), int(expected.stack(position).size()));
                const auto rows = expected.registerRows();
                QCOMPARE(registers->topLevelItemCount(), int(rows.size()));
                for (size_t i = 0; i < rows.size(); ++i)
                    for (int col = 0; col < 3; ++col)
                        QCOMPARE(registers->topLevelItem(int(i))->text(col).toStdString(),
                                 rows[i][col].get<std::string>());
                QCOMPARE(view.exportReport(), expected.exportReport());
                ++positions;
            };
            verify(0);
            action(view, "debugPrevious")->trigger();
            QCOMPARE(view.model()->position(), int64_t(0));
            for (size_t i = 1; i < expected.size(); ++i) {
                action(view, "debugStep")->trigger();
                verify(i);
            }
            action(view, "debugStep")->trigger();
            QCOMPARE(view.model()->position(), int64_t(expected.size() - 1));
            for (size_t i = expected.size() - 1; i > 0; --i) {
                action(view, "debugPrevious")->trigger();
                verify(i - 1);
            }
            for (const auto &mode : {"int", "uint", "hex"}) {
                view.findChild<QComboBox *>("debugInterpretation")->setCurrentText(mode);
                const auto rows = expected.registerRows(mode);
                for (size_t i = 0; i < rows.size(); ++i)
                    QCOMPARE(registers->topLevelItem(int(i))->text(1).toStdString(),
                             rows[i][1].get<std::string>());
            }
            step->setValue(int(expected.size() - 1));
            const auto position = view.model()->position();
            for (int i = 0; i < stack->topLevelItemCount(); ++i)
                stack->setCurrentItem(stack->topLevelItem(i));
            for (int i = 0; i < view.findChild<QComboBox *>("debugSourceFile")->count(); ++i)
                view.findChild<QComboBox *>("debugSourceFile")->setCurrentIndex(i);
            QCOMPARE(view.model()->position(), position);
            auto watch = view.findChild<QLineEdit *>("debugWatchExpression");
            watch->setText("2u + 3u");
            action(view, "debugAddWatch")->trigger();
            auto watches = view.findChild<QTreeWidget *>("debugWatchValues");
            QCOMPARE(watches->topLevelItemCount(), 1);
            QCOMPARE(watches->topLevelItem(0)->text(2), QString("5"));
            QTemporaryDir temp;
            const auto exported = temp.path() + "/trace.json";
            view.exportResult(exported);
            QCOMPARE(read(exported), view.exportReport());
            view.setWorkerBusy(true);
            QVERIFY(!action(view, "debugToggleBreakpoint")->isEnabled());
            QVERIFY(!action(view, "debugStep")->isEnabled());
            view.setWorkerBusy(false);
            QVERIFY2(errors.empty(), qPrintable(errors.empty() ? QString{} : errors[0][0].toString()));
            view.setContext("changed", 105);
            QVERIFY(!view.model());
            QCOMPARE(registers->topLevelItemCount(), 0);
            QCOMPARE(variables->topLevelItemCount(), 0);
            QVERIFY(!action(view, "replayDebugExport")->isEnabled());
            ++verified;
        }
        QVERIFY(verified >= 21);
        QVERIFY(positions >= 500);
    }
    void sourceRulesAndConfig() {
        const auto root = qEnvironmentVariable("FLORA_REPLAY_DEBUG_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_REPLAY_DEBUG_REPORTS to the native shader debug corpus");
        ReplayDebugView view("ps");
        view.resize(1250, 660);
        view.show();
        view.setContext("fixture", 105);
        QVERIFY(view.finish(view.requestId(), read(root + "/loop-ps-hardware-native/result.json")));
        auto model = view.model();
        auto step = view.findChild<QSpinBox *>("debugStepIndex");
        QSignalSpy navigationErrors(&view, &ReplayDebugView::error);
        view.findChild<QComboBox *>("debugNavigation")->setCurrentIndex(1);
        for (size_t i = 0; i < model->size(); ++i) {
            for (const auto name : {"debugPrevious", "debugStep", "debugOver", "debugOut"}) {
                step->setValue(int(i));
                navigationErrors.clear();
                std::optional<size_t> expected;
                try {
                    if (QString(name) == "debugPrevious")
                        expected = model->nextSource(i, -1);
                    else if (QString(name) == "debugStep")
                        expected = model->nextSource(i, 1);
                    else
                        expected = model->functionStep(i, QString(name) == "debugOut").first;
                } catch (const std::exception &) {
                }
                action(view, name)->trigger();
                QCOMPARE(model->position(), int64_t(expected.value_or(i)));
                QCOMPARE(navigationErrors.size(), expected ? 0 : 1);
            }
        }
        step->setValue(0);
        view.findChild<QLineEdit *>("debugInstruction")
            ->setText(QString::fromStdString(model->result().at("steps").at(1).at("nextInstruction").dump()));
        action(view, "debugRunTo")->trigger();
        QCOMPARE(model->position(), int64_t(1));
        for (size_t i = 0; i < model->size(); ++i) {
            step->setValue(int(i));
            if (!model->location(i).is_null())
                break;
        }
        const auto position = model->position();
        QVERIFY(!model->location(position).is_null());
        auto condition = view.findChild<QLineEdit *>("debugCondition");
        condition->setText("true");
        view.findChild<QComboBox *>("debugHitMode")->setCurrentIndex(3);
        view.findChild<QLineEdit *>("debugHitCount")->setText("2");
        action(view, "debugApplyRule")->trigger();
        QCOMPARE(model->breakpoints().size(), size_t(1));
        QCOMPARE(model->breakpoints()[0].at("condition"), Json("true"));
        view.findChild<QLineEdit *>("debugWatchExpression")->setText("1u");
        action(view, "debugAddWatch")->trigger();
        const auto config = model->configuration();
        QTemporaryDir temp;
        const auto path = temp.path() + "/调试配置.json";
        view.findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(3);
        QSignalSpy saveErrors(&view, &ReplayDebugView::error);
        QVERIFY(action(view, "debugSave")->isEnabled());
        selectFile(path);
        action(view, "debugSave")->trigger();
        QVERIFY2(saveErrors.empty(),
                 qPrintable(saveErrors.empty() ? QString{} : saveErrors[0][0].toString()));
        QVERIFY(QFile::exists(path));
        QCOMPARE(read(path), config);
        auto rules = view.findChild<QTreeWidget *>("debugRules");
        rules->setCurrentItem(rules->topLevelItem(0));
        action(view, "debugRemoveRule")->trigger();
        QVERIFY(model->breakpoints().empty());
        selectFile(path);
        action(view, "debugImport")->trigger();
        QCOMPARE(model->configuration(), config);
        QCOMPARE(model->position(), position);
        navigationErrors.clear();
        std::optional<size_t> next;
        try {
            next = model->seek(position);
        } catch (const std::exception &) {
        }
        action(view, "debugContinue")->trigger();
        QCOMPARE(model->position(), int64_t(next.value_or(position)));
        QCOMPARE(navigationErrors.size(), next ? 0 : 1);
        const auto selectedRule = model->breakpoints().at(0);
        QCOMPARE(rules->topLevelItem(0)->text(3).toULongLong(),
                 qulonglong(model->encounterCount(model->position(),
                                                  {selectedRule.at("file"), selectedRule.at("line")})));
        step->setValue(int(position));
        auto bad = config;
        bad["watches"] = {"1", "1"};
        const auto badPath = temp.path() + "/invalid.json";
        writeNativeDebugConfig(badPath.toStdWString(), bad);
        QSignalSpy errors(&view, &ReplayDebugView::error);
        selectFile(badPath);
        action(view, "debugImport")->trigger();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(model->configuration(), config);
        QCOMPARE(model->position(), position);
        if (!output_.isEmpty()) {
            step->setValue(int((position + 1) % int64_t(model->size())));
            step->setValue(int(position));
            view.findChild<QTabWidget *>("debugCodeTabs")->setCurrentIndex(1);
            view.findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(3);
            QVERIFY(view.grab().save(output_ + "/loop-rules.png"));
        }
    }
    void independentFrame_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void independentFrame() {
        QFETCH(bool, warp);
        const auto source = qEnvironmentVariable("FLORA_DEBUG_SOURCE_CAPTURE");
        if (source.isEmpty())
            QSKIP("Set FLORA_DEBUG_SOURCE_CAPTURE to the original shader_sources capture");
        QStringList dialogs;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, this, [&] {
            for (auto w : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
                    dialogs << box->text();
                    box->accept();
                }
        });
        dismiss.start(20);
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(source);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        if (adapter->currentIndex() != int(warp)) {
            adapter->setCurrentIndex(int(warp));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
        }
        auto api = window.findChild<QTableView *>("apiLog");
        auto select = [&](qulonglong event) {
            for (int i = 0; i < api->model()->rowCount(); ++i)
                if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == event) {
                    api->setCurrentIndex(api->model()->index(i, 0));
                    return;
                }
            QFAIL("Missing API event");
        };
        auto stages = window.findChild<QTabWidget *>("checkpointStageTabs");
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(stages);
        QCOMPARE(stages->count(), 6);
        Json capture;
        for (const auto &stage : {"vs", "ps", "cs"}) {
            select(QString(stage) == "cs" ? 51 : 105);
            QTest::qWait(800);
            QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
            tasks.clear();
            auto view = window.findChild<ReplayDebugView *>(QString("replayDebug-") + stage);
            QVERIFY(view);
            stages->setCurrentWidget(view);
            if (QString(stage) == "vs")
                view->findChild<QLineEdit *>("debugVertex")->setText("2");
            if (QString(stage) == "ps")
                view->selectPixel(48, 32, 0);
            view->setBackendPath(flora::defaultRenderDocLibrary());
            action(*view, "replayDebugRead")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
            tasks.clear();
            QVERIFY(view->model());
            const auto report = view->exportReport();
            if (capture.is_null()) {
                capture = report.at("capture");
                if (!output_.isEmpty()) {
                    const auto file = QString::fromStdString(capture.get<std::string>());
                    const auto bytes = read(QFileInfo(file).dir().filePath("report.json")).dump(2);
                    QSaveFile saved(output_ + (warp ? "/recapture-warp.json" : "/recapture-hardware.json"));
                    QVERIFY(saved.open(QIODevice::WriteOnly));
                    QCOMPARE(saved.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
                    QVERIFY(saved.commit());
                }
            } else
                QCOMPARE(report.at("capture"), capture);
            QCOMPARE(report.at("gpa_event"), Json(QString(stage) == "cs" ? 51 : 105));
            QVERIFY(!report.at("steps").empty());
            if (!output_.isEmpty()) {
                const auto name = QString(stage) + (warp ? "-warp" : "-hardware");
                view->exportResult(output_ + "/" + name + ".json");
                view->findChild<QTabWidget *>("debugCodeTabs")->setCurrentIndex(1);
                view->findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(1);
                QVERIFY(window.grab().save(output_ + "/" + name + ".png"));
            }
        }
        auto view = window.findChild<ReplayDebugView *>("replayDebug-cs");
        action(*view, "replayDebugRead")->trigger();
        action(*view, "replayDebugCancel")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(!view->model());
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->exportReport().at("capture"), capture);
        QVERIFY2(dialogs.empty(), qPrintable(dialogs.join('\n')));
        QAction *disable{}, *undo{};
        for (auto a : window.findChildren<QAction *>()) {
            if (a->text() == "Disable Event")
                disable = a;
            if (a->text() == "Undo")
                undo = a;
        }
        QVERIFY(disable && undo);
        disable->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(!view->model());
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(!view->model());
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->exportReport().at("capture"), capture);
        view->setBackendPath("C:/missing/renderdoc.dll");
        action(*view, "replayDebugRead")->trigger();
        QCOMPARE(tasks.size(), 1);
        QVERIFY(!tasks.takeLast()[0].toBool());
        QVERIFY(!view->model());
        view->setBackendPath(flora::defaultRenderDocLibrary());
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->exportReport().at("capture"), capture);
        action(*view, "replayDebugRead")->trigger();
        select(105);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(!view->model());
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto ps = window.findChild<ReplayDebugView *>("replayDebug-ps");
        auto image = window.findChild<ImageView *>("frameOutput");
        QVERIFY(QMetaObject::invokeMethod(image, "pixelSelected", Qt::DirectConnection, Q_ARG(int, 48),
                                          Q_ARG(int, 32), Q_ARG(QColor, QColor(0, 0, 255))));
        QCOMPARE(ps->findChild<QSpinBox *>("debugX")->value(), 48);
        QCOMPARE(ps->findChild<QSpinBox *>("debugY")->value(), 32);
        auto history = window.findChild<PixelHistoryView *>();
        history->findChild<QLineEdit *>("historyResource")->clear();
        history->findChild<QLineEdit *>("historyEvent")->setText("105");
        action(*history, "readPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        QTemporaryDir temp;
        const auto historyPath = temp.path() + "/history.json";
        history->exportResult(historyPath);
        QCOMPARE(read(historyPath).at("capture"), capture);
        if (!output_.isEmpty())
            history->exportResult(output_ + (warp ? "/history-warp.json" : "/history-hardware.json"));
    }
};
QTEST_MAIN(ReplayDebugUiTests)
#include "ReplayDebugUiTests.moc"
