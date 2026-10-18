#include "MsaaCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include "app/ReplayDebugView.h"
#include "app/ReplayMeshView.h"
#include "app/RdcCountersView.h"
#include "app/Dx12PipelineView.h"
#include "application/RenderDocLibrary.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <cmath>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing history test report");
    auto bytes = f.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
QAction *action(QWidget &view, const char *name) { return view.findChild<QAction *>(name); }
bool showHistoryTab(MainWindow &window, PixelHistoryView *view) {
    for (auto tabs : window.findChildren<QTabWidget *>()) {
        if (tabs->indexOf(view) < 0)
            continue;
        tabs->setCurrentWidget(view);
        return view->isVisible();
    }
    return false;
}
} // namespace
class HistoryUiTests : public QObject {
    Q_OBJECT
    QString output_;
  private slots:
    void dx12WorkgroupRegression() {
        const auto root = qEnvironmentVariable("FLORA_DX12_WORKGROUP_EVIDENCE_DIR");
        if (root.isEmpty())
            QSKIP("Set the workgroup reproduction evidence directory");
        const auto stock = read(root + "/bf1-workgroup-stock1/result.json");
        const auto fixed = read(root + "/bf1-workgroup-fixed1/result.json");
        const auto native = read(root + "/bf1-workgroup-native2/output_storage.json");
        QVERIFY(stock.at("ok").get<bool>());
        QVERIFY(fixed.at("ok").get<bool>());
        QVERIFY(stock.at("gpa_modules_loaded").empty());
        QVERIFY(fixed.at("gpa_modules_loaded").empty());
        QCOMPARE(stock.at("backend_dxbc_workgroup_patch"), Json(0));
        QCOMPARE(fixed.at("backend_dxbc_workgroup_patch"), Json(1));
        QVERIFY(stock.at("partial").get<bool>());
        QCOMPARE(stock.at("debug_scope").at("dxbc_workgroup").at("status"),
                 Json("known_incorrect_helper_group_ids"));
        QCOMPARE(fixed.at("debug_scope").at("dxbc_workgroup").at("status"), Json("group_ids_initialized"));
        const auto store = [](const Json &report) {
            Json values;
            for (const auto &step : report.at("steps"))
                if (step.at("nextInstruction") == 12)
                    for (const auto &change : step.at("changes"))
                        if (change.at("after").at("name") == "r0") {
                            const auto &all = change.at("after").at("value").at("f32v");
                            values = Json::array({all[0], all[1], all[2], all[3]});
                        }
            return values;
        };
        const auto expected = Json::array({1.0, 0.5, 0.25, 1.0});
        QCOMPARE(stock.at("gpu_reference").at("value").at("floatValue"), expected);
        QCOMPARE(fixed.at("gpu_reference").at("value").at("floatValue"), expected);
        QCOMPARE(store(stock), Json::array({0.0, 0.5, 0.25, 1.0}));
        QCOMPARE(store(fixed), expected);
        QFile storage(root + "/bf1-workgroup-native2/output_storage.bin");
        QVERIFY(storage.open(QIODevice::ReadOnly));
        const auto bytes = storage.readAll();
        QCOMPARE(bytes.size(), qsizetype(1920 * 1009 * 4));
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString(),
                 native.at("sha256").get<std::string>());
        const uint32_t packed = (15U << 6) | ((14U << 6) << 11) | ((13U << 5) << 22);
        size_t wrong = 0;
        for (qsizetype i = 0; i < bytes.size(); i += 4) {
            uint32_t value{};
            memcpy(&value, bytes.constData() + i, 4);
            wrong += value != packed;
        }
        QCOMPARE(wrong, size_t(0));
        ReplayDebugView view("cs");
        view.setContext("workgroup", 36666, true);
        QVERIFY(view.finish(view.requestId(), stock));
        auto notice = view.findChild<QLabel *>("debugBackendNotice");
        QVERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains("group zero"));
        QVERIFY(view.finish(view.requestId(), fixed));
        QVERIFY(notice->isHidden());
    }
    void dx12ComputeGpuReference() {
        const auto path = qEnvironmentVariable("FLORA_DX12_CS_REFERENCE_REPORT");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_CS_REFERENCE_REPORT to the BF1 temporal CS trace");
        const auto report = read(path);
        QVERIFY(report.at("ok").get<bool>());
        QCOMPARE(report.at("execution"), Json("36666"));
        QVERIFY(report.at("gpa_modules_loaded").empty());
        const auto &reference = report.at("gpu_reference");
        QCOMPARE(reference.at("status"), Json("read"));
        QCOMPARE(reference.at("resource_creation"), Json("299115"));
        QCOMPARE(reference.at("resource_object"), Json("10432933744"));
        QCOMPARE(reference.at("scope"), Json("post_event_selected_texel_before_debug_replays"));
        ReplayDebugView view("cs");
        view.resize(1500, 950);
        view.show();
        view.setContext("temporal:original", 36666, true);
        view.findChild<QLineEdit *>("debugGroup")->setText("209 60 0");
        view.findChild<QLineEdit *>("debugThread")->setText("2 5 0");
        QVERIFY(!view.request().contains("resource"));
        view.selectTextureReference(10432933744ULL, 299115, 1674, 485, 0, 0, 0);
        const auto request = view.request();
        QCOMPARE(request.at("resource"), reference.at("resource_object"));
        QCOMPARE(request.at("resource_creation"), reference.at("resource_creation"));
        QCOMPARE(request.at("group"), Json::array({209, 60, 0}));
        QCOMPARE(request.at("thread"), Json::array({2, 5, 0}));
        QCOMPARE(request.at("x"), Json(1674));
        QCOMPARE(request.at("y"), Json(485));
        QVERIFY(view.finish(view.requestId(), report));
        auto label = view.findChild<QLabel *>("debugGpuReference");
        QVERIFY(label);
        QVERIFY(!label->isHidden());
        QVERIFY(label->text().contains("GPU selected texel after dispatch"));
        QVERIFY(label->text().contains("does not prove"));
        const auto model = view.model();
        QVERIFY(model);
        size_t storeStep = 0;
        for (size_t i = 0; i < model->size(); ++i)
            if (report.at("steps")[i].at("nextInstruction") == 373) {
                storeStep = i;
                break;
            }
        QVERIFY(storeStep > 0);
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(int(storeStep));
        model->addWatch("r1.x");
        // DXBC temporary registers are typeless; the float register view is
        // explicit, while a source Watch must not invent their type.
        QCOMPARE(model->watchResults().at(0).at("status"), Json("unavailable"));
        bool storeValueVisible = false;
        for (const auto &row : model->registerRows("float"))
            if (row[0] == "r1")
                storeValueVisible = !row[1].get<std::string>().empty();
        QVERIFY(storeValueVisible);
        QCOMPARE(view.exportReport().at("gpu_reference"), reference);
        auto registers = view.findChild<QTreeWidget *>("debugRegisters");
        for (int i = 0; i < registers->topLevelItemCount(); ++i)
            if (registers->topLevelItem(i)->text(0) == "r1") {
                registers->setCurrentItem(registers->topLevelItem(i));
                registers->scrollToItem(registers->topLevelItem(i), QAbstractItemView::PositionAtTop);
            }
        QCoreApplication::processEvents();
        if (!output_.isEmpty())
            QVERIFY(view.grab().save(output_ + "/compute-gpu-reference.png"));
        auto warned = report;
        warned["debug_scope"]["warnings"] = {"Known incorrect helper group IDs in stock backend"};
        QVERIFY(view.finish(view.requestId(), warned));
        auto notice = view.findChild<QLabel *>("debugBackendNotice");
        QVERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains("incorrect helper group"));
        QVERIFY(view.finish(view.requestId(), report));
        QVERIFY(notice->isHidden());
        const auto old = view.requestId();
        action(view, "clearComputeReference")->trigger();
        QVERIFY(!view.request().contains("resource"));
        QVERIFY(label->isHidden());
        QVERIFY(!view.finish(old, report));
        view.selectTextureReference(9007199254740993ULL, 9007199254740995ULL, 1, 2, 3, 4, 0);
        QCOMPARE(view.request().at("resource"), Json("9007199254740993"));
        QCOMPARE(view.request().at("resource_creation"), Json("9007199254740995"));
        view.setContext("temporal:modified", 36666, true);
        QVERIFY(!view.request().contains("resource"));
        view.setContext("dx11", 1, false);
        QVERIFY(!action(view, "clearComputeReference")->isVisible());
    }
    void dx12DebugGpuReference() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PS_REFERENCE_REPORT");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_PS_REFERENCE_REPORT to the BF1 producer PS trace");
        const auto report = read(path);
        QVERIFY(report.at("ok").get<bool>());
        QCOMPARE(report.at("execution"), Json("36758"));
        QVERIFY(report.at("gpa_modules_loaded").empty());
        const auto &reference = report.at("gpu_reference");
        QCOMPARE(reference.at("status"), Json("read"));
        QCOMPARE(reference.at("scope"), Json("post_event_target_before_debug_replays"));
        QCOMPARE(reference.at("x"), Json(1674));
        QCOMPARE(reference.at("y"), Json(485));
        QCOMPARE(reference.at("mip"), Json(0));
        QCOMPARE(reference.at("layer"), Json(0));
        QCOMPARE(reference.at("format").at("type"), Json(13)); // R11G11B10
        QCOMPARE(reference.at("interpretation"), Json("float"));
        ReplayDebugView view("ps");
        view.resize(1500, 950);
        view.show();
        view.setContext("producer:original", 36758, true);
        view.selectPixel(1674, 485, 0);
        QVERIFY(view.finish(view.requestId(), report));
        auto label = view.findChild<QLabel *>("debugGpuReference");
        QVERIFY(label);
        QVERIFY(!label->isHidden());
        QVERIFY(label->text().contains("GPU target after event"));
        QVERIFY(label->text().contains("Read before debugger replay"));
        const auto model = view.model();
        QVERIFY(model);
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(int(model->size() - 1));
        model->addWatch("o0.x");
        QCOMPARE(model->watchResults().at(0).at("status"), Json("available"));
        QVERIFY(model->watchResults().at(0).at("values").at(0).get<double>() > 0.0);
        // This selected copy branch has no blend/conversion difference. Other
        // draws are not required to match their post-event target reference.
        QCOMPARE(model->watchResults().at(0).at("values").at(0),
                 reference.at("value").at("floatValue").at(0));
        QCOMPARE(view.exportReport().at("gpu_reference"), reference);
        QCoreApplication::processEvents();
        if (!output_.isEmpty())
            QVERIFY(view.grab().save(output_ + "/debug-gpu-reference.png"));
        auto unavailable = report;
        unavailable["gpu_reference"] = {{"status", "unavailable"}, {"reason", "Ambiguous output layer"}};
        QVERIFY(view.finish(view.requestId(), unavailable));
        QVERIFY(label->text().contains("Ambiguous output layer"));
        const auto old = view.requestId();
        view.setContext("changed-experiment", 36758, true);
        QVERIFY(label->isHidden());
        QVERIFY(!view.finish(old, report));
        auto legacy = report;
        legacy.erase("gpu_reference");
        QVERIFY(view.finish(view.requestId(), legacy));
        QVERIFY(label->isHidden());
    }
    void dx12PipelineReport() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PIPELINE_REPORT");
        if (path.isEmpty()) QSKIP("Set FLORA_DX12_PIPELINE_REPORT to a real pipeline result");
        const auto report = read(path);
        QVERIFY(report.at("ok").get<bool>());
        Dx12PipelineView view;
        view.resize(1400, 850); view.show();
        view.setContext("bf1:experiment:1", 37273);
        QCOMPARE(view.request().at("execution"), Json("37273"));
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("dx12PipelineRows");
        QCOMPARE(rows->topLevelItemCount(), int(report.at("pipeline_rows").size()));
        QSignalSpy jump(&view, &Dx12PipelineView::resourceRequested);
        bool targetFound = false, shaderFound = false;
        for (int i = 0; i < rows->topLevelItemCount(); ++i) {
            const auto &row = report.at("pipeline_rows")[i];
            rows->setCurrentItem(rows->topLevelItem(i));
            if (row.at("section") == "PS" && row.at("binding") == "Shader") {
                shaderFound = true; QVERIFY(!action(view, "inspectPipelineResource")->isEnabled());
            }
            if (row.at("section") == "OM" && row.at("binding") == "RTV 0") {
                QCOMPARE(row.at("resource").at("resource_creation"), Json("11399"));
                QCOMPARE(row.at("resource").at("resource_object"), Json("2119382448"));
                action(view, "inspectPipelineResource")->trigger();
                QCOMPARE(jump.size(), 1); QCOMPARE(jump.takeFirst()[0].toULongLong(), 11399ULL);
                targetFound = true;
            }
        }
        QVERIFY(targetFound); QVERIFY(shaderFound);
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->text(1).startsWith("SRV mainTexture ")) rows->setCurrentItem(rows->topLevelItem(i));
        QCoreApplication::processEvents();
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/pipeline.png"));
        QTemporaryDir dir;
        view.exportResult(dir.path() + "/pipeline.json"); QCOMPARE(read(dir.path() + "/pipeline.json"), report);
        const auto previous = view.requestId();
        view.setContext("bf1:experiment:2", 37273);
        QVERIFY(!view.finish(previous, report)); QCOMPARE(rows->topLevelItemCount(), 0);
        auto huge = report;
        huge["pipeline_rows"][0]["resource"].update({{"mapping_status", "capture_resource_marker"},
            {"resource_creation", "9007199254740993"}, {"resource_object", "18446744073709551615"}});
        QVERIFY(view.finish(view.requestId(), huge)); rows->setCurrentItem(rows->topLevelItem(0));
        action(view, "inspectPipelineResource")->trigger();
        QCOMPARE(jump.takeFirst()[0].toULongLong(), 9007199254740993ULL);
        huge["pipeline_rows"][0]["resource"]["resource_creation"] = 9007199254740993ULL;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), huge));
        huge = report; huge["execution"] = "37274";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), huge));
        const auto beforeSwitch = view.requestId(); view.setContext("", 0);
        QVERIFY(!view.finish(beforeSwitch, report)); QVERIFY(!action(view, "readDx12Pipeline")->isEnabled());
    }
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-HistoryTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty())
            QVERIFY(QDir().mkpath(output_));
    }
    void bundledDefaults() {
        const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath("RenderDoc/renderdoc.dll");
        if (!QFileInfo::exists(bundled)) QSKIP("Deploy the pinned runtime beside the test");
        QSettings().remove("analysis/renderdoc");
        PixelHistoryView history;
        ReplayDebugView debug("ps");
        ReplayMeshView mesh;
        RdcCountersView counters;
        QCOMPARE(history.backendPath(), bundled);
        QCOMPARE(debug.backendPath(), bundled);
        QCOMPARE(mesh.backendPath(), bundled);
        QCOMPARE(counters.backendPath(), bundled);
        QVERIFY(historyBackendCompatible(bundled));
        QVERIFY(!QSettings().contains("analysis/renderdoc"));
    }
    void d16MultiFragmentReports() {
        const auto root = qEnvironmentVariable("FLORA_D16_MULTI_REPORTS");
        if (root.isEmpty()) QSKIP("Set FLORA_D16_MULTI_REPORTS to the Talos two-layer experiment evidence");
        for (const auto &name : {QString("rejected"), QString("passing")}) {
            const auto result = read(root + "/talos-depth-" + name + "-history2/result.json");
            const auto native = read(root + "/talos-depth-" + name + "1/report.json");
            QVERIFY(result.at("ok").get<bool>()); QVERIFY(result.at("partial").get<bool>());
            QVERIFY(native.at("completed").get<bool>());
            QCOMPARE(native.at("device").at("validation_error_count"), Json("0"));
            QCOMPARE(result.at("history").size(), size_t(3));
            const auto &first = result.at("history")[1], &second = result.at("history")[2];
            QCOMPARE(first.at("primitiveID"), Json(0)); QCOMPARE(second.at("primitiveID"), Json(1));
            QCOMPARE(first.at("execution"), Json("12675")); QCOMPARE(second.at("execution"), Json("12675"));
            QCOMPARE(first.at("source_record"), Json("185550"));
            QCOMPARE(second.at("source_record"), Json("185550"));
            QVERIFY(!first.at("depthTestFailed").get<bool>());
            QCOMPARE(second.at("depthTestFailed").get<bool>(), name == "rejected");
            QCOMPARE(first.at("postMod").at("depth"), second.at("preMod").at("depth"));
            QCOMPARE(first.at("shaderOut").at("depth"), Json(name == "rejected" ? 0.75 : 0.25));
            QCOMPARE(second.at("shaderOut").at("depth"), Json(name == "rejected" ? 0.25 : 0.75));
            QCOMPARE(result.at("history_scope").at("reconstructed_depth_values"), Json(2));
            QFile storage(root + "/talos-depth-" + name + "1/output_storage.bin");
            QVERIFY(storage.open(QIODevice::ReadOnly)); QVERIFY(storage.seek((1240 * 2048 + 1527) * 2));
            const auto bytes = storage.read(2); QCOMPARE(bytes.size(), 2);
            uint16_t raw{}; memcpy(&raw, bytes.constData(), 2);
            QCOMPARE(raw, uint16_t(49151));
            QCOMPARE(std::lround(second.at("postMod").at("depth").get<double>() * 65535), long(raw));
            PixelHistoryView view; view.resize(1400, 650);
            view.setContext("multi-d16-fixture", 12675, true);
            view.setDx12Resource(167081, 4980699392ULL);
            view.selectPixel(4980699392ULL, 1527, 1240, 0, 1, 0);
            QVERIFY(view.finish(view.requestId(), result));
            auto rows = view.findChild<QTreeWidget *>("historyRecords");
            QCOMPARE(rows->topLevelItemCount(), 3);
            QVERIFY(!rows->isColumnHidden(2));
            QCOMPARE(rows->topLevelItem(1)->text(2), QString("0 / 0"));
            QCOMPARE(rows->topLevelItem(2)->text(2), QString("1 / 1"));
            QVERIFY(rows->topLevelItem(1)->text(5).contains("D32 intermediate"));
            QVERIFY(rows->topLevelItem(2)->text(4).contains("D32 intermediate"));
            QVERIFY(rows->topLevelItem(2)->text(5).startsWith("Depth 0.7499962"));
            QVERIFY(!rows->topLevelItem(2)->text(5).contains("Stencil"));
            QCOMPARE(rows->topLevelItem(2)->text(3).contains("Depth test"), name == "rejected");
            rows->setCurrentItem(rows->topLevelItem(2));
            QSignalSpy jump(&view, &PixelHistoryView::eventRequested);
            action(view, "locateHistoryEvent")->trigger();
            QCOMPARE(jump.size(), 1); QCOMPARE(jump.front().front().toULongLong(), 12675ULL);
            if (!output_.isEmpty()) {
                view.show(); QTest::qWait(30);
                QVERIFY(view.grab().save(output_ + '/' + name + ".png"));
            }
        }
    }
    void d16PatchedReports() {
        const auto root = qEnvironmentVariable("FLORA_D16_HISTORY_REPORTS");
        if (root.isEmpty()) QSKIP("Set FLORA_D16_HISTORY_REPORTS to the focused Talos D16 reports");
        QTemporaryDir temporary;
        for (const auto &name : {QString("original"), QString("modified")}) {
            const auto result = read(root + '/' + name + ".json");
            QVERIFY(result.at("ok").get<bool>());
            QCOMPARE(result.at("backend_d16_history_patch"), Json(1));
            QCOMPARE(result.at("history_scope").at("depth_value_status"), Json("patched_backend_values"));
            QCOMPARE(result.at("history").size(), size_t(2));
            const auto &draw = result.at("history").back();
            QCOMPARE(draw.at("execution"), Json("12675"));
            QCOMPARE(draw.at("source_record"), Json("185550"));
            QCOMPARE(draw.at("primitiveID"), Json(12999));
            QCOMPARE(std::lround(draw.at("postMod").at("depth").get<double>() * 65535),
                     name == "original" ? 17980L : 16384L);
            PixelHistoryView view;
            view.setContext("d16-fixture", 12675, true);
            view.setDx12Resource(167081, 4980699392ULL);
            QVERIFY(view.finish(view.requestId(), result));
            QVERIFY(view.findChild<QLabel *>("historySummary")->text().contains("patched RenderDoc 1.45"));
            auto rows = view.findChild<QTreeWidget *>("historyRecords");
            QCOMPARE(rows->topLevelItemCount(), 2);
            rows->setCurrentItem(rows->topLevelItem(1));
            QSignalSpy jump(&view, &PixelHistoryView::eventRequested);
            action(view, "locateHistoryEvent")->trigger();
            QCOMPARE(jump.size(), 1);
            QCOMPARE(jump.front().front().toULongLong(), 12675ULL);
            const auto exported = temporary.path() + '/' + name + ".json";
            view.exportResult(exported);
            QCOMPARE(read(exported), result);
        }
        const auto stock = read(root + "/stock.json");
        PixelHistoryView view;
        view.setContext("stock-d16-fixture", 12675, true);
        QVERIFY(view.finish(view.requestId(), stock));
        QVERIFY(view.findChild<QLabel *>("historySummary")->text().contains("event-boundary depths only"));
        QVERIFY(stock.at("history").back().at("postMod").at("depth").is_null());
    }
    void backendReports() {
        const auto root = qEnvironmentVariable("FLORA_HISTORY_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_HISTORY_REPORTS to the native/Python history comparison corpus");
        const auto directories = QDir(root).entryList({"*-native"}, QDir::Dirs);
        int verified = 0;
        for (const auto &directory : directories) {
            const auto result = read(root + '/' + directory + "/result.json");
            if (!result.value("ok", false) || result.value("action", "") != "history")
                continue;
            PixelHistoryView view;
            view.resize(1200, 660);
            view.show();
            view.setContext("fixture", 1);
            QVERIFY(view.finish(view.requestId(), result));
            auto rows = view.findChild<QTreeWidget *>("historyRecords");
            QCOMPARE(rows->topLevelItemCount(), int(result["history"].size()));
            QSignalSpy jump(&view, &PixelHistoryView::eventRequested);
            for (int i = 0; i < rows->topLevelItemCount(); ++i) {
                const auto &record = result["history"][size_t(i)];
                auto item = rows->topLevelItem(i);
                rows->setCurrentItem(item);
                QCOMPARE(Json::parse(
                             view.findChild<QPlainTextEdit *>("historyDetails")->toPlainText().toStdString()),
                         record);
                if (record["gpa_event"].is_null())
                    QVERIFY(!action(view, "locateHistoryEvent")->isEnabled());
                else {
                    jump.clear();
                    action(view, "locateHistoryEvent")->trigger();
                    QCOMPARE(jump.size(), 1);
                    QCOMPARE(jump[0][0].toULongLong(), qulonglong(record["gpa_event"].get<uint64_t>()));
                }
                if (record["record_kind"] == "cpu_write_snapshot") {
                    QCOMPARE(item->text(2), QString("—"));
                    QVERIFY(item->text(3).contains("CPU snapshot"));
                }
                if (record["color_interpretation"] == "uint")
                    QCOMPARE(item->text(5).split(", ")[0],
                             QString::number(record["postMod"]["col"]["uintValue"][0].get<uint32_t>()));
                if (record["color_interpretation"] == "sint")
                    QCOMPARE(item->text(5).split(", ")[0],
                             QString::number(record["postMod"]["col"]["intValue"][0].get<int32_t>()));
            }
            QTemporaryDir temporary;
            const auto path = temporary.path() + "/history.json";
            view.exportResult(path);
            QCOMPARE(read(path), result);
            // A later event selection may retain this frame/experiment snapshot for navigation.
            view.setContext("fixture", 2);
            QVERIFY(action(view, "exportPixelHistory")->isEnabled());
            const auto old = view.requestId();
            view.setContext("changed-experiment", 2);
            QVERIFY(!view.finish(old, result));
            QCOMPARE(rows->topLevelItemCount(), 0);
            QVERIFY(!action(view, "exportPixelHistory")->isEnabled());
            ++verified;
        }
        QCOMPARE(verified, 22);
    }
    void cpuWriteMainWindow() {
        const auto path = qEnvironmentVariable("FLORA_HISTORY_COMMANDS");
        if (path.isEmpty() || !flora::historyBackendCompatible(flora::defaultRenderDocLibrary()))
            QSKIP("Set FLORA_HISTORY_COMMANDS to the command fixture and install RenderDoc 1.45");
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto view = window.findChild<PixelHistoryView *>();
        QVERIFY(view);
        QVERIFY(showHistoryTab(window, view));
        view->selectPixel(84, 0, 0, 1, 0, 0);
        view->findChild<QLineEdit *>("historyEvent")->setText("213");
        action(*view, "readPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto rows = view->findChild<QTreeWidget *>("historyRecords");
        QTreeWidgetItem *cpu{};
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->text(0) == "213")
                cpu = rows->topLevelItem(i);
        QVERIFY(cpu);
        QCOMPARE(cpu->text(2), QString("—"));
        QVERIFY(cpu->text(3).contains("CPU snapshot"));
        rows->setCurrentItem(cpu);
        action(*view, "historyDetailsToggle")->trigger();
        QTRY_VERIFY(view->findChild<QPlainTextEdit *>("historyDetails")->isVisible());
        QTRY_VERIFY(view->findChild<QPlainTextEdit *>("historyDetails")->height() > 50);
        if (!output_.isEmpty()) {
            view->exportResult(output_ + "/history-cpu.json");
            const auto report = read(output_ + "/history-cpu.json");
            const auto capture = QString::fromStdString(report.at("capture").get<std::string>());
            QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"),
                                output_ + "/recapture-cpu.json"));
            QVERIFY(window.grab().save(output_ + "/history-cpu.png"));
        }
        window.findChild<QLineEdit *>("apiSearch")->setText("not present");
        action(*view, "locateHistoryEvent")->trigger();
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 qulonglong(213));
        QVERIFY(rows->topLevelItemCount() > 0);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
    }
    void mainWindow_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void mainWindow() {
        QFETCH(bool, warp);
        if (!flora::historyBackendCompatible(flora::defaultRenderDocLibrary()))
            QSKIP("Optional RenderDoc 1.45 is not installed");
        QTemporaryDir directory;
        const auto frame = directory.path() + "/msaa.gpa_frame";
        testing::msaaOutputCapture().save(frame);
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QStringList dialogs;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, [&] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(widget)) {
                    dialogs << box->text();
                    box->reject();
                }
        });
        dismiss.start(25);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(frame);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        QVERIFY(adapter);
        if (adapter->currentIndex() != int(warp)) {
            adapter->setCurrentIndex(int(warp));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
        }
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 110) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto history = window.findChild<PixelHistoryView *>();
        QVERIFY(history);
        QVERIFY(showHistoryTab(window, history));
        history->selectPixel(20, 2, 2, 0, 0, 2);
        history->findChild<QLineEdit *>("historyEvent")->setText("100");
        auto readAction = action(*history, "readPixelHistory");
        QVERIFY(readAction->isEnabled());
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        const auto exported = directory.path() + "/history.json";
        history->exportResult(exported);
        const auto original = read(exported);
        QVERIFY(original["history_events"].get<size_t>() > 0);
        const auto &last = original["history"].back();
        QCOMPARE(last["gpa_event"].get<uint64_t>(), uint64_t(100));
        QCOMPARE(last["postMod"]["col"]["uintValue"][0].get<uint32_t>(), uint32_t(12));
        QCOMPARE(last["postMod"]["col"]["uintValue"][1].get<uint32_t>(), uint32_t(16777219));
        const auto firstCapture = original["capture"];
        history->findChild<QSpinBox *>("historyX")->setValue(3);
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        history->exportResult(exported);
        QCOMPARE(read(exported)["capture"], firstCapture);
        // Cancellation and retry use the existing Windows job isolation.
        readAction->trigger();
        action(*history, "cancelPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
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
        QCOMPARE(history->findChild<QTreeWidget *>("historyRecords")->topLevelItemCount(), 0);
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        history->exportResult(exported);
        const auto edited = read(exported);
        QVERIFY(edited["capture"] != firstCapture);
        QVERIFY(!edited["gpa_command_map"].contains("110"));
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        history->exportResult(exported);
        QCOMPARE(read(exported)["capture"], firstCapture);
        if (!output_.isEmpty()) {
            history->exportResult(output_ + (warp ? "/history-warp.json" : "/history-hardware.json"));
            const auto capture = QString::fromStdString(firstCapture.get<std::string>());
            QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"),
                                output_ + (warp ? "/recapture-warp.json" : "/recapture-hardware.json")));
            QVERIFY(window.grab().save(output_ + (warp ? "/history-warp.png" : "/history-hardware.png")));
        }
        QVERIFY2(dialogs.empty(), qPrintable(dialogs.join('\n')));
        // API navigation remains usable under filters and keeps the historical rows visible.
        auto rows = history->findChild<QTreeWidget *>("historyRecords");
        rows->setCurrentItem(rows->topLevelItem(0));
        window.findChild<QLineEdit *>("apiSearch")->setText("no match");
        action(*history, "locateHistoryEvent")->trigger();
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(100));
        QVERIFY(rows->topLevelItemCount() > 0);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        history->setBackendPath(directory.path() + "/missing.dll");
        readAction->trigger();
        QCOMPARE(tasks.size(), 1);
        QVERIFY(!tasks.takeLast()[0].toBool());
        history->setBackendPath(flora::defaultRenderDocLibrary());
        readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        history->exportResult(exported);
        QCOMPARE(read(exported)["capture"], firstCapture);
        // A changed API selection cancels an active analysis, rather than accepting stale rows.
        readAction->trigger();
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 110) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTest::qWait(1000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QCOMPARE(rows->topLevelItemCount(), 0);
        QVERIFY(!action(*history, "exportPixelHistory")->isEnabled());
        if (!warp) {
            // A selected output pixel fills the history query without replacing the Output tab.
            auto output = window.findChild<ImageView *>("frameOutput");
            QVERIFY(output);
            QVERIFY(QMetaObject::invokeMethod(output, "pixelSelected", Qt::DirectConnection, Q_ARG(int, 2),
                                              Q_ARG(int, 1), Q_ARG(QColor, QColor(1, 2, 3))));
            QCOMPARE(history->findChild<QSpinBox *>("historyX")->value(), 2);
            QCOMPARE(history->findChild<QSpinBox *>("historyY")->value(), 1);
            QCOMPARE(history->findChild<QLineEdit *>("historyResource")->text(), QString("20"));
        }
    }
};
QTEST_MAIN(HistoryUiTests)
#include "HistoryUiTests.moc"
