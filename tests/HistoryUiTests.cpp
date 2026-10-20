#include "MsaaCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include "app/ResourceHistoryView.h"
#include "app/ReplayDebugView.h"
#include "app/ReplayMeshView.h"
#include "app/RdcCountersView.h"
#include "app/Dx12PipelineView.h"
#include "application/RenderDocLibrary.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <bit>
#include <cmath>
#include <set>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTextBlock>
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
    void layerTestHistory() {
        const auto root=qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the array-layer test evidence directory");
        const auto before=read(root + "/half-blend-layer-cull1/before/result.json");
        QCOMPARE(before.at("history").back().at("backfaceCulled"),Json(false));
        const auto cull=read(root + "/half-blend-layer-cull1/final/result.json");
        QCOMPARE(cull.at("backend_dx12_layer_test_history_patch"),Json(1));
        QCOMPARE(cull.at("history_scope").at("array_layer_test_reason_status"),Json("backend_selected_layer_probes"));
        QVERIFY(cull.at("history_scope").at("array_layer_test_probes").at("fixed_shader_queries").get<uint32_t>() > 0);
        QCOMPARE(cull.at("history_scope").at("array_layer_test_probes").at("failures"),Json(0));
        for (const auto &kind : {QString("cull"),QString("discard"),QString("depth")}) {
            const auto folder=root + "/half-blend-layer-" + kind + "1/";
            const auto report=kind == "cull" ? cull : read(folder + (kind == "depth" ? "rejected" : "after") + "/result.json");
            const auto native=read(folder + "gpu.json");
            const auto flag=kind == "cull" ? "backfaceCulled" : kind == "depth" ? "depthTestFailed" : "shaderDiscarded";
            size_t draws{};
            for (const auto &row : report.at("history")) {
                if (row.value("record_kind","") != "backend_modification") continue;
                QCOMPARE(row.at(flag),Json(true)); ++draws;
                for (size_t c=0;c<4;++c) {
                    const auto bits=native.at("prefix_half_words")[0][c].get<uint16_t>();
                    QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),
                        std::ldexp(1.0 + double(bits & 1023) / 1024.0,int(bits >> 10)-15));
                }
            }
            QCOMPARE(draws,kind == "cull" ? size_t(3) : size_t(5));
            PixelHistoryView view; view.setContext("array-test:" + kind,3,true); view.setDx12Resource(1,1001);
            view.selectPixel(1001,5,5,0,1,0); QVERIFY(view.finish(view.requestId(),report));
            auto rows=view.findChild<QTreeWidget *>("historyRecords");
            const auto last=rows->topLevelItem(rows->topLevelItemCount()-1);
            QVERIFY(last->text(3).contains(kind == "cull" ? "Backface" : kind == "depth" ? "Depth test" : "Discard"));
            rows->setCurrentItem(last); QSignalSpy jump(&view,&PixelHistoryView::eventRequested);
            action(view,"locateHistoryEvent")->trigger(); QCOMPARE(jump.count(),1); QCOMPARE(jump[0][0].toULongLong(),3ULL);
            if (kind == "cull") QVERIFY(view.findChild<QLabel *>("historySummary")->text().contains("selected-layer GPU queries"));
            if (!output_.isEmpty()) {
                const auto file=output_ + "/layer-test-" + kind + ".json";
                view.exportResult(file); QCOMPARE(read(file),report);
            }
        }
        const auto passed=read(root + "/half-blend-layer-cull1/passing-layer/result.json");
        QCOMPARE(passed.at("history").size(),size_t(4));
        QCOMPARE(passed.at("history").back().at("backfaceCulled"),Json(false));
    }
    void routedLayerHistory() {
        const auto root=qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the routed-layer history evidence directory");
        const auto negative=read(root + "/half-blend-routed1/other-layer-masked/result.json");
        QCOMPARE(negative.at("history").size(),size_t(3));
        for (const auto &row : negative.at("history"))
            QCOMPARE(row.at("record_kind"),Json("resource_operation"));
        for (int layer=0;layer<2;++layer) {
            const auto report=read(root + "/half-blend-split1/" + (layer ? "layer1-scoped" : "layer0") + "/result.json");
            if (layer) {
                QCOMPARE(report.at("partial"),Json(true));
                QCOMPARE(report.at("backend_dx12_routed_layer_history_patch"),Json(1));
                QCOMPARE(report.at("history_scope").at("array_layer_test_reason_status"),Json("unverified"));
            }
            const auto native=read(root + "/half-blend-split1/" + (layer ? "gpu.json" : "native-layer0.json"));
            QCOMPARE(report.at("history").size(),layer ? size_t(8) : size_t(4));
            size_t fragment{};
            for (const auto &row : report.at("history")) {
                if (row.value("execution",Json()) != "3") continue;
                QVERIFY(fragment < (layer ? 2 : 1));
                const size_t nativePrefix=layer ? fragment : 2;
                for (size_t c=0;c<4;++c) {
                    const auto bits=native.at("prefix_half_words")[nativePrefix][c].get<uint16_t>();
                    QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),
                        std::ldexp(1.0 + double(bits & 1023) / 1024.0,int(bits >> 10)-15));
                }
                ++fragment;
            }
            QCOMPARE(fragment,layer ? size_t(2) : size_t(1));
            PixelHistoryView pixel; pixel.setContext("routed:layer",3,true); pixel.setDx12Resource(1,1001);
            pixel.selectPixel(1001,5,5,0,layer,0); QVERIFY(pixel.finish(pixel.requestId(),report));
            auto rows=pixel.findChild<QTreeWidget *>("historyRecords");
            QCOMPARE(rows->topLevelItemCount(),int(report.at("history").size()));
            rows->setCurrentItem(rows->topLevelItem(rows->topLevelItemCount()-1));
            // This GS passes its color selector through TEXCOORD0; it does not
            // output SV_PrimitiveID. An unavailable ID must not launch PS debugging.
            QVERIFY(!action(pixel,"debugHistoryPixel")->isEnabled());
            QSignalSpy jump(&pixel,&PixelHistoryView::eventRequested);
            action(pixel,"locateHistoryEvent")->trigger();
            QCOMPARE(jump.count(),1); QCOMPARE(jump[0][0].toULongLong(),3ULL);
            if (!output_.isEmpty()) {
                const auto file=output_ + "/routed-layer" + QString::number(layer) + ".json";
                pixel.exportResult(file); QCOMPARE(read(file),report);
            }
        }
    }
    void fixedLayerHistory() {
        const auto root=qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the fixed-layer history evidence directory");
        const auto before=read(root + "/half-blend-default1/other-layer-before/result.json");
        const auto after=read(root + "/half-blend-default1/other-layer-after/result.json");
        QCOMPARE(before.at("history").size(),size_t(9));
        QCOMPARE(after.at("backend_dx12_default_view_history_patch"),Json(2));
        QCOMPARE(after.at("history").size(),size_t(3));
        for (const auto &row : after.at("history"))
            QCOMPARE(row.at("record_kind"),Json("resource_operation"));
        PixelHistoryView pixel; pixel.setContext("default:layer",3,true); pixel.setDx12Resource(1,1001);
        pixel.selectPixel(1001,5,5,0,1,0); QVERIFY(pixel.finish(pixel.requestId(),after));
        auto rows=pixel.findChild<QTreeWidget *>("historyRecords");
        QCOMPARE(rows->topLevelItemCount(),3);
        rows->setCurrentItem(rows->topLevelItem(0));
        QVERIFY(!action(pixel,"debugHistoryPixel")->isEnabled());
        const auto positive=read(root + "/half-blend-array1/fixed-layer-positive/result.json");
        const auto native=read(root + "/half-blend-array1/gpu.json");
        size_t fragment{};
        for (const auto &row : positive.at("history")) {
            if (row.value("execution",Json()) != "3") continue;
            QVERIFY(fragment < 3);
            for (size_t c=0;c<4;++c) {
                const auto bits=native.at("prefix_half_words")[fragment][c].get<uint16_t>();
                QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),
                    std::ldexp(1.0 + double(bits & 1023) / 1024.0,int(bits >> 10)-15));
            }
            ++fragment;
        }
        QCOMPARE(fragment,size_t(3));
        if (!output_.isEmpty()) {
            pixel.exportResult(output_ + "/fixed-layer-history.json");
            QCOMPARE(read(output_ + "/fixed-layer-history.json"),after);
        }
    }
    void defaultViewHistory() {
        const auto root=qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the default-view history evidence directory");
        const auto base=root + "/half-blend-default1/";
        const auto before=read(base + "other-mip-before/result.json");
        const auto after=read(base + "other-mip-after/result.json");
        QCOMPARE(before.at("history").size(),size_t(6));
        QVERIFY(after.at("history").empty());
        QCOMPARE(after.at("backend_dx12_default_view_history_patch"),Json(1));
        QCOMPARE(after.at("history_scope").at("excluded_resource_operations").size(),size_t(3));
        const auto positive=read(base + "positive/result.json"), native=read(base + "gpu.json");
        size_t fragment{};
        for (const auto &row : positive.at("history")) {
            if (row.value("execution",Json()) != "3") continue;
            QVERIFY(fragment < 3);
            for (size_t c=0;c<4;++c) {
                const auto bits=native.at("prefix_half_words")[fragment][c].get<uint16_t>();
                QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),
                    std::ldexp(1.0 + double(bits & 1023) / 1024.0,int(bits >> 10)-15));
            }
            ++fragment;
        }
        QCOMPARE(fragment,size_t(3));
        const auto layer=read(base + "clear-layer1/result.json");
        QCOMPARE(layer.at("history").size(),size_t(1));
        const auto &clear=layer.at("history")[0];
        QCOMPARE(clear.at("coverage").at("view_definition"),Json("default_resource_view"));
        QCOMPARE(clear.at("coverage").at("mip"),Json(0));
        QCOMPARE(clear.at("coverage").at("layer_count"),Json(2));
        QCOMPARE(clear.at("postMod").at("col").at("floatValue"),
            Json::array({0.027862548828125,0.0273284912109375,0.024871826171875,0.3974609375}));
        QCOMPARE(layer.at("history_scope").at("replay_consistency").at("history_boundary").at("status"),Json("matches_selected_texel"));
        PixelHistoryView pixel; pixel.setContext("default:mip",3,true); pixel.setDx12Resource(1,1001);
        pixel.selectPixel(1001,5,5,1,0,0); QVERIFY(pixel.finish(pixel.requestId(),after));
        QCOMPARE(pixel.findChild<QTreeWidget *>("historyRecords")->topLevelItemCount(),0);
        const auto resource=read(root + "/half-blend-operation-ranges1/default-source/result.json");
        QCOMPARE(resource.at("history_scope").at("captured_operation_ranges"),Json(4));
        QCOMPARE(resource.at("history_scope").at("operation_range_gaps"),Json(0));
        ResourceHistoryView view; view.setContext("default:resource",3,2,1002);
        QVERIFY(view.finish(view.requestId(),resource));
        QVERIFY(view.findChild<QTreeWidget *>("resourceHistoryRows")->topLevelItem(0)->text(4).contains("default view"));
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/default-resource-history.json");
            QCOMPARE(read(output_ + "/default-resource-history.json"),resource);
        }
    }
    void isolatedQueryResolveHistory() {
        const auto root=qEnvironmentVariable("FLORA_QUERY_RESOLVE_HISTORY");
        if (root.isEmpty()) QSKIP("Set the verified Talos query-resolve history directory");
        const auto report=read(root + "/full/result.json"), through=read(root + "/through/result.json");
        QCOMPARE(report.at("frame_complete"),Json(false));
        QVERIFY(report.at("query_resolve_mapping_gaps").empty());
        QCOMPARE(report.at("history_scope").at("captured_operation_ranges"),Json(52));
        QCOMPARE(report.at("history_scope").at("operation_range_gaps"),Json(0));
        QCOMPARE(report.at("history_scope").at("unmapped_uses"),Json(0));
        QCOMPARE(report.at("resource_history").size(),size_t(52));
        QCOMPARE(report.at("resource_history"),through.at("resource_history"));
        QCOMPARE(through.at("execution"),Json("16001"));
        QCOMPARE(through.at("history_scope").at("through_event"),through.at("resource_history_selection").at("eventId"));
        QCOMPARE(through.at("history_scope").at("selected_event_replayed"),Json(false));
        uint64_t bytes{},previousEnd{};
        for (size_t i=0;i<report.at("resource_history").size();++i) {
            const auto &row=report.at("resource_history")[i], &range=row.at("operation_ranges").at("ranges")[0];
            QCOMPARE(row.at("execution"),Json("16001")); QCOMPARE(row.at("source_record"),Json("188923"));
            QCOMPARE(row.at("selectable"),Json(true)); QCOMPARE(row.at("query_resolve_part"),Json(i));
            QCOMPARE(row.at("query_resolve_part_count"),Json(52));
            QCOMPARE(row.at("content_change"),Json("not_measured"));
            const auto offset=std::stoull(range.at("offset").get<std::string>());
            const auto size=std::stoull(range.at("byte_length").get<std::string>());
            QVERIFY(offset >= previousEnd); previousEnd=offset+size; bytes+=size;
            QCOMPARE(size,range.at("query_count").get<uint64_t>()*8);
        }
        QCOMPARE(bytes,768ULL);
        ResourceHistoryView view; view.setContext("isolated:query",19410,78132,1643440736,true);
        QVERIFY(view.finish(view.requestId(),report));
        auto rows=view.findChild<QTreeWidget *>("resourceHistoryRows");
        QVERIFY(rows->topLevelItem(0)->text(0).contains("resolve part 1/52"));
        QVERIFY(rows->topLevelItem(0)->text(4).contains("bytes [64, 72)"));
        rows->setCurrentItem(rows->topLevelItem(51));
        QSignalSpy locate(&view,&ResourceHistoryView::eventRequested);
        action(view,"locateResourceHistoryEvent")->trigger();
        QCOMPARE(locate.count(),1); QCOMPARE(locate[0][0].toULongLong(),16001ULL);
        QCOMPARE(locate[0][1].toLongLong(),-1LL); // Original execution, not an indirect action.
        auto invalid=report; invalid["resource_history"][0]["query_resolve_part"]=52;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view.finish(view.requestId(),invalid));
        const auto old=view.requestId(); view.setContext("isolated:through-query",16001,78132,1643440736,true);
        QVERIFY(!view.finish(old,report)); QVERIFY(view.finish(view.requestId(),through));
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/query-resolve-history.json");
            QCOMPARE(read(output_ + "/query-resolve-history.json"),through);
        }
    }
    void isolatedQueryPixelHistoryWorkspace() {
        try {
            const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
            const auto output = qEnvironmentVariable("FLORA_QUERY_PIXEL_HISTORY_UI");
            const auto library = qEnvironmentVariable("FLORA_QUERY_HISTORY_RENDERDOC");
            if (captures.isEmpty() || output.isEmpty() || library.isEmpty())
                QSKIP("Set Talos captures, a new pixel-history output directory and the pinned RenderDoc "
                      "library");
            QVERIFY(!QFileInfo::exists(output));
            QVERIFY(QDir().mkpath(output));
            MainWindow window;
            QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
            window.openCapture(captures + "/Talos_2026_10_18__00_55_08.gpa_frame");
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
            window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
            auto locate = window.findChild<QLineEdit *>("apiLocate");
            locate->setText("19408");
            QTest::keyClick(locate, Qt::Key_Return);
            auto resources = window.findChild<QTableView *>("resources");
            QModelIndex selected;
            for (int row = 0; row < resources->model()->rowCount(); ++row)
                if (resources->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 35118)
                    selected = resources->model()->index(row, 0);
            QVERIFY(selected.isValid());
            resources->setCurrentIndex(selected);
            auto view = window.findChild<PixelHistoryView *>();
            QVERIFY(view);
            view->setBackendPath(library, false);
            QVERIFY(!action(*view, "readPixelHistory")->isEnabled());
            auto isolation = window.findChild<QCheckBox *>("dx12QueryIsolation");
            isolation->setChecked(true);
            window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
            done.clear();
            window.replay();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY(done.takeLast()[0].toBool());
            QVERIFY(!window.busy());
            auto image = window.findChild<ImageView *>("frameOutput");
            QCOMPARE(image->image().size(), QSize(1920, 1080));
            auto pick = action(window, "pickPixelHistory");
            QVERIFY(pick->isEnabled());
            pick->setChecked(true);
            done.clear();
            QVERIFY(QMetaObject::invokeMethod(image, "pixelSelected", Qt::DirectConnection, Q_ARG(int, 65),
                                              Q_ARG(int, 172),
                                              Q_ARG(QColor, image->image().pixelColor(65, 172))));
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 180000);
            QVERIFY2(done.takeLast()[0].toBool(),
                     qPrintable(view->findChild<QLabel *>("historySummary")->text()));
            QVERIFY(!window.busy());
            view->exportResult(output + "/result.json");
            const auto result = read(output + "/result.json");
            const auto capture = QString::fromStdString(result.at("capture").get<std::string>());
            QVERIFY(
                QFile::copy(QFileInfo(capture).dir().filePath("report.json"), output + "/recapture.json"));
            QCOMPARE(result.at("execution"), Json("19408"));
            QCOMPARE(result.at("frame_complete"), Json(false));
            QCOMPARE(result.at("partial"), Json(true));
            QCOMPARE(result.at("history_scope").at("query_outputs_incomplete"), Json(true));
            QCOMPARE(result.at("query_isolation").at("selected_execution"), Json("19408"));
            QCOMPARE(result.at("request_identity").at("missing_query_policy"),
                     Json("isolate_readback_outputs"));
            QCOMPARE(view->request().at("isolate_missing_queries"), Json(true));
            QCOMPARE(view->request().at("resource_creation"), Json("35118"));
            const auto &consistency = result.at("history_scope").at("replay_consistency");
            QCOMPARE(consistency.at("status"), Json("selected_texel_unchanged"));
            QCOMPARE(consistency.at("history_boundary").at("status"), Json("matches_selected_texel"));
            QVERIFY(
                view->findChild<QLabel *>("historySummary")->text().contains("Incomplete query-isolated"));
            auto rows = view->findChild<QTreeWidget *>("historyRecords");
            QVERIFY(rows->topLevelItemCount() > 0);
            int fragment = -1;
            for (size_t i = 0; i < result.at("history").size(); ++i) {
                const auto &row = result.at("history")[i];
                if (row.value("execution", Json()) == "19408" && row.value("fragIndex", Json()) == 0) {
                    QCOMPARE(row.at("postMod").at("col").at("uintValue"),
                             Json::array({1015054465U, 1015054465U, 998277249U, 1060155569U}));
                    fragment = int(i);
                }
            }
            QVERIFY(fragment >= 0);
            for (const auto *key : {"frame_complete", "partial"}) {
                auto invalid = result;
                invalid[key] = !result.at(key).get<bool>();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), invalid));
            }
            auto invalid = result;
            invalid["query_isolation"] = nullptr;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), invalid));
            invalid = result;
            invalid["history_scope"]["query_outputs_incomplete"] = false;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), invalid));
            rows->setCurrentItem(rows->topLevelItem(fragment));
            auto filter = window.findChild<QLineEdit *>("apiSearch");
            filter->setText("no matching execution");
            action(*view, "debugHistoryPixel")->trigger();
            auto debug = window.findChild<ReplayDebugView *>("replayDebug-ps");
            QVERIFY(debug);
            QCOMPARE(debug->request().at("execution"), Json("19408"));
            QCOMPARE(debug->request().at("primitive"), Json(0));
            QCOMPARE(debug->request().at("x"), Json(65));
            QCOMPARE(debug->request().at("y"), Json(172));
            QCOMPARE(debug->request().at("isolate_missing_queries"), Json(true));
            QVERIFY(filter->text().isEmpty()); // Locating the history execution restores visibility.
            const auto old = view->requestId();
            isolation->setChecked(false);
            QCOMPARE(rows->topLevelItemCount(), 0);
            QVERIFY(!view->finish(old, result));
            QVERIFY(!action(*view, "readPixelHistory")->isEnabled());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), result));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void isolatedQueryShaderWorkspace() {
        const auto captures=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output=qEnvironmentVariable("FLORA_QUERY_SHADER_UI");
        const auto library=qEnvironmentVariable("FLORA_QUERY_HISTORY_RENDERDOC");
        if (captures.isEmpty() || output.isEmpty() || library.isEmpty())
            QSKIP("Set Talos captures, a new shader output directory and the pinned RenderDoc library");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window,&MainWindow::captureLoaded),done(&window,&MainWindow::taskFinished);
        window.openCapture(captures + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate=window.findChild<QLineEdit *>("apiLocate"); locate->setText("19408"); QTest::keyClick(locate,Qt::Key_Return);
        auto view=window.findChild<ReplayDebugView *>("replayDebug-vs"); QVERIFY(view);
        QVERIFY(!action(*view,"replayDebugRead")->isEnabled());
        auto isolation=window.findChild<QCheckBox *>("dx12QueryIsolation"); isolation->setChecked(true);
        view->setBackendPath(library,false); view->selectVertex(0,0);
        QVERIFY(action(*view,"replayDebugRead")->isEnabled());
        QCOMPARE(view->request().at("isolate_missing_queries"),Json(true));
        done.clear(); action(*view,"replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),180000);
        QVERIFY2(done.takeLast()[0].toBool(),qPrintable(view->findChild<QLabel *>("replayDebugSummary")->text()));
        QVERIFY(!window.busy()); QVERIFY(view->model());
        const auto result=view->model()->result();
        QFile saved(output + "/result.json"); QVERIFY(saved.open(QIODevice::WriteOnly));
        saved.write(QByteArray::fromStdString(result.dump(2))); saved.close();
        const auto capture=QString::fromStdString(result.at("capture").get<std::string>());
        QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"),output + "/recapture.json"));
        QCOMPARE(result.at("execution"),Json("19408"));
        QCOMPARE(result.at("frame_complete"),Json(false));
        QCOMPARE(result.at("partial"),Json(true));
        QCOMPARE(result.at("debug_scope").at("query_outputs_incomplete"),Json(true));
        QCOMPARE(result.at("query_isolation").at("selected_execution"),Json("19408"));
        QCOMPARE(result.at("request_identity").at("missing_query_policy"),Json("isolate_readback_outputs"));
        QVERIFY(view->model()->size()>2); QVERIFY(view->model()->files().empty());
        QVERIFY(view->findChild<QLabel *>("replayDebugSummary")->text().contains("Incomplete"));
        action(*view,"debugStep")->trigger(); QCOMPARE(view->model()->position(),int64_t(1));
        const auto instruction=result.at("steps")[view->model()->size()/2].at("nextInstruction").get<uint64_t>();
        view->findChild<QLineEdit *>("debugInstruction")->setText(QString::number(instruction));
        action(*view,"debugToggleInstructionBreakpoint")->trigger();
        action(*view,"debugContinue")->trigger();
        QCOMPARE(result.at("steps")[size_t(view->model()->position())].at("nextInstruction"),Json(instruction));
        view->findChild<QSpinBox *>("debugStepIndex")->setValue(int(view->model()->size()-1));
        view->findChild<QLineEdit *>("debugWatchExpression")->setText("SV_Position.x");
        action(*view,"debugAddWatch")->trigger();
        QCOMPARE(view->model()->watchResults()[0].at("status"),Json("available"));
        QVERIFY(view->findChild<QLabel *>("replayDebugSummary")->text().contains("Incomplete"));
        view->exportResult(output + "/export.json"); QCOMPARE(read(output + "/export.json"),view->exportReport());
        for (const auto *key : {"frame_complete","partial"}) {
            auto invalid=result; invalid[key]=!result.at(key).get<bool>();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        }
        auto invalid=result; invalid["query_isolation"]=nullptr;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        invalid=result; invalid["debug_scope"]["query_outputs_incomplete"]=false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        const auto old=view->requestId(); isolation->setChecked(false);
        QVERIFY(!view->model()); QVERIFY(!view->finish(old,result));
        QVERIFY(!action(*view,"replayDebugRead")->isEnabled());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),result));
    }
    void isolatedQueryPipelineWorkspace() {
        const auto captures=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output=qEnvironmentVariable("FLORA_QUERY_PIPELINE_UI");
        const auto library=qEnvironmentVariable("FLORA_QUERY_HISTORY_RENDERDOC");
        if (captures.isEmpty() || output.isEmpty() || library.isEmpty())
            QSKIP("Set Talos captures, a new pipeline output directory and the pinned RenderDoc library");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window,&MainWindow::captureLoaded),done(&window,&MainWindow::taskFinished);
        window.openCapture(captures + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate=window.findChild<QLineEdit *>("apiLocate"); locate->setText("19408"); QTest::keyClick(locate,Qt::Key_Return);
        auto view=window.findChild<Dx12PipelineView *>(); QVERIFY(view);
        QVERIFY(!action(*view,"readDx12Pipeline")->isEnabled());
        auto isolation=window.findChild<QCheckBox *>("dx12QueryIsolation"); isolation->setChecked(true);
        view->setBackendPath(library,false);
        QVERIFY(action(*view,"readDx12Pipeline")->isEnabled());
        QCOMPARE(view->request().at("isolate_missing_queries"),Json(true));
        done.clear(); action(*view,"readDx12Pipeline")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),180000);
        QVERIFY2(done.takeLast()[0].toBool(),qPrintable(view->findChild<QLabel *>("dx12PipelineSummary")->text()));
        QVERIFY(!window.busy());
        const auto result=view->result();
        QCOMPARE(result.at("execution"),Json("19408"));
        QCOMPARE(result.at("captured_call").at("source_record"),Json("192508"));
        QCOMPARE(result.at("frame_complete"),Json(false));
        QCOMPARE(result.at("query_isolation").at("resolves").size(),size_t(1));
        QCOMPARE(result.at("pipeline_scope").at("query_outputs_incomplete"),Json(true));
        QVERIFY(view->findChild<QLabel *>("dx12PipelineSummary")->text().contains("Incomplete replay"));
        auto rows=view->findChild<QTreeWidget *>("dx12PipelineRows");
        bool target{},shader{};
        for (size_t i=0;i<result.at("pipeline_rows").size();++i) {
            const auto &row=result.at("pipeline_rows")[i];
            rows->setCurrentItem(rows->topLevelItem(int(i)));
            QCOMPARE(action(*view,"replaceBoundPipelineShader")->isEnabled(),
                     row.at("binding") == "Shader" && row.at("resource").value("mapping_status", "") == "capture_pipeline_stage");
            if (row.at("section") == "PS" && row.at("binding") == "Shader") {
                QCOMPARE(row.at("resource").at("mapping_status"),Json("capture_pipeline_stage"));
                QVERIFY(action(*view,"locatePipelineCreation")->isEnabled()); shader=true;
            }
            if (row.at("section") == "OM" && row.at("binding") == "RTV 0") {
                QCOMPARE(row.at("resource").at("resource_creation"),Json("35118"));
                QSignalSpy jump(view,&Dx12PipelineView::resourceRequested);
                action(*view,"inspectPipelineResource")->trigger();
                QCOMPARE(jump.count(),1); QCOMPARE(jump[0][0].toULongLong(),35118ULL); target=true;
            }
        }
        QVERIFY(target); QVERIFY(shader);
        view->exportResult(output + "/result.json"); QCOMPARE(read(output + "/result.json"),result);
        const auto capture=QString::fromStdString(result.at("capture").get<std::string>());
        QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"),output + "/recapture.json"));
        auto invalid=result; invalid["query_isolation"]=nullptr;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        invalid=result; invalid["frame_complete"]=true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        invalid=result; invalid["pipeline_scope"]["query_outputs_incomplete"]=false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        const auto old=view->requestId(); isolation->setChecked(false);
        QVERIFY(!view->finish(old,result));
        QVERIFY(!action(*view,"readDx12Pipeline")->isEnabled());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),result));
    }
    void frameEndResourceHistoryWorkspace() {
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_FRAME_RESOURCE_HISTORY_UI");
        const auto library = qEnvironmentVariable("FLORA_QUERY_HISTORY_RENDERDOC");
        if (captures.isEmpty() || output.isEmpty() || library.isEmpty())
            QSKIP("Set captures, a new frame-end resource history directory and the pinned RenderDoc library");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(captures + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        auto view = window.findChild<ResourceHistoryView *>();
        QCOMPARE(window.findChild<QTableView *>("resources")->currentIndex().data(Qt::UserRole).toULongLong(), 35118ULL);
        QVERIFY(!action(*view, "readResourceHistory")->isEnabled());
        window.findChild<QCheckBox *>("dx12QueryIsolation")->setChecked(true);
        view->setBackendPath(library, false);
        QCOMPARE(view->request().at("frame_end"), Json(true));
        QCOMPARE(view->request().at("frame_end_record"), Json("192535"));
        QVERIFY(action(*view, "readResourceHistory")->isEnabled());
        action(*view, "readResourceHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 180000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        const auto result = view->result();
        QCOMPARE(result.at("frame_end"), Json(true));
        QCOMPARE(result.at("frame_complete"), Json(false));
        QCOMPARE(result.at("history_scope").at("output_boundary"), Json("frame_end"));
        QCOMPARE(result.at("history_scope").at("through_record"), Json("192535"));
        QCOMPARE(result.at("history_scope").at("selected_event_replayed"), Json(false));
        QCOMPARE(result.at("frame_stream_boundary").at("boundary").at("last_record"), Json("192535"));
        const auto &uses = result.at("resource_history");
        QCOMPARE(uses.size(), size_t(6));
        QCOMPARE(uses.front().at("execution"), Json("19378"));
        QCOMPARE(uses.back().at("execution"), Json("19410"));
        QCOMPARE(uses.back().at("source_record"), Json("192530"));
        QCOMPARE(uses.back().at("usage_name"), Json("Barrier"));
        const auto &transition = uses.back().at("captured_resource_barriers").at(0);
        QCOMPARE(transition.at("Resource"), Json("1651480816"));
        QCOMPARE(transition.at("StateBefore"), Json(4));
        QCOMPARE(transition.at("StateAfter"), Json(0));
        QVERIFY(uses.back().at("eventId").get<uint32_t>() < result.at("frame_stream_boundary").at("event_id").get<uint32_t>());
        QVERIFY(!result.at("captured_creation").empty());
        QVERIFY(!result.at("replay_initialization").empty());
        const auto &context = result.at("queue_context");
        QCOMPARE(context.at("resource_dependency_proven"), Json(false));
        QCOMPARE(context.at("gpu_stall_duration_measured"), Json(false));
        QCOMPARE(context.at("through_record"), Json("192535"));
        std::set<std::string> records;
        for (const auto &row : context.at("operations")) {
            QCOMPARE(row.at("queue"), Json("175347920"));
            records.insert(row.at("record").get<std::string>());
        }
        QVERIFY(records.contains("192533")); QVERIFY(records.contains("192534"));
        auto sync = view->findChild<QTreeWidget *>("resourceHistorySynchronization");
        QCOMPARE(size_t(sync->topLevelItemCount()), context.at("operations").size());
        QVERIFY(view->findChild<QLabel *>("resourceHistorySummary")->text().contains("Frame stream end"));
        view->exportResult(output + "/result.json");
        const auto capture = QString::fromStdString(result.at("capture").get<std::string>());
        QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"), output + "/recapture.json"));
        auto wrong = result; wrong["frame_end"] = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), wrong));
        wrong = result; wrong["frame_stream_boundary"]["boundary"]["last_record"] = "192532";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), wrong));
        wrong = result; wrong["queue_context"]["resource_dependency_proven"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view->finish(view->requestId(), wrong));
        const auto old = view->requestId();
        QTreeWidgetItem *signalItem{};
        for (int i = 0; i < sync->topLevelItemCount(); ++i)
            if (sync->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() == 192534)
                signalItem = sync->topLevelItem(i);
        QVERIFY(signalItem);
        window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
        emit sync->itemDoubleClicked(signalItem, 0);
        QCOMPARE(window.findChild<QComboBox *>("apiEventDomain")->currentIndex(), 0);
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(), 192534ULL);
        QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
        QVERIFY(!view->finish(old, result));
        QVERIFY(view->result().is_null());
    }
    void isolatedQueryResourceHistoryWorkspace() {
        const auto captures=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output=qEnvironmentVariable("FLORA_QUERY_HISTORY_UI");
        const auto library=qEnvironmentVariable("FLORA_QUERY_HISTORY_RENDERDOC");
        if (captures.isEmpty() || output.isEmpty() || library.isEmpty())
            QSKIP("Set Talos captures, a new output directory and the pinned RenderDoc library");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window,&MainWindow::captureLoaded),done(&window,&MainWindow::taskFinished);
        window.openCapture(captures + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate=window.findChild<QLineEdit *>("apiLocate"); locate->setText("19410"); QTest::keyClick(locate,Qt::Key_Return);
        auto resources=window.findChild<QTableView *>("resources"); QModelIndex selected;
        for (int row=0;row<resources->model()->rowCount();++row)
            if (resources->model()->index(row,0).data(Qt::UserRole).toULongLong() == 35118)
                selected=resources->model()->index(row,0);
        QVERIFY(selected.isValid()); resources->setCurrentIndex(selected);
        auto isolation=window.findChild<QCheckBox *>("dx12QueryIsolation"); isolation->setChecked(true);
        auto view=window.findChild<ResourceHistoryView *>(); QVERIFY(view);
        view->setBackendPath(library,false);
        QVERIFY(action(*view,"readResourceHistory")->isEnabled());
        QCOMPARE(view->request().at("isolate_missing_queries"),Json(true));
        QVERIFY(action(*window.findChild<PixelHistoryView *>(),"readPixelHistory")->isEnabled());
        done.clear(); action(*view,"readResourceHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),180000);
        QVERIFY2(done.takeLast()[0].toBool(),qPrintable(view->findChild<QLabel *>("resourceHistorySummary")->text()));
        QVERIFY(!window.busy());
        const auto result=view->result();
        QCOMPARE(result.at("frame_complete"),Json(false));
        QCOMPARE(result.at("query_isolation").at("resolves").size(),size_t(2));
        QCOMPARE(result.at("history_scope").at("selected_event_replayed"),Json(false));
        QCOMPARE(result.at("history_scope").at("query_outputs_incomplete"),Json(true));
        QVERIFY(!result.at("resource_history").empty());
        QVERIFY(view->findChild<QLabel *>("resourceHistorySummary")->text().contains("Incomplete replay"));
        view->exportResult(output + "/result.json"); QCOMPARE(read(output + "/result.json"),result);
        auto invalid=result; invalid["query_isolation"]=nullptr;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        invalid=result; invalid["frame_complete"]=true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,view->finish(view->requestId(),invalid));
        const auto old=view->requestId(); isolation->setChecked(false);
        QVERIFY(!view->finish(old,result));
        QVERIFY(!action(*view,"readResourceHistory")->isEnabled());
    }
    void uavClearResourceHistory() {
        const auto path=qEnvironmentVariable("FLORA_UAV_CLEAR_HISTORY");
        if (path.isEmpty()) QSKIP("Set the BF1 shadow UAV resource history report");
        const auto report=read(path);
        ResourceHistoryView view;
        view.setContext("bf1:uav-clear",35772,300425,8697204528ULL);
        QVERIFY(view.finish(view.requestId(),report));
        auto rows=view.findChild<QTreeWidget *>("resourceHistoryRows");
        bool found{};
        for (size_t i=0; i<report.at("resource_history").size(); ++i) {
            const auto &row=report.at("resource_history")[i];
            if (row.value("execution",Json()) != "35703") continue;
            QCOMPARE(row.at("evidence"),Json("recorded_operation"));
            QCOMPARE(row.at("content_change"),Json("not_measured"));
            const auto &operation=row.at("operation_ranges");
            QCOMPARE(operation.at("status"),Json("captured"));
            const auto &range=operation.at("ranges")[0];
            QCOMPARE(range.at("kind"),Json("buffer_view"));
            QCOMPARE(range.at("first_element"),Json("0"));
            QCOMPARE(range.at("element_count"),Json("64000"));
            QCOMPARE(range.at("format"),Json(41));
            QCOMPARE(range.at("rectangles"),Json::array({{{"left",0},{"top",0},{"right",4700},{"bottom",1}}}));
            QVERIFY(rows->topLevelItem(int(i))->text(4).contains("UAV elements [0, 64000)"));
            QVERIFY(rows->topLevelItem(int(i))->text(4).contains("Clear rectangle"));
            rows->setCurrentItem(rows->topLevelItem(int(i)));
            QSignalSpy jump(&view,&ResourceHistoryView::eventRequested);
            action(view,"locateResourceHistoryEvent")->trigger();
            QCOMPARE(jump.count(),1); QCOMPARE(jump[0][0].toULongLong(),35703ULL);
            found=true;
        }
        QVERIFY(found);
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/uav-clear-resource-history.json");
            QCOMPARE(read(output_ + "/uav-clear-resource-history.json"),report);
        }
    }
    void resourceOperationRanges() {
        const auto root=qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the resource operation range evidence directory");
        ResourceHistoryView view;
        for (const auto &side : {QString("target"),QString("source"),QString("readback")}) {
            const auto report=read(root + "/half-blend-operation-ranges1/" + side + "/result.json");
            const uint64_t creation=side == "target" ? 1 : side == "source" ? 2 : 3;
            view.setContext("fixture:" + side,3,creation,1000+creation);
            QVERIFY(view.finish(view.requestId(),report));
            auto rows=view.findChild<QTreeWidget *>("resourceHistoryRows");
            size_t found{};
            for (size_t i=0;i<report.at("resource_history").size();++i) {
                const auto &row=report.at("resource_history")[i];
                QCOMPARE(row.at("content_change"),Json("not_measured"));
                if (side == "readback") {
                    const auto &range=row.at("operation_ranges").at("ranges")[0];
                    QCOMPARE(range.at("kind"),Json("buffer_footprint"));
                    QCOMPARE(range.at("RowPitch"),Json(256));
                    QVERIFY(rows->topLevelItem(int(i))->text(4).contains("footprint at byte")); ++found;
                } else if (row.value("execution",Json()) == "11") {
                    const auto &range=row.at("operation_ranges").at("ranges")[0];
                    QCOMPARE(range.at("role"),Json(side == "target" ? "destination" : "source"));
                    QCOMPARE(range.at("mip"),Json(side == "target" ? 1 : 0));
                    QCOMPARE(range.at("layer"),Json(side == "target" ? 1 : 0));
                    QVERIFY(rows->topLevelItem(int(i))->text(4).contains("Source box"));
                    rows->setCurrentItem(rows->topLevelItem(int(i)));
                    QSignalSpy jump(&view,&ResourceHistoryView::eventRequested);
                    action(view,"locateResourceHistoryEvent")->trigger();
                    QCOMPARE(jump.count(),1); QCOMPARE(jump[0][0].toULongLong(),11ULL); ++found;
                }
            }
            QCOMPARE(found,side == "readback" ? size_t(5) : size_t(1));
            if (side == "source") {
                QCOMPARE(report.at("history_scope").at("operation_range_gaps"),Json(1));
                QVERIFY(rows->topLevelItem(0)->text(4).contains("range unavailable"));
            }
        }
        const auto bf1=read(root + "/bf1-particle-operation-ranges1/result.json");
        view.setContext("bf1:operation-ranges",36560,325012,10432990048ULL);
        QVERIFY(view.finish(view.requestId(),bf1));
        QCOMPARE(bf1.at("history_scope").at("captured_operation_ranges"),Json(1));
        auto rows=view.findChild<QTreeWidget *>("resourceHistoryRows"); bool clear{};
        for (size_t i=0;i<bf1.at("resource_history").size();++i) {
            const auto &row=bf1.at("resource_history")[i];
            if (row.value("execution",Json()) != "36391") continue;
            QCOMPARE(row.at("operation_ranges").at("ranges")[0].at("mip"),Json(0));
            QVERIFY(rows->topLevelItem(int(i))->text(4).contains("whole view")); clear=true;
        }
        QVERIFY(clear);
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/resource-operation-ranges.json");
            QCOMPARE(read(output_ + "/resource-operation-ranges.json"),bf1);
        }
    }
    void uavClearPixelHistory() {
        const auto root=qEnvironmentVariable("FLORA_UAV_PIXEL_HISTORY");
        if (root.isEmpty()) QSKIP("Set the UAV Clear pixel-history evidence directory");
        const auto report=read(root + "/inside/result.json"), gpu=read(root + "/gpu.json");
        const auto outside=read(root + "/outside/result.json"), layer=read(root + "/other-layer/result.json");
        QCOMPARE(report.at("history").size(),size_t(12));
        QCOMPARE(outside.at("history").size(),size_t(9));
        QCOMPARE(outside.at("history_scope").at("excluded_resource_operations").size(),size_t(3));
        QVERIFY(layer.at("history").empty());
        QCOMPARE(layer.at("history_scope").at("excluded_resource_operations").size(),size_t(6));
        QVERIFY(report.at("history_scope").at("resource_operation_coverage_gaps").empty());
        QCOMPARE(report.at("history_scope").at("replay_consistency").at("history_boundary").at("status"),
            Json("matches_selected_texel"));
        size_t clears{};
        for (const auto &row : report.at("history")) {
            if (!row.contains("coverage") ||
                row.at("coverage").value("view_definition","") != "captured_cpu_descriptor") continue;
            QCOMPARE(row.at("execution"),Json(std::to_string(11+clears)));
            QCOMPARE(row.at("coverage").at("status"),Json("covered"));
            QCOMPARE(row.at("coverage").at("mip"),Json(1));
            QCOMPARE(row.at("coverage").at("first_layer"),Json(1));
            QVERIFY(row.at("primitiveID").is_null()); QVERIFY(row.at("shaderOut").is_null());
            QCOMPARE(row.at("preMod").at("col").at("floatValue"),
                Json::array({0.027862548828125,0.0273284912109375,0.024871826171875,0.3974609375}));
            for (size_t c=0;c<4;++c) {
                const auto bits=gpu.at("transfer_half_words")[clears][c].get<uint16_t>();
                QVERIFY(bits == 0 || bits == 0x3c00);
                QCOMPARE(row.at("postMod").at("col").at("floatValue")[c],Json(bits ? 1. : 0.));
            }
            ++clears;
        }
        QCOMPARE(clears,size_t(3));
        PixelHistoryView view; view.setContext("fixture:uav",3,true);
        view.setDx12Resource(1,1001); view.selectPixel(1001,5,5,1,1,0);
        QVERIFY(view.finish(view.requestId(),report));
        auto rows=view.findChild<QTreeWidget *>("historyRecords"); rows->setCurrentItem(rows->topLevelItem(1));
        QVERIFY(!action(view,"debugHistoryPixel")->isEnabled());
        QSignalSpy locate(&view,&PixelHistoryView::eventRequested);
        action(view,"locateHistoryEvent")->trigger();
        QCOMPARE(locate.count(),1); QCOMPARE(locate[0][0].toULongLong(),11ULL);
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/uav-clear-pixel-history.json");
            QCOMPARE(read(output_ + "/uav-clear-pixel-history.json"),report);
        }
    }
    void transferOperationHistory() {
        const auto root=qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the transfer history evidence directory");
        for (const auto &mode : {QString("copy"),QString("resolve")}) {
            const auto base=root + "/half-blend-" + mode + "1/";
            const auto report=read(base + (mode == "copy" ? "inside/result.json" : "patched/result.json"));
            const auto native=read(base + "gpu.json"), outside=read(base + "outside/result.json");
            QCOMPARE(report.at("history").size(),size_t(12));
            QCOMPARE(outside.at("history").size(),size_t(9));
            QCOMPARE(outside.at("history_scope").at("excluded_resource_operations").size(),size_t(3));
            size_t fragment{},operations{};
            for (const auto &row : report.at("history")) {
                if (row.value("operation","") == mode.toStdString()) {
                    QCOMPARE(row.at("coverage").at("status"),Json("covered"));
                    QCOMPARE(row.at("coverage").at("destination_rectangle"),Json::array({4,4,8,8}));
                    QCOMPARE(row.at("postMod").at("col").at("floatValue"),Json::array({1.,0.,0.,1.}));
                    QVERIFY(row.at("primitiveID").is_null()); QVERIFY(row.at("shaderOut").is_null()); ++operations;
                }
                if (row.value("execution",Json()) != "3") continue;
                QVERIFY(fragment < 3);
                for (size_t c=0;c<4;++c) {
                    const auto bits=native.at("prefix_half_words")[fragment][c].get<uint16_t>();
                    QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),
                        std::ldexp(1.0 + double(bits & 1023) / 1024.0,int(bits >> 10)-15));
                }
                ++fragment;
            }
            QCOMPARE(fragment,size_t(3)); QCOMPARE(operations,size_t(3));
        }
        const auto wrongLayer=read(root + "/half-blend-copy1/other-layer/result.json");
        QVERIFY(wrongLayer.at("history").empty());
        QCOMPARE(wrongLayer.at("history_scope").at("excluded_resource_operations").size(),size_t(6));
        const auto old=read(root + "/half-blend-resolve1/inside/result.json");
        QCOMPARE(old.at("history")[1].at("postMod").at("col").at("uintValue")[0],Json(0xdeadbeefU));
        const auto report=read(root + "/half-blend-resolve2/transfer/result.json");
        const auto native=read(root + "/half-blend-resolve2/gpu.json");
        QCOMPARE(report.at("backend_dx12_resolve_history_patch"),Json(1));
        size_t transfers{};
        for (const auto &row : report.at("history")) {
            if (row.value("operation","") != "resolve") continue;
            const auto &words=native.at("transfer_half_words")[transfers++];
            for (size_t c=0;c<4;++c) {
                const auto bits=words[c].get<uint16_t>();
                QVERIFY(bits == 0 || bits == 0x3c00);
                QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),bits ? 1. : 0.);
            }
        }
        QCOMPARE(transfers,size_t(3));
        QCOMPARE(report.at("history_scope").at("replay_consistency").at("history_boundary").at("status"),Json("matches_selected_texel"));
        PixelHistoryView view; view.setContext("fixture:resolve",13,true); view.selectPixel(0,5,5,1,1,0);
        QVERIFY(view.finish(view.requestId(),report));
        auto rows=view.findChild<QTreeWidget *>("historyRecords"); rows->setCurrentItem(rows->topLevelItem(1));
        QVERIFY(!action(view,"debugHistoryPixel")->isEnabled());
        QSignalSpy locate(&view,&PixelHistoryView::eventRequested); action(view,"locateHistoryEvent")->trigger();
        QCOMPARE(locate.count(),1); QCOMPARE(locate[0][0].toULongLong(),11ULL);
        const auto guarded=read(root + "/half-blend-resolve1/unpatched-guard/result.json");
        QVERIFY(guarded.at("partial").get<bool>());
        QCOMPARE(guarded.at("history_scope").at("resource_operation_value_gaps").size(),size_t(3));
        QCOMPARE(guarded.at("history_scope").at("replay_consistency").at("history_boundary").at("status"),Json("not_compared"));
        QVERIFY(view.finish(view.requestId(),guarded));
        QVERIFY(rows->topLevelItem(1)->text(4).contains("Value unavailable"));
        QVERIFY(rows->topLevelItem(1)->text(3).contains("values unavailable"));
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/resolve-unavailable.json");
            QCOMPARE(read(output_ + "/resolve-unavailable.json"),guarded);
        }
    }
    void clearOperationCoverage() {
        const auto root = qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the Clear coverage evidence directory");
        const auto outside = read(root + "/half-blend-array1/clear-excluded1/result.json");
        QVERIFY(outside.at("history").empty());
        QCOMPARE(outside.at("history_scope").at("excluded_resource_operations").size(), size_t(3));
        const auto rect = read(root + "/half-blend-rect1/outside/result.json");
        const auto inside = read(root + "/half-blend-rect1/inside/result.json");
        QCOMPARE(rect.at("history").size(), size_t(9));
        QCOMPARE(inside.at("history").size(), size_t(12));
        for (const auto &excluded : rect.at("history_scope").at("excluded_resource_operations")) {
            QCOMPARE(excluded.at("coverage").at("rectangles"), Json::array({Json::array({0,0,4,4})}));
            QCOMPARE(excluded.at("coverage").at("view_covers_selection"), Json(true));
            QCOMPARE(excluded.at("coverage").at("rectangles_cover_pixel"), Json(false));
        }
        QCOMPARE(rect.at("history_scope").at("excluded_resource_operations").size(), size_t(3));
        for (const auto &row : inside.at("history")) {
            if (row.value("record_kind", "") != "resource_operation") continue;
            QCOMPARE(row.at("coverage").at("status"), Json("covered"));
            QVERIFY(row.at("fragIndex").is_null()); QVERIFY(row.at("primitiveID").is_null());
            QVERIFY(row.at("shaderOut").is_null());
        }
        PixelHistoryView view;
        view.setContext("fixture:clear", 3, true); view.selectPixel(0, 5, 5, 1, 0, 0);
        QVERIFY(view.finish(view.requestId(), outside));
        QCOMPARE(view.findChild<QTreeWidget *>("historyRecords")->topLevelItemCount(), 0);
        QVERIFY(view.findChild<QLabel *>("historySummary")->text().contains("3 resource operations excluded"));
        const auto bf1 = read(root + "/bf1-particle-clear-history1/result.json");
        const auto previous = read(root + "/bf1-particle-half-history1/result.json");
        QCOMPARE(bf1.at("history").size(), size_t(23));
        for (size_t i=1; i<23; ++i) {
            const auto &row=bf1.at("history")[i], &old=previous.at("history")[i];
            QCOMPARE(row.at("primitiveID"),old.at("primitiveID"));
            QCOMPARE(row.at("shaderOut"),old.at("shaderOut"));
            QCOMPARE(row.at("postMod"),old.at("postMod"));
        }
        view.setContext("bf1:clear", 36560, true); view.setDx12Resource(325012, 10432990048ULL);
        view.selectPixel(10432990048ULL, 836, 241, 0, 0, 0);
        view.findChild<QCheckBox *>("historyDepthRoundtrip")->setChecked(true);
        QVERIFY(view.finish(view.requestId(), bf1));
        auto rows=view.findChild<QTreeWidget *>("historyRecords");
        rows->setCurrentItem(rows->topLevelItem(0));
        QVERIFY(rows->topLevelItem(0)->text(3).contains("Resource operation"));
        QVERIFY(!action(view,"debugHistoryPixel")->isEnabled());
        QVERIFY(action(view,"locateHistoryEvent")->isEnabled());
        QSignalSpy locate(&view,&PixelHistoryView::eventRequested);
        action(view,"locateHistoryEvent")->trigger();
        QCOMPARE(locate.count(),1); QCOMPARE(locate[0][0].toULongLong(),36391ULL);
        rows->setCurrentItem(rows->topLevelItem(22));
        QVERIFY(action(view,"debugHistoryPixel")->isEnabled());
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/clear-history.json");
            QCOMPARE(read(output_ + "/clear-history.json"), bf1);
        }
    }
    void halfColorHistorySubresources() {
        const auto root = qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the half-color mip/array history evidence directory");
        for (const auto &name : {QString("half-blend-mip1"), QString("half-blend-array1")}) {
            const auto native = read(root + '/' + name + "/gpu.json");
            const auto report = read(root + '/' + name + "/final/result.json");
            QCOMPARE(report.at("backend_dx12_half_color_history_patch"), Json(2));
            const auto &scope = report.at("history_scope").at("replay_consistency");
            QCOMPARE(scope.at("mip"), native.at("mip")); QCOMPARE(scope.at("layer"), native.at("layer"));
            QCOMPARE(scope.at("history_boundary").at("status"), Json("matches_selected_texel"));
            size_t fragment{};
            for (const auto &row : report.at("history")) {
                if (row.value("execution", Json()) != "3") continue;
                QVERIFY(fragment < 3); QCOMPARE(row.at("primitiveID").get<size_t>(), fragment);
                for (size_t c=0; c<4; ++c) {
                    const auto bits = native.at("prefix_half_words")[fragment][c].get<uint16_t>();
                    QVERIFY(bits > 0x400 && bits < 0x7c00);
                    QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(),
                        std::ldexp(1.0 + double(bits & 1023) / 1024.0, int(bits >> 10) - 15));
                }
                ++fragment;
            }
            QCOMPARE(fragment, size_t(3));
        }
        const auto missing = read(root + "/half-blend-mip1/before/result.json");
        QCOMPARE(std::count_if(missing.at("history").begin(), missing.at("history").end(),
            [](const Json &row) { return row.value("execution", Json()) == "3"; }), 1);
        const auto wrongLayer = read(root + "/half-blend-array1/other-layer-before/result.json");
        QCOMPARE(std::count_if(wrongLayer.at("history").begin(), wrongLayer.at("history").end(),
            [](const Json &row) { return row.value("execution", Json()).is_string(); }), 6);
        for (const auto &name : {QString("other-layer-final"), QString("other-mip-final")}) {
            const auto report = read(root + "/half-blend-array1/" + name + "/result.json");
            QVERIFY(report.at("history_scope").contains("subresource_filter"));
            for (const auto &row : report.at("history"))
                QVERIFY(!row.value("execution", Json()).is_string());
        }
    }
    void halfColorFragmentHistory() {
        const auto root = qEnvironmentVariable("FLORA_HALF_COLOR_HISTORY");
        if (root.isEmpty()) QSKIP("Set the half-color fixture and BF1 history evidence directory");
        const auto native = read(root + "/half-blend-history1/gpu.json");
        const auto fixture = read(root + "/half-blend-history1/after/result.json");
        const auto oldFixture = read(root + "/half-blend-history1/before/result.json");
        size_t fragment{}, oldDifferences{};
        for (size_t i = 0; i < fixture.at("history").size(); ++i) {
            const auto &row = fixture.at("history")[i];
            if (row.value("execution", Json()) != "3") continue;
            QVERIFY(fragment < 3);
            QCOMPARE(row.at("primitiveID").get<size_t>(), fragment);
            for (size_t c = 0; c < 4; ++c) {
                const auto bits = native.at("prefix_half_words")[fragment][c].get<uint16_t>();
                // The oracle is native stored half words; these fixture values are positive normal numbers.
                QVERIFY(bits > 0x400 && bits < 0x7c00);
                const auto expected = std::ldexp(1.0 + double(bits & 1023) / 1024.0, int(bits >> 10) - 15);
                QCOMPARE(row.at("postMod").at("col").at("floatValue")[c].get<double>(), expected);
                oldDifferences += oldFixture.at("history")[i].at("postMod").at("col").at("floatValue")[c].get<double>() != expected;
            }
            ++fragment;
        }
        QCOMPARE(fragment, size_t(3)); QCOMPARE(oldDifferences, size_t(4));
        const auto report = read(root + "/bf1-particle-half-history1/result.json");
        const auto old = read(root + "/bf1-particle-roundtrip-history1/result.json");
        QCOMPARE(report.at("backend_dx12_half_color_history_patch"), Json(1));
        QCOMPARE(report.at("history").size(), size_t(23));
        for (size_t i=0; i<23; ++i) {
            QCOMPARE(report.at("history")[i].at("shaderOut"), old.at("history")[i].at("shaderOut"));
            QCOMPARE(report.at("history")[i].at("primitiveID"), old.at("history")[i].at("primitiveID"));
        }
        const auto &last = report.at("history").back();
        QCOMPARE(last.at("primitiveID"), Json(1334));
        QCOMPARE(last.at("preMod").at("col"), last.at("postMod").at("col"));
        QVERIFY(old.at("history").back().at("preMod").at("col") != last.at("preMod").at("col"));
        QCOMPARE(report.at("history_scope").at("replay_consistency").at("history_boundary").at("status"), Json("matches_selected_texel"));
        PixelHistoryView view; view.resize(1650, 900); view.show();
        view.setContext("bf1:half-color", 36560, true); view.setDx12Resource(325012, 10432990048ULL);
        view.selectPixel(10432990048ULL, 836, 241, 0, 0, 0);
        view.findChild<QCheckBox *>("historyDepthRoundtrip")->setChecked(true);
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("historyRecords"); QCOMPARE(rows->topLevelItemCount(), 23);
        rows->setCurrentItem(rows->topLevelItem(22));
        QCOMPARE(rows->topLevelItem(22)->text(4), rows->topLevelItem(22)->text(5));
        QSignalSpy debug(&view, &PixelHistoryView::pixelDebugRequested);
        action(view, "debugHistoryPixel")->trigger(); QCOMPARE(debug.count(), 1);
        QCOMPARE(debug[0][0].toULongLong(), 36560ULL); QCOMPARE(debug[0][4].toUInt(), 1334U);
        QVERIFY(debug[0][6].toBool());
        QCoreApplication::processEvents();
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/half-color-history.json");
            QCOMPARE(read(output_ + "/half-color-history.json"), report);
            QVERIFY(view.grab().save(output_ + "/half-color-history.png"));
        }
    }
    void talosCubeCoordinates() {
        const auto root = qEnvironmentVariable("FLORA_TALOS_CUBE_DEBUG");
        if (root.isEmpty()) QSKIP("Set the Talos cube debug evidence directory");
        const auto report = read(root + "/patched/result.json");
        const auto baseline = read(root + "/../talos-depth-state1/debug-roundtrip/result.json");
        const auto history = read(root + "/../talos-depth-state1/history-roundtrip/result.json");
        QCOMPARE(report.at("backend_dx12_cube_coordinates_patch"), Json(1));
        QCOMPARE(report.at("trace"), baseline.at("trace"));
        QCOMPARE(report.at("steps").size(), size_t(278));
        for (size_t i = 0; i < 240; ++i) QCOMPARE(report.at("steps")[i], baseline.at("steps")[i]);
        QVERIFY(report.at("steps")[240] != baseline.at("steps")[240]);
        const auto &gpu = report.at("gpu_reference");
        QCOMPARE(gpu.at("value"), gpu.at("after_trace_creation_value"));
        QCOMPARE(gpu.at("value"), gpu.at("after_debug_value"));
        QCOMPARE(gpu.at("value"), baseline.at("gpu_reference").at("value"));
        ReplayDebugView view("ps"); view.resize(1500, 950); view.show();
        view.setContext("talos:cube-fixed", 14835, true); view.selectPixel(1776, 380, 0);
        view.selectPrimitive(172); view.selectDepthRoundtrip(true);
        QVERIFY(view.finish(view.requestId(), report));
        view.findChild<QLineEdit *>("debugInstruction")->setText("239");
        action(view, "debugToggleInstructionBreakpoint")->trigger(); action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(239));
        auto expression = view.findChild<QLineEdit *>("debugWatchExpression");
        expression->setText("registers.r2.float"); action(view, "debugAddWatch")->trigger();
        QCOMPARE(view.model()->watchResults()[0].at("values")[2], Json(1.0));
        action(view, "debugStep")->trigger();
        QCOMPARE(view.model()->position(), int64_t(240));
        QCOMPARE(view.model()->watchResults()[0].at("values"),
            Json::array({0.5990996956825256, 0.721889078617096, 0.9115129113197327, 0.531212329864502}));
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(277);
        expression->setText("registers.o0.float"); action(view, "debugAddWatch")->trigger();
        const auto output = view.model()->watchResults()[1].at("values");
        const auto observed = history.at("history").back().at("shaderOut").at("col").at("floatValue");
        const uint32_t nativeHalf[]{7043, 7423, 8142, 15360};
        for (size_t i = 0; i < 4; ++i) {
            const auto bits = std::bit_cast<uint32_t>(output[i].get<float>());
            const auto expected = std::bit_cast<uint32_t>(observed[i].get<float>());
            QVERIFY(std::abs(int64_t(bits) - int64_t(expected)) <= 1);
            const auto exponent = int((bits >> 23) & 255) - 127 + 15;
            QVERIFY((bits & 0x80000000) == 0 && exponent > 0 && exponent < 31);
            // Observed positive-normal binary16 truncation, not a general rounding guarantee.
            QCOMPARE((uint32_t(exponent) << 10) | ((bits >> 13) & 1023), nativeHalf[i]);
        }
        auto tabs = view.findChild<QTabWidget *>("debugValueTabs");
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Watches") tabs->setCurrentIndex(i);
        QCoreApplication::processEvents();
        QCOMPARE(view.exportReport().at("backend_dx12_cube_coordinates_patch"), Json(1));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/talos-cube-fixed.png"));
    }
    void talosReadOnlyDepthRoundtrip() {
        const auto root = qEnvironmentVariable("FLORA_TALOS_DEPTH_STATE");
        if (root.isEmpty()) QSKIP("Set the Talos D32 depth evidence directory");
        const auto report = read(root + "/history-roundtrip/result.json");
        const auto baseline = read(root + "/history-default/result.json");
        QCOMPARE(report.at("backend_depth_roundtrip_patch"), Json(2));
        QCOMPARE(report.at("history"), baseline.at("history"));
        QCOMPARE(report.at("history").size(), size_t(2));
        const auto &audit = report.at("history_scope").at("replay_consistency");
        QCOMPARE(audit.at("history_boundary").at("status"), Json("matches_selected_texel"));
        QCOMPARE(baseline.at("history_scope").at("replay_consistency").at("history_boundary").at("status"),
                 Json("differs_from_selected_texel"));
        QCOMPARE(audit.at("before_history"), audit.at("after_restoration"));
        QCOMPARE(audit.at("before_history"), report.at("history").back().at("postMod").at("col"));
        PixelHistoryView view; view.resize(1600, 800); view.show();
        view.setContext("talos:d32", 14835, true); view.setDx12Resource(193, 1657122592ULL);
        view.selectPixel(1657122592ULL, 1776, 380, 0, 0, 0);
        auto refresh = view.findChild<QCheckBox *>("historyDepthRoundtrip");
        QVERIFY(!refresh->isChecked()); refresh->setChecked(true);
        QVERIFY(view.finish(view.requestId(), report));
        QVERIFY(!view.findChild<QLabel *>("historySummary")->text().contains("differs from the selected texel"));
        auto rows = view.findChild<QTreeWidget *>("historyRecords");
        rows->setCurrentItem(rows->topLevelItem(1));
        QSignalSpy debug(&view, &PixelHistoryView::pixelDebugRequested);
        action(view, "debugHistoryPixel")->trigger();
        QCOMPARE(debug.size(), 1); QCOMPARE(debug[0][0].toULongLong(), 14835ULL);
        QCOMPARE(debug[0][4].toUInt(), 172U); QCOMPARE(debug[0][6].toBool(), true);
        QCoreApplication::processEvents();
        if (!output_.isEmpty()) {
            QVERIFY(view.grab().save(output_ + "/talos-d32-history.png"));
            view.exportResult(output_ + "/talos-d32-history.json");
            QCOMPARE(read(output_ + "/talos-d32-history.json"), report);
        }
        const auto trace = read(root + "/debug-roundtrip/result.json");
        ReplayDebugView pixel("ps"); pixel.resize(1500, 950); pixel.show();
        pixel.setContext("talos:d32", 14835, true); pixel.selectPixel(1776, 380, 0); pixel.selectPrimitive(172);
        pixel.selectDepthRoundtrip(true); QVERIFY(pixel.finish(pixel.requestId(), trace));
        QCOMPARE(trace.at("step_count"), Json(278));
        QCOMPARE(trace.at("gpu_reference").at("consistency"), Json("selected_texel_unchanged"));
        pixel.findChild<QSpinBox *>("debugStepIndex")->setValue(277);
        pixel.findChild<QLineEdit *>("debugWatchExpression")->setText("registers.o0.float");
        action(pixel, "debugAddWatch")->trigger();
        const auto output = pixel.model()->watchResults()[0];
        QCOMPARE(output.at("status"), Json("available"));
        // A corrected target observation does not validate this simulated PS invocation.
        QVERIFY(std::abs(output.at("values")[2].get<double>() -
            trace.at("gpu_reference").at("value").at("floatValue")[2].get<double>()) > 0.0008);
        auto tabs = pixel.findChild<QTabWidget *>("debugValueTabs");
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Watches") tabs->setCurrentIndex(i);
        QCoreApplication::processEvents();
        if (!output_.isEmpty()) QVERIFY(pixel.grab().save(output_ + "/talos-d32-debug.png"));
    }
    void depthRoundtripHistory() {
        const auto root = qEnvironmentVariable("FLORA_DEPTH_ROUNDTRIP_HISTORY_REPORTS");
        if (root.isEmpty()) QSKIP("Set the selected D32S8 history evidence directory");
        const auto report = read(root + "/hellblade-depth-roundtrip-history1/result.json");
        const auto baseline = read(root + "/hellblade-indirect36222-history2/result.json");
        QCOMPARE(report.at("history"), baseline.at("history"));
        QCOMPARE(report.at("history").size(), size_t(5));
        QCOMPARE(report.at("depth_roundtrip").at("copies"), Json("3"));
        const auto &audit = report.at("history_scope").at("replay_consistency");
        QCOMPARE(audit.at("before_history"), audit.at("after_restoration"));
        QCOMPARE(audit.at("history_boundary").at("status"), Json("matches_selected_texel"));
        QCOMPARE(audit.at("before_history"), report.at("history").back().at("postMod").at("col"));
        QVERIFY(report.at("gpa_modules_loaded").empty());
        PixelHistoryView view; view.resize(1600, 760); view.show();
        view.setContext("hellblade:depth-history", 36222, true);
        view.setDx12Resource(511789, 2152463529088ULL);
        view.selectPixel(2152463529088ULL, 456, 206, 0, 0, 0);
        auto refresh = view.findChild<QCheckBox *>("historyDepthRoundtrip");
        QVERIFY(refresh); QVERIFY(!refresh->isChecked());
        const auto old = view.requestId(); refresh->setChecked(true);
        QVERIFY(!view.finish(old, baseline));
        QCOMPARE(view.request().at("depth_roundtrip"), Json(true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), baseline));
        view.setWorkerBusy(true); QVERIFY(!refresh->isEnabled());
        view.setWorkerBusy(false); QVERIFY(refresh->isEnabled());
        QVERIFY(view.finish(view.requestId(), report));
        const auto summary = view.findChild<QLabel *>("historySummary");
        QVERIFY(summary->text().contains("3 completed depth refreshes"));
        QVERIFY(!summary->text().contains("differs from the selected texel"));
        QVERIFY(summary->text().contains("intermediate fragments are not validated"));
        auto rows = view.findChild<QTreeWidget *>("historyRecords");
        QCOMPARE(rows->topLevelItemCount(), 5);
        rows->setCurrentItem(rows->topLevelItem(4));
        QSignalSpy locate(&view, &PixelHistoryView::eventRequested), debug(&view, &PixelHistoryView::pixelDebugRequested);
        action(view, "locateHistoryEvent")->trigger(); action(view, "debugHistoryPixel")->trigger();
        QCOMPARE(locate.size(), 1); QCOMPARE(locate[0][0].toULongLong(), 36222ULL);
        QCOMPARE(locate[0][1].toLongLong(), 0LL);
        QCOMPARE(debug.size(), 1); QCOMPARE(debug[0][4].toUInt(), 0U);
        QCOMPARE(debug[0][5].toLongLong(), 0LL); QCOMPARE(debug[0][6].toBool(), true);
        rows->setCurrentItem(rows->topLevelItem(1));
        action(view, "debugHistoryPixel")->trigger();
        QCOMPARE(debug.size(), 2); QCOMPARE(debug[1][0].toULongLong(), 12574ULL);
        QCOMPARE(debug[1][6].toBool(), false);
        rows->setCurrentItem(rows->topLevelItem(4));
        QCoreApplication::processEvents();
        if (!output_.isEmpty()) {
            QVERIFY(view.grab().save(output_ + "/depth-roundtrip-history.png"));
            view.exportResult(output_ + "/depth-roundtrip-history.json");
            QCOMPARE(read(output_ + "/depth-roundtrip-history.json"), report);
        }
        const auto stale = view.requestId(); view.setContext("next-experiment", 36222, true);
        QVERIFY(!refresh->isChecked()); QVERIFY(!view.finish(stale, report));
        refresh->setChecked(true); view.setContext("next-experiment", 36223, true);
        QVERIFY(!refresh->isChecked());
        view.setContext("dx11", 1, false); QVERIFY(refresh->isHidden());
    }
    void depthRoundtripHistoryDebugHandoff() {
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (captures.isEmpty()) QSKIP("Set the lightweight DX12 capture directory for workspace navigation");
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(captures + "/bf1_2025_12_10__14_29_55.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("36560"); QTest::keyClick(locate, Qt::Key_Return);
        done.clear();
        auto history = window.findChild<PixelHistoryView *>();
        ReplayDebugView *pixel{};
        for (auto view : window.findChildren<ReplayDebugView *>())
            if (view->findChild<QCheckBox *>("debugDepthRoundtrip")) pixel = view;
        QVERIFY(history); QVERIFY(pixel);
        // Navigation transport only; BF1's D32 attachment is not a D32S8 GPU validation.
        emit history->pixelDebugRequested(36560, 836, 241, 0, 1322, -1, true);
        QCOMPARE(pixel->request().at("depth_roundtrip"), Json(true));
        QCOMPARE(pixel->request().at("primitive"), Json(1322));
        QCOMPARE(window.findChild<QTabWidget *>("checkpointStageTabs")->currentWidget(), pixel);
        emit history->pixelDebugRequested(36560, 836, 241, 0, 1322);
        QVERIFY(!pixel->request().contains("depth_roundtrip"));
        QVERIFY(done.empty()); QVERIFY(!window.busy());
    }
    void dxilInstructionFlags() {
        const auto path = qEnvironmentVariable("FLORA_DXIL_FLAGS_REPORT");
        if (path.isEmpty()) QSKIP("Set the actual DXIL instruction-flags debug report");
        const auto report = read(path);
        const auto &flags = report.at("debug_scope").at("dxil_instruction_flags");
        QCOMPARE(flags.at("status"), Json("included"));
        QCOMPARE(flags.at("patch_version"), Json(1));
        QCOMPARE(flags.at("annotated_lines"), Json(53));
        QCOMPARE(flags.at("driver_instructions_observed"), Json(false));
        ReplayDebugView view("ps"); view.resize(1500, 950); view.show();
        view.setContext("hellblade:instruction-flags", 36222, true);
        view.selectPixel(456, 206, 0); view.selectDepthRoundtrip(true);
        QVERIFY(view.finish(view.requestId(), report));
        auto notice = view.findChild<QLabel *>("debugBackendNotice");
        QVERIFY(notice->text().contains("allowed compiler optimizations"));
        auto assembly = view.findChild<QPlainTextEdit *>("debugAssembly");
        QVERIFY(assembly);
        // Breakpoints still follow the original trace line mapping. Both
        // divisions and their subtraction retain their captured fast flag.
        for (const auto instruction : {99, 100, 103}) {
            view.findChild<QLineEdit *>("debugInstruction")->setText(QString::number(instruction));
            action(view, "debugToggleInstructionBreakpoint")->trigger();
            action(view, "debugContinue")->trigger();
            const auto &step = report.at("steps").at(size_t(view.model()->position()));
            QCOMPARE(step.at("nextInstruction"), Json(instruction));
            const auto info = view.model()->instructionInfo(instruction);
            QCOMPARE(assembly->textCursor().blockNumber() + 1,
                     info.at("lineInfo").at("disassemblyLine").get<int>());
            QVERIFY(assembly->textCursor().block().text().contains("// fast-math: fast"));
        }
        if (!output_.isEmpty()) {
            QCoreApplication::processEvents();
            assembly->centerCursor();
            QVERIFY(assembly->viewport()->rect().intersects(assembly->cursorRect()));
            QVERIFY(view.grab().save(output_ + "/dxil-instruction-flags.png"));
            view.exportResult(output_ + "/dxil-instruction-flags.json");
            const auto exported = read(output_ + "/dxil-instruction-flags.json");
            QCOMPARE(exported.at("disassembly"), report.at("disassembly"));
            QCOMPARE(exported.at("debug_scope").at("dxil_instruction_flags"), flags);
        }
        // Legacy reports remain loadable without claiming annotation support.
        auto legacy = report; legacy["debug_scope"].erase("dxil_instruction_flags");
        QVERIFY(view.finish(view.requestId(), legacy));
        QVERIFY(!notice->text().contains("allowed compiler optimizations"));
    }
    void depthRoundtripPixelDebug() {
        const auto root = qEnvironmentVariable("FLORA_DEPTH_ROUNDTRIP_DEBUG_REPORTS");
        if (root.isEmpty()) QSKIP("Set the selected D32S8 pixel debug evidence directory");
        const auto report = read(root + "/enabled/result.json");
        const auto baseline = read(root + "/disabled/result.json");
        QCOMPARE(report.at("depth_roundtrip").at("copies"), Json("3"));
        QCOMPARE(report.at("steps").size(), size_t(131));
        const auto &gpu = report.at("gpu_reference");
        QCOMPARE(gpu.at("value"), gpu.at("after_trace_creation_value"));
        QCOMPARE(gpu.at("value"), gpu.at("after_debug_value"));
        QCOMPARE(gpu.at("consistency"), Json("selected_texel_unchanged"));
        QCOMPARE(baseline.at("gpu_reference").at("value").at("uintValue"), Json::array({0,0,0,0}));
        QVERIFY(gpu.at("value") != baseline.at("gpu_reference").at("value"));
        QVERIFY(report.at("gpa_modules_loaded").empty());
        ReplayDebugView view("ps"); view.resize(1500, 950); view.show();
        view.setContext("hellblade:roundtrip", 36222, true);
        view.selectPixel(456, 206, 0);
        auto refresh = view.findChild<QCheckBox *>("debugDepthRoundtrip");
        auto snapshot = view.findChild<QCheckBox *>("debugReadOnlyDepthSnapshot");
        QVERIFY(refresh); QVERIFY(snapshot); QVERIFY(!refresh->isChecked());
        const auto old = view.requestId();
        refresh->setChecked(true);
        QVERIFY(!view.finish(old, baseline));
        QCOMPARE(view.request().at("depth_roundtrip"), Json(true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), baseline));
        snapshot->setChecked(true); QVERIFY(!refresh->isChecked());
        refresh->setChecked(true); QVERIFY(!snapshot->isChecked());
        view.setWorkerBusy(true); QVERIFY(!refresh->isEnabled());
        view.setWorkerBusy(false); QVERIFY(refresh->isEnabled());
        QVERIFY(view.finish(view.requestId(), report));
        QVERIFY(view.findChild<QLabel *>("debugBackendNotice")->text().contains("3 completed depth refreshes"));
        QSignalSpy errors(&view, &ReplayDebugView::error);
        view.findChild<QLineEdit *>("debugInstruction")->setText("137");
        action(view, "debugToggleInstructionBreakpoint")->trigger();
        action(view, "debugContinue")->trigger();
        QCOMPARE(view.model()->position(), int64_t(125));
        view.findChild<QLineEdit *>("debugWatchExpression")->setText("_175");
        action(view, "debugAddWatch")->trigger();
        const auto watch = view.model()->watchResults().at(0);
        QCOMPARE(watch.at("status"), Json("available"));
        QCOMPARE(watch.at("values").at(0), Json(0.14453497529029846));
        // This unresolved difference is retained, not accepted as shader equivalence.
        QVERIFY(std::abs(watch.at("values").at(0).get<double>() -
                         gpu.at("value").at("floatValue").at(3).get<double>()) > 0.001);
        auto tabs = view.findChild<QTabWidget *>("debugValueTabs");
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Watches") tabs->setCurrentIndex(i);
        QCoreApplication::processEvents();
        QVERIFY(refresh->isVisible()); QVERIFY(snapshot->isVisible());
        if (!output_.isEmpty()) {
            QVERIFY(view.grab().save(output_ + "/depth-roundtrip-pixel-debug.png"));
            view.exportResult(output_ + "/depth-roundtrip-pixel-debug.json");
            QCOMPARE(read(output_ + "/depth-roundtrip-pixel-debug.json").at("depth_roundtrip"), report.at("depth_roundtrip"));
        }
        QVERIFY(errors.empty());
        const auto stale = view.requestId();
        view.setContext("next-experiment", 36222, true);
        QVERIFY(!refresh->isChecked()); QVERIFY(!view.finish(stale, report));
        view.setContext("dx11", 1, false); QVERIFY(refresh->isHidden());
    }
    void indirectHistoryBoundaryMismatch() {
        const auto root = qEnvironmentVariable("FLORA_HISTORY_CONSISTENCY_REPORTS");
        if (root.isEmpty()) QSKIP("Set the Hellblade indirect boundary evidence directory");
        const auto report = read(root + "/hellblade-indirect36222-history2/result.json");
        const auto &audit = report.at("history_scope").at("replay_consistency");
        QCOMPARE(audit.at("status"), Json("selected_texel_unchanged"));
        QCOMPARE(audit.at("before_history"), audit.at("after_restoration"));
        QCOMPARE(audit.at("history_boundary").at("status"), Json("differs_from_selected_texel"));
        QCOMPARE(audit.at("history_boundary").at("post_modification"), report.at("history").back().at("postMod").at("col"));
        QVERIFY(report.at("partial").get<bool>());
        PixelHistoryView view; view.setContext("indirect-boundary", 36222, true);
        view.setDx12Resource(511789, 2152463529088ULL);
        view.selectPixel(2152463529088ULL, 456, 206, 0, 0, 0);
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("historyRecords");
        QCOMPARE(rows->topLevelItemCount(), 5);
        rows->setCurrentItem(rows->topLevelItem(4));
        QSignalSpy locate(&view, &PixelHistoryView::eventRequested);
        action(view, "locateHistoryEvent")->trigger();
        QCOMPARE(locate.size(), 1); QCOMPARE(locate[0][0].toULongLong(), 36222ULL);
        QCOMPARE(locate[0][1].toLongLong(), 0LL);
        QVERIFY(view.findChild<QLabel *>("historySummary")->text().contains("History post-event color differs"));
        if (!output_.isEmpty()) {
            view.exportResult(output_ + "/indirect-boundary-history.json");
            QCOMPARE(read(output_ + "/indirect-boundary-history.json"), report);
            view.resize(1600, 760); view.show();
            QVERIFY(view.grab().save(output_ + "/indirect-boundary-history.png"));
        }
    }
    void placedHeapRestoredHistory() {
        const auto root = qEnvironmentVariable("FLORA_HISTORY_CONSISTENCY_REPORTS");
        if (root.isEmpty()) QSKIP("Set the Hellblade2 placed heap restoration evidence directory");
        const auto previous = read(root + "/hellblade-scenevelocity-consistency1/result.json");
        const auto restored = read(root + "/hellblade-scenevelocity-heap-restored1/result.json");
        QCOMPARE(previous.at("history_scope").at("replay_consistency").at("status"), Json("selected_texel_changed"));
        const auto &audit = restored.at("history_scope").at("replay_consistency");
        QCOMPARE(audit.at("status"), Json("selected_texel_unchanged"));
        QCOMPARE(audit.at("before_history"), audit.at("after_restoration"));
        QCOMPARE(previous.at("history").size(), size_t(4));
        QCOMPARE(restored.at("history").size(), size_t(5));
        const auto &last = restored.at("history").back();
        QCOMPARE(last.at("execution"), Json("36222"));
        QCOMPARE(last.at("eventId"), Json(88694));
        QCOMPARE(last.at("indirect_action"), Json(0));
        QCOMPARE(last.at("postMod").at("col").at("uintValue"), audit.at("after_restoration").at("uintValue"));
        PixelHistoryView view; view.setContext("heap-restored", 36670, true);
        view.setDx12Resource(511789, 2152463529088ULL);
        view.selectPixel(2152463529088ULL, 456, 206, 0, 0, 0);
        QVERIFY(view.finish(view.requestId(), restored));
        auto rows = view.findChild<QTreeWidget *>("historyRecords");
        rows->setCurrentItem(rows->topLevelItem(4));
        QSignalSpy locate(&view, &PixelHistoryView::eventRequested);
        action(view, "locateHistoryEvent")->trigger();
        QCOMPARE(locate.size(), 1); QCOMPARE(locate[0][0].toULongLong(), 36222ULL);
        QCOMPARE(locate[0][1].toLongLong(), 0LL);
        QVERIFY(view.findChild<QLabel *>("historySummary")->text().contains("Selected texel unchanged"));
        if (!output_.isEmpty()) {
            view.resize(1600, 720); view.show();
            QVERIFY(view.grab().save(output_ + "/heap-restored-history.png"));
        }
    }
    void pixelHistoryReplayConsistency() {
        const auto root = qEnvironmentVariable("FLORA_HISTORY_CONSISTENCY_REPORTS");
        if (root.isEmpty()) QSKIP("Set the velocity variation and stable depth history evidence directory");
        const auto changing = read(root + "/hellblade-scenevelocity-consistency1/result.json");
        QCOMPARE(changing.at("execution"), Json("36670"));
        const auto &audit = changing.at("history_scope").at("replay_consistency");
        QCOMPARE(audit.at("status"), Json("selected_texel_changed"));
        QVERIFY(audit.at("before_history").at("uintValue") != audit.at("after_restoration").at("uintValue"));
        QCOMPARE(audit.at("after_restoration").at("uintValue"), Json::array({0,0,0,0}));
        QVERIFY(changing.at("partial").get<bool>());
        QCOMPARE(changing.at("history").size(), size_t(4));
        PixelHistoryView view; view.setContext("velocity", 36670, true);
        view.setDx12Resource(511789, 2152463529088ULL);
        view.selectPixel(2152463529088ULL, 456, 206, 0, 0, 0);
        QVERIFY(view.finish(view.requestId(), changing));
        auto summary = view.findChild<QLabel *>("historySummary");
        QVERIFY(summary->text().contains("Selected texel changed"));
        QVERIFY(summary->text().contains("one consistent replay state"));
        auto rows = view.findChild<QTreeWidget *>("historyRecords");
        rows->setCurrentItem(rows->topLevelItem(3));
        QSignalSpy locate(&view, &PixelHistoryView::eventRequested);
        action(view, "locateHistoryEvent")->trigger();
        QCOMPARE(locate.size(), 1); QCOMPARE(locate[0][0].toULongLong(), 26320ULL);
        if (!output_.isEmpty()) {
            view.resize(1600, 720); view.show();
            QVERIFY(view.grab().save(output_ + "/history-consistency.png"));
            view.exportResult(output_ + "/history-consistency.json");
            QCOMPARE(read(output_ + "/history-consistency.json").at("history_scope").at("replay_consistency"), audit);
        }
        auto legacy = changing; legacy["history_scope"].erase("replay_consistency");
        QVERIFY(view.finish(view.requestId(), legacy));
        QVERIFY(summary->text().contains("not measured or unavailable"));
        QVERIFY(!summary->text().contains("Selected texel changed"));
        const auto oldRequest = view.requestId();
        view.setContext("depth", 12675, true);
        QVERIFY(!view.finish(oldRequest, changing));
        QVERIFY(!summary->text().contains("Selected texel changed"));
        const auto stable = read(root + "/talos-depth-consistency2/result.json");
        const auto &stableAudit = stable.at("history_scope").at("replay_consistency");
        QCOMPARE(stableAudit.at("status"), Json("selected_texel_unchanged"));
        QCOMPARE(stableAudit.at("before_history"), stableAudit.at("after_restoration"));
        QCOMPARE(stableAudit.at("type_cast"), Json("depth"));
        const auto selectedDepth = stableAudit.at("after_restoration").at("floatValue")[0].get<double>();
        QVERIFY(std::abs(selectedDepth - 49151.0 / 65535.0) < 1e-7);
        const auto previous = read(root + "/talos-depth-passing-history2/result.json");
        QCOMPARE(stable.at("history"), previous.at("history"));
        view.setDx12Resource(167081, 4980699392ULL);
        view.selectPixel(4980699392ULL, 1527, 1240, 0, 1, 0);
        QVERIFY(view.finish(view.requestId(), stable));
        QVERIFY(summary->text().contains("Selected texel unchanged"));
        QVERIFY(summary->text().contains("completeness are not validated"));
    }
    void indexedComputeResourceUses() {
        const auto path = qEnvironmentVariable("FLORA_DX12_INDEXED_HISTORY");
        if (path.isEmpty()) QSKIP("Set the Hellblade2 mip-1 indexed history report");
        const auto report = read(path);
        ResourceHistoryView view; view.setContext("indexed-work", 36779, 570673, 2149346324304ULL);
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("resourceHistoryRows");
        const std::map<std::string, uint32_t> expected{{"36766",4902}, {"36770",0}, {"36775",44}, {"36779",1174}};
        size_t checked{};
        for (size_t i = 0; i < report.at("resource_history").size(); ++i) {
            const auto &row = report.at("resource_history")[i];
            if (row.at("usage_name") != "CS_RWResource") continue;
            const auto found = expected.find(row.at("execution").get<std::string>());
            if (found == expected.end()) continue; // Earlier lifetime uses precede the selected Discard.
            const auto count = found->second;
            const auto &work = row.at("indexed_action");
            QCOMPARE(work.at("event_id"), row.at("eventId"));
            QCOMPARE(work.at("group_count"), Json::array({count,1,1}));
            QCOMPARE(work.at("has_workgroups"), Json(count != 0));
            QCOMPARE(row.at("evidence"), Json("bound_use"));
            QCOMPARE(row.at("content_change"), Json("not_measured"));
            auto item = rows->topLevelItem(int(i));
            QCOMPARE(item->text(5).contains("no shader invocations"), count == 0);
            QVERIFY(item->toolTip(5).contains("may differ"));
            if (count == 0) { rows->setCurrentItem(item); rows->scrollToItem(item); }
            ++checked;
        }
        QCOMPARE(checked, size_t(4));
        const auto screenshot = qEnvironmentVariable("FLORA_DX12_UI_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            applyAppearance(*qApp); view.resize(1650, 650); view.show();
            QVERIFY(view.grab().save(screenshot));
        }
        // Old reports lack work metadata and must remain readable without guessed counts.
        auto legacy = report;
        for (auto &row : legacy["resource_history"]) row.erase("indexed_action");
        QVERIFY(view.finish(view.requestId(), legacy));
        for (int i = 0; i < rows->topLevelItemCount(); ++i) QVERIFY(rows->topLevelItem(i)->text(5).isEmpty());
    }
    void indirectHistoryNavigation() {
        // Synthetic multi-action provenance; no claim of GPU multi-action coverage.
        Json row{{"execution", "9007199254740993"}, {"name", "ID3D12GraphicsCommandListExecuteIndirect"},
            {"selectable", true}, {"indirect_action", 1}, {"indirect_action_count", 2},
            {"indirect_action_kind", "draw"}, {"event_ids", {10, 11}}, {"eventId", 11},
            {"record_kind", "backend_modification"}, {"primitiveID", uint32_t(17)},
            {"preMod", {{"col", {{"floatValue", {0, 0, 0, 0}}}}}},
            {"postMod", {{"col", {{"floatValue", {1, 1, 1, 1}}}}}}};
        Json report{{"ok", true}, {"backend", "dx12"}, {"execution", "9007199254740993"},
            {"history", {row}}, {"x", 4}, {"y", 5},
            {"history_scope", {{"cpu_write_snapshots", 0}, {"cpu_write_gaps", Json::array()}, {"unmapped_records", 0}}}};
        PixelHistoryView pixel; pixel.setContext("fixture", 9007199254740993ULL, true);
        pixel.setDx12Resource(7, 8); pixel.selectPixel(8, 4, 5, 0, 0, 0);
        QVERIFY(pixel.finish(pixel.requestId(), report));
        auto rows = pixel.findChild<QTreeWidget *>("historyRecords"); rows->setCurrentItem(rows->topLevelItem(0));
        QSignalSpy locate(&pixel, &PixelHistoryView::eventRequested), debug(&pixel, &PixelHistoryView::pixelDebugRequested);
        action(pixel, "locateHistoryEvent")->trigger(); action(pixel, "debugHistoryPixel")->trigger();
        QCOMPARE(locate.size(), 1); QCOMPARE(locate[0][0].toULongLong(), 9007199254740993ULL);
        QCOMPARE(locate[0][1].toLongLong(), 1LL); QCOMPARE(debug.size(), 1);
        QCOMPARE(debug[0][4].toUInt(), 17U); QCOMPARE(debug[0][5].toLongLong(), 1LL);
        auto invalid = report; invalid["history"][0]["indirect_action"] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixel.finish(pixel.requestId(), invalid));
        QCOMPARE(rows->topLevelItemCount(), 1);
        report["history"][0]["indirect_action_kind"] = "dispatch";
        QVERIFY(pixel.finish(pixel.requestId(), report)); rows->setCurrentItem(rows->topLevelItem(0));
        QVERIFY(!action(pixel, "debugHistoryPixel")->isEnabled());
        ResourceHistoryView resource; resource.setContext("fixture", 9007199254740993ULL, 7, 8);
        Json uses{{"ok", true}, {"backend", "dx12"}, {"execution", "9007199254740993"},
            {"resource_object", "8"}, {"resource_creation", "7"}, {"resource_history", {row}}};
        QVERIFY(resource.finish(resource.requestId(), uses));
        auto resourceRows = resource.findChild<QTreeWidget *>("resourceHistoryRows");
        resourceRows->setCurrentItem(resourceRows->topLevelItem(0));
        QSignalSpy resourceLocate(&resource, &ResourceHistoryView::eventRequested);
        action(resource, "locateResourceHistoryEvent")->trigger();
        QCOMPARE(resourceLocate.size(), 1); QCOMPARE(resourceLocate[0][1].toLongLong(), 1LL);
        uses["resource_history"][0].erase("indirect_action_count");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, resource.finish(resource.requestId(), uses));
    }
    void dx12PixelInvocationReference() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PS_FRAGMENT_REPORT");
        if (path.isEmpty()) QSKIP("Set the BF1 particle fragment reference report");
        const auto report = read(path);
        QCOMPARE(report.at("execution"), Json("36560"));
        QVERIFY(report.at("gpa_modules_loaded").empty());
        const auto &reference = report.at("gpu_invocation_reference");
        QCOMPARE(reference.at("status"), Json("read"));
        QCOMPARE(reference.at("matching_fragments"), Json(1));
        QCOMPARE(reference.at("primitive"), Json(1322));
        QCOMPARE(reference.at("target_consistency"), Json("selected_texel_unchanged"));
        QCOMPARE(reference.at("history_boundary_consistency"), Json("exact_match"));
        Json output;
        for (const auto &step : report.at("steps"))
            for (const auto &change : step.at("changes"))
                if (change.at("after").at("name") == "o0") output = change.at("after").at("value").at("u32v");
        QVERIFY(output.is_array());
        for (size_t i = 0; i < 4; ++i)
            QCOMPARE(output.at(i), reference.at("value").at("uintValue").at(i));
        QVERIFY(reference.at("value").at("uintValue") != report.at("gpu_reference").at("value").at("uintValue"));
        ReplayDebugView view("ps");
        view.resize(1500, 950); view.show();
        view.setContext("particle:barriers", 36560, true);
        view.selectPixel(836, 241, 0);
        auto checkbox = view.findChild<QCheckBox *>("debugPixelHistoryReference");
        QVERIFY(checkbox); QVERIFY(!checkbox->isChecked());
        checkbox->setChecked(true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        view.selectPrimitive(1322);
        QCOMPARE(view.request().at("pixel_history_reference"), Json(true));
        auto snapshot = view.findChild<QCheckBox *>("debugReadOnlyDepthSnapshot");
        QVERIFY(snapshot); QVERIFY(!snapshot->isChecked());
        snapshot->setChecked(true);
        QCOMPARE(view.request().at("readonly_depth_snapshot"), Json(true));
        QVERIFY(view.finish(view.requestId(), report));
        auto label = view.findChild<QLabel *>("debugGpuInvocationReference");
        QVERIFY(label); QVERIFY(!label->isHidden());
        QVERIFY(label->text().contains("before blending"));
        QCOMPARE(view.exportReport().at("gpu_invocation_reference"), reference);
        view.findChild<QSpinBox *>("debugStepIndex")->setValue(int(view.model()->size() - 1));
        QCoreApplication::processEvents();
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/pixel-fragment-reference.png"));
        const auto unavailable = read(qEnvironmentVariable("FLORA_DX12_PS_REJECTED_REPORT"));
        const auto &rejected = unavailable.at("gpu_invocation_reference");
        QCOMPARE(rejected.at("status"), Json("unavailable"));
        QCOMPARE(rejected.at("primitive"), Json(265));
        QVERIFY(rejected.at("fragment").at("depthTestFailed").get<bool>());
        QVERIFY(!rejected.contains("value"));
        view.selectPrimitive(265);
        QVERIFY(view.finish(view.requestId(), unavailable));
        QVERIFY(label->text().contains("unavailable: The matching fragment did not pass"));
        const auto request = view.requestId();
        view.setContext("other-session", 36560, true);
        QVERIFY(label->isHidden()); QVERIFY(!checkbox->isChecked());
        QVERIFY(!snapshot->isChecked());
        QVERIFY(!view.finish(request, report));
    }
    void dx12TemporalStoreReference() {
        const auto root = qEnvironmentVariable("FLORA_DX12_TEMPORAL_EVIDENCE_DIR");
        if (root.isEmpty())
            QSKIP("Set the BF1 temporal shader evidence directory");
        const auto output = read(root + "/bf1-temporal-cs-stability2/result.json");
        const auto history = read(root + "/bf1-temporal-history-store1/result.json");
        // These stores contain positive normal values within R11G11B10's
        // range. D3D high-to-low floating conversion truncates the mantissa,
        // unlike the round-to-nearest RGBA8 display conversion.
        const auto storage = [](const Json &report, uint32_t instruction) {
            Json value;
            for (const auto &step : report.at("steps")) {
                for (const auto &change : step.at("changes"))
                    if (change.at("after").at("name") == "r1")
                        value = change.at("after").at("value").at("f32v");
                if (step.at("nextInstruction") == instruction) {
                    Json rgb = Json::array();
                    for (size_t i = 0; i < 3; ++i) {
                        const float f = value.at(i).get<float>();
                        if (!std::isfinite(f) || f < std::ldexp(1.0f, -14) || f > 64512.0f)
                            throw std::runtime_error("Temporal fixture needs positive normal RGB");
                        const uint32_t mantissa = i == 2 ? 5U : 6U;
                        const uint32_t bits = std::bit_cast<uint32_t>(f) &
                                              (UINT32_MAX << (23U - mantissa));
                        rgb.push_back(std::bit_cast<float>(bits));
                    }
                    return rgb;
                }
            }
            throw std::runtime_error("Missing temporal UAV store");
        };
        const auto rgb = [](const Json &value) {
            const auto &v = value.at("floatValue");
            return Json::array({v.at(0), v.at(1), v.at(2)});
        };
        for (const auto *report : {&output, &history}) {
            QVERIFY(report->at("ok").get<bool>());
            QVERIFY(report->at("gpa_modules_loaded").empty());
            QCOMPARE(report->at("backend_dxbc_workgroup_patch"), Json(1));
            QCOMPARE(report->at("execution"), Json("36666"));
        }
        const auto &reference = output.at("gpu_reference");
        QCOMPARE(reference.at("resource_creation"), Json("299115"));
        QCOMPARE(reference.at("consistency"), Json("selected_texel_changed"));
        QCOMPARE(storage(output, 373), rgb(reference.at("after_trace_creation_value")));
        QVERIFY(storage(output, 373) != rgb(reference.at("value")));
        QCOMPARE(reference.at("value"), reference.at("after_debug_value"));
        QCOMPARE(history.at("gpu_reference").at("resource_creation"), Json("286299"));
        QCOMPARE(storage(history, 295), rgb(history.at("gpu_reference").at("value")));
    }
    void dx12DebugReferenceStability() {
        const auto path = qEnvironmentVariable("FLORA_DX12_STABILITY_REPORT");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_STABILITY_REPORT to a trace with before/after GPU reference values");
        auto report = read(path);
        QVERIFY(report.at("ok").get<bool>());
        QVERIFY(report.at("gpa_modules_loaded").empty());
        const auto &reference = report.at("gpu_reference");
        const bool same = reference.at("value").at("uintValue") ==
                              reference.at("after_debug_value").at("uintValue") &&
                          reference.at("value").at("uintValue") ==
                              reference.at("after_trace_creation_value").at("uintValue");
        QCOMPARE(reference.at("consistency"),
                 Json(same ? "selected_texel_unchanged" : "selected_texel_changed"));
        if (!same) {
            QVERIFY(report.at("partial").get<bool>());
            QVERIFY(!report.at("debug_scope").at("warnings").empty());
        }
        ReplayDebugView view("cs");
        view.setContext("stability", 36666, true);
        QVERIFY(view.finish(view.requestId(), report));
        auto label = view.findChild<QLabel *>("debugGpuReference");
        QVERIFY(label->text().contains("After debug replays"));
        QVERIFY(label->text().contains("After trace creation"));
        QCOMPARE(view.exportReport().at("gpu_reference"), reference);
        // A changed-reference fixture must remain distinguishable from the
        // real observation and from older reports.
        auto changed = report;
        changed["gpu_reference"]["after_debug_value"]["floatValue"] = {0.25, 0.5, 0.75, 1.0};
        changed["gpu_reference"]["after_debug_value"]["uintValue"] = {
            1048576000U, 1056964608U, 1061158912U, 1065353216U};
        changed["gpu_reference"]["after_debug_value"]["intValue"] = {
            1048576000, 1056964608, 1061158912, 1065353216};
        changed["gpu_reference"]["consistency"] = "selected_texel_changed";
        changed["partial"] = true;
        changed["debug_scope"]["warnings"] = {"The selected GPU texel changed during shader-debug replays."};
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("changed; reference cannot validate this trace"));
        QVERIFY(label->text().contains("0.25"));
        auto notice = view.findChild<QLabel *>("debugBackendNotice");
        QVERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains("GPU texel changed"));
        QCOMPARE(view.exportReport().at("gpu_reference"), changed.at("gpu_reference"));
        // A transient change is still a warning even if the final texel
        // returned to its initial value.
        changed["gpu_reference"]["after_trace_creation_value"] =
            changed["gpu_reference"]["after_debug_value"];
        changed["gpu_reference"]["after_debug_value"] = reference.at("value");
        QVERIFY(view.finish(view.requestId(), changed));
        QVERIFY(label->text().contains("changed; reference cannot validate this trace"));
        QVERIFY(label->text().contains("0.25"));
        auto unchanged = report;
        unchanged["gpu_reference"]["after_trace_creation_value"] = reference.at("value");
        unchanged["gpu_reference"]["after_debug_value"] = reference.at("value");
        unchanged["gpu_reference"]["consistency"] = "selected_texel_unchanged";
        unchanged["debug_scope"].erase("warnings");
        unchanged["partial"] = false;
        QVERIFY(view.finish(view.requestId(), unchanged));
        QVERIFY(label->text().contains("input stability is not established"));
        QVERIFY(notice->isHidden());
        auto legacy = report;
        legacy["gpu_reference"].erase("after_trace_creation_value");
        legacy["gpu_reference"].erase("after_debug_value");
        legacy["gpu_reference"].erase("consistency");
        legacy["gpu_reference"].erase("consistency_scope");
        legacy["debug_scope"].erase("warnings");
        QVERIFY(view.finish(view.requestId(), legacy));
        QVERIFY(!label->text().contains("After debug replays"));
        QVERIFY(notice->isHidden());
        view.setContext("different-capture", 1, true);
        QVERIFY(label->isHidden());
    }
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
    void dx12IndirectInvocationReport() {
        const auto path = qEnvironmentVariable("FLORA_DX12_INDIRECT_PIPELINE_REPORT");
        if (path.isEmpty()) QSKIP("Set FLORA_DX12_INDIRECT_PIPELINE_REPORT to the Hellblade2 indirect pipeline result");
        const auto report = read(path);
        QVERIFY(report.at("ok").get<bool>());
        Dx12PipelineView view;
        view.setContext("hellblade:indirect", 36779);
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("dx12PipelineRows");
        bool found = false;
        for (int i = 0; i < rows->topLevelItemCount(); ++i) {
            const auto &row = report.at("pipeline_rows")[i];
            if (row.at("section") != "Execution") continue;
            const auto &details = row.at("details");
            QCOMPARE(details.at("group_count"), Json::array({1174, 1, 1}));
            QCOMPARE(details.at("shader_group_size"), Json::array({16, 16, 1}));
            QCOMPARE(details.at("indirect"), Json(true));
            rows->setCurrentItem(rows->topLevelItem(i));
            const auto displayed = view.findChild<QPlainTextEdit *>()->toPlainText();
            QCOMPARE(Json::parse(displayed.toStdString()), row);
            QVERIFY(!action(view, "inspectPipelineResource")->isEnabled());
            QVERIFY(!action(view, "locatePipelineCreation")->isEnabled());
            QVERIFY(!action(view, "replaceBoundPipelineShader")->isEnabled());
            found = true;
        }
        QVERIFY(found);
        const auto serial = view.requestId();
        view.setContext("hellblade:changed-experiment", 36779);
        QCOMPARE(rows->topLevelItemCount(), 0);
        QVERIFY(!view.finish(serial, report));
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
