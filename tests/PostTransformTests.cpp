#include "DepthStencilCapture.h"
#include "HullCapture.h"
#include "StreamCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "application/CheckpointInspection.h"
#include "application/DxbcIdentity.h"
#include "application/InvocationSelector.h"
#include "application/PostTransform.h"
#include "application/SessionUi.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class PostTransformTests final : public QObject {
    Q_OBJECT
    void projectFile(MainWindow &window, const char *action, const QString &path) {
        bool handled = false;
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->selectFile(path);
            handled = true;
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>(action)->trigger();
        QVERIFY(handled);
    }
  private slots:
    void checkpointCaptureAndIsolation() {
        try {
            QTemporaryDir dir;
            using Json = nlohmann::json;
            for (const std::string stage : {"gs", "ds", "hs"})
                for (bool warp : {false, true}) {
                    const auto path = dir.path() + "/checkpoint.gpa_frame";
                    if (stage == "gs")
                        streamCapture().save(path);
                    else
                        hullCapture().save(path);
                    Frame frame(path.toStdWString());
                    ReplayOptions opts;
                    opts.warp = warp;
                    opts.before = true;
                    opts.until = stage == "gs" ? 150 : 200;
                    Replay replay(frame, opts);
                    replay.run();
                    const Id storageId = stage == "gs" ? 70 : 10;
                    const auto storage = replay.readBuffer(storageId), pixels = replay.readTexture(20);
                    const auto counter = stage == "gs" ? 0 : replay.readCounter(12);
                    const auto counts = replay.counts;
                    CheckpointInspectionOptions request;
                    request.stage = stage;
                    const auto catalog = inspectCheckpoint(replay, opts.until, request);
                    QCOMPARE(catalog.report.at("record_count").get<unsigned>(), 0u);
                    QVERIFY(!catalog.shader.empty());
                    QVERIFY(!catalog.assembly.empty());
                    QVERIFY(!catalog.report.contains("register_capture"));
                    const auto phases =
                        stage == "hs" ? catalog.report.at("hs_phases") : Json::array({nullptr});
                    for (const auto &phase : phases) {
                        request.trace = true;
                        request.instruction.reset();
                        request.inputSelector = nullptr;
                        if (!phase.is_null())
                            request.hullPhase = phase.at("id");
                        auto capture = inspectCheckpoint(replay, opts.until, request);
                        const auto &meta = capture.report.at("register_capture");
                        qInfo().noquote()
                            << QString::fromStdString(stage) << warp << QString::fromStdString(phase.dump())
                            << QString::fromStdString(meta.at("attempts").dump());
                        const auto &initialAttempt = meta.at("attempts").at(0);
                        const bool overflow = initialAttempt.at("observed_records").get<uint64_t>() >
                                              initialAttempt.at("capacity").get<uint64_t>();
                        QCOMPARE(meta.at("attempts").size(), overflow ? size_t(2) : size_t(1));
                        if (stage != "hs" || phase.at("kind") == "control_points")
                            QVERIFY(overflow);
                        QVERIFY(capture.report.at("record_count").get<unsigned>() > 0);
                        QVERIFY(!capture.bytes.empty());
                        QCOMPARE(replay.readBuffer(storageId), storage);
                        QCOMPARE(replay.readTexture(20), pixels);
                        if (stage != "gs")
                            QCOMPARE(replay.readCounter(12), counter);
                        QVERIFY(replay.counts == counts);
                        const auto firstCapacity = meta.at("attempts").at(0).at("capacity").get<uint64_t>();
                        request.maxBytes = meta.value("data_offset", 16u) +
                                           firstCapacity * meta.at("record_stride").get<uint64_t>();
                        if (meta.at("attempts").size() == 1)
                            --request.maxBytes;
                        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                                 inspectCheckpoint(replay, opts.until, request));
                        request.maxBytes = 256ull * 1024 * 1024;
                        QCOMPARE(replay.readBuffer(storageId), storage);
                        if (stage != "gs")
                            QCOMPARE(replay.readCounter(12), counter);
                        const auto rows = checkpointHeaders(capture.bytes, meta);
                        const auto &first = rows.at(0);
                        const auto registers = checkpointRegisters(capture.bytes, meta, first.at("record"));
                        if (!checkpoint::inputKeys(meta.at("register_slots"), meta.at("known_inputs"))
                                 .empty()) {
                            auto selector =
                                checkpoint::selectorFromSnapshot(capture.report, registers, first, "all");
                            request.inputSelector = selector;
                            const auto selected = inspectCheckpoint(replay, opts.until, request);
                            const auto matched = selected.report.at("register_capture")
                                                     .at("matched_invocations")
                                                     .get<uint32_t>();
                            QVERIFY(matched > 0);
                            request.inputSelector["match_policy"] = "unique";
                            if (matched == 1)
                                QVERIFY(inspectCheckpoint(replay, opts.until, request)
                                            .report.at("record_count")
                                            .get<uint32_t>() > 0);
                            else
                                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                                         inspectCheckpoint(replay, opts.until, request));
                            request.inputSelector = selector;
                            request.inputSelector["inputs"][0]["bits"] =
                                selector.at("inputs").at(0).at("bits").get<uint32_t>() ^ 0xffffffffu;
                            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                                     inspectCheckpoint(replay, opts.until, request));
                            request.inputSelector = nullptr;
                        }
                        request.trace = false;
                        for (const auto &entry : catalog.report.at("catalog"))
                            if (entry.at("checkpoint_allowed") == true && entry.at("opcode") == 62 &&
                                (phase.is_null() || entry.at("hs_phase") == phase.at("id"))) {
                                request.instruction = entry.at("instruction");
                                break;
                            }
                        QVERIFY(request.instruction.has_value());
                        const auto single = inspectCheckpoint(replay, opts.until, request);
                        QCOMPARE(single.report.at("checkpoint"), Json(*request.instruction));
                        QVERIFY(single.report.at("record_count").get<unsigned>() > 0);
                        const auto target = std::filesystem::path(dir.path().toStdWString()) / "export";
                        exportCheckpoint(single, target);
                        QCOMPARE(std::filesystem::file_size(target / "snapshots.bin"), single.bytes.size());
                        QVERIFY(std::filesystem::file_size(target / "hits.csv") > 0);
                        QVERIFY(std::filesystem::file_size(target / "registers.csv") > 0);
                        QCOMPARE(replay.readBuffer(storageId), storage);
                        QCOMPARE(replay.readTexture(20), pixels);
                        if (stage != "gs")
                            QCOMPARE(replay.readCounter(12), counter);
                        QVERIFY(replay.counts == counts);
                    }
                    replay.inspectNativeState([&](auto context, const auto &) {
                        if (stage == "gs")
                            context->Draw(1, 0);
                        else
                            context->DrawInstanced(6, 2, 0, 9);
                    });
                    const auto after = replay.readBuffer(storageId);
                    const auto afterCounter = stage == "gs" ? 0 : replay.readCounter(12);
                    opts.before = false;
                    Replay baseline(frame, opts);
                    baseline.run();
                    QCOMPARE(after, baseline.readBuffer(storageId));
                    if (stage != "gs")
                        QCOMPARE(afterCounter, baseline.readCounter(12));
                    opts.before = true;
                    request.trace = true;
                    request.instruction.reset();
                    request.inputSelector = nullptr;
                    for (bool suppress : {false, true}) {
                        opts.suppressDraws = suppress;
                        opts.disabled.clear();
                        if (!suppress)
                            opts.disabled.insert(opts.until);
                        Replay disabled(frame, opts);
                        disabled.run();
                        const auto empty = inspectCheckpoint(disabled, opts.until, request);
                        QVERIFY(empty.bytes.empty());
                        QCOMPARE(empty.report.at("record_count").get<unsigned>(), 0u);
                        QVERIFY(!empty.report.at("register_capture").at("enabled").get<bool>());
                        QCOMPARE(empty.report.at("register_capture").at("invocations").get<unsigned>(), 0u);
                    }
                }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void geometryProjectSelection() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/geometry.gpa_frame";
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        using Json = nlohmann::json;
        const Json original{{"geometry_selection",
                             {{"stage", "GS"},
                              {"stream", "3"},
                              {"instance", "4294967295"},
                              {"ia_table", "唯一顶点"},
                              {"future", 42}}}};
        auto state = replayUiState(frame, original);
        QCOMPARE(state.geometryStage, std::string("gs"));
        QCOMPARE(state.geometryStream, 3u);
        QCOMPARE(state.geometryInstance, std::string("4294967295"));
        QCOMPARE(state.geometryTable, std::string("unique_vertices"));
        auto saved = replayUiDocument(frame, state, original);
        QVERIFY(saved.at("geometry_selection") == original.at("geometry_selection"));
        state.geometryStage = "ia";
        QCOMPARE(replayUiState(frame, replayUiDocument(frame, state)).geometryStage, std::string("ia"));
        state.geometryStage = "vs-index";
        QCOMPARE(replayUiState(frame, replayUiDocument(frame, state)).geometryStage, std::string("vs-index"));
        for (const auto *stage : {"vs-writes", "ds-writes", "gs-emits", "hs"}) {
            state.geometryStage = stage;
            QCOMPARE(replayUiState(frame, replayUiDocument(frame, state)).geometryStage, std::string(stage));
        }
        state.geometryTable = "patch_constants";
        QCOMPARE(replayUiState(frame, replayUiDocument(frame, state)).geometryTable,
                 std::string("patch_constants"));
        for (const auto *invalid : {"-1", "4294967296", "x", "1.2", "+1"}) {
            state.geometryInstance = invalid;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayUiDocument(frame, state));
        }
    }
    void identityBitVariants() {
        PostTransformGeometry geometry;
        geometry.report = {{"vertices", 6},
                           {"stride", 4},
                           {"vertices_per_primitive", 1},
                           {"event", {{"id", 100}}},
                           {"vertex_identity", nlohmann::json::object()},
                           {"attributes",
                            {{{"semantic", "NAN"},
                              {"index", 0},
                              {"components", {0}},
                              {"component_count", 1},
                              {"component_type", 3},
                              {"system_value", 0},
                              {"offset", 0}}}}};
        geometry.bytes = statePack(0x7fc00001u, 0x7fc00002u, 0x7fc00001u, 0u, 0x80000000u, 0x7fc00001u);
        geometry.identities.assign(5, {0, 10, 10});
        geometry.identities.push_back({1, 10, 10});
        const auto tables = postTransformTables(geometry);
        QCOMPARE(tables.at("unique_vertices").get<unsigned>(), 5u);
        QCOMPARE(tables.at("unique_identities").get<unsigned>(), 2u);
        QCOMPARE(tables.at("conflicting_identities").get<unsigned>(), 1u);
        const unsigned variants[]{0, 1, 0, 2, 3, 0};
        const auto &refs = tables.at("tables").at("references").at("rows");
        for (unsigned i = 0; i < 6; ++i)
            QCOMPARE(refs[i][7].get<unsigned>(), variants[i]);
        QTemporaryDir dir;
        exportPostTransform(geometry, dir.path().toStdWString());
        QFile file(dir.path() + "/unique_vertices.bin");
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto unique = file.readAll();
        QCOMPARE(unique.size(), qsizetype(20));
        QByteArray restored;
        for (const auto &row : refs)
            restored += unique.mid(row[8].get<unsigned>() * 4, 4);
        QCOMPARE(restored,
                 QByteArray(reinterpret_cast<const char *>(geometry.bytes.data()), geometry.bytes.size()));
    }
    void outputIsolationAndFailure() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/counter.gpa_frame";
        graphicsCounterCapture(true).save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions o;
            o.warp = warp;
            o.until = 200;
            o.before = true;
            o.shaders[30] = compileClassProgram(
                "RWStructuredBuffer<uint> data:register(u1);"
                "float4 main(uint id:SV_VertexID):SV_Position {uint n=data.IncrementCounter();data[n]=100;"
                "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],n/4.,1);}",
                "vs_5_0");
            Replay replay(frame, o);
            replay.run();
            const auto storage = replay.readBuffer(10), target = replay.readTexture(20);
            const auto count = replay.readCounter(12);
            const auto commands = replay.counts;
            QCOMPARE(count, 0u);
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
                PostTransformOptions inspect;
                inspect.stage = repeat == 2 ? "vs-writes" : repeat ? "vs-index" : "final";
                const auto geometry = inspectPostTransform(replay, 200, inspect);
                QCOMPARE(geometry.report.at("vertices").get<unsigned>(), 3u);
                if (repeat != 2)
                    QVERIFY(geometry.report.at("pre_raster_uav_isolation").at("stages") ==
                            nlohmann::json::array({"vs"}));
                else
                    QCOMPARE(geometry.report.at("vertex_writes").at("records").get<unsigned>(), 3u);
                QCOMPARE(replay.readBuffer(10), storage);
                QCOMPARE(replay.readTexture(20), target);
                QCOMPARE(replay.readCounter(12), count);
                QVERIFY(replay.counts == commands);
            }
            PostTransformOptions bounded;
            bounded.maxBytes = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 200, bounded));
            QCOMPARE(replay.readBuffer(10), storage);
            QCOMPARE(replay.readCounter(12), count);
            QCOMPARE(inspectPostTransform(replay, 200).bytes.size(), size_t(48));
            // Native state is usable immediately after inspection, without a replay reset.
            replay.inspectNativeState([](auto context, const auto &) { context->Draw(3, 0); });
            const auto after = replay.readBuffer(10);
            const auto afterCount = replay.readCounter(12);
            o.before = false;
            Replay baseline(frame, o);
            baseline.run();
            QCOMPARE(after, baseline.readBuffer(10));
            QCOMPARE(afterCount, baseline.readCounter(12));
        }
    }
    void streamCursorAndSignatureProvider() {
        QTemporaryDir dir;
        for (bool signature : {false, true})
            for (bool dirty : {false, true}) {
                const auto path = dir.path() + "/so.gpa_frame";
                streamCapture(signature, dirty).save(path);
                Frame frame(path.toStdWString());
                for (bool warp : {false, true}) {
                    ReplayOptions o;
                    o.warp = warp;
                    o.until = 150;
                    o.before = true;
                    Replay replay(frame, o);
                    replay.run();
                    const auto before = replay.readBuffer(70);
                    for (unsigned i = 0; i < 2; ++i) {
                        auto geometry = inspectPostTransform(replay, 150);
                        QCOMPARE(geometry.bytes.size(), size_t(48));
                        QCOMPARE(replay.readBuffer(70), before);
                    }
                    PostTransformOptions indexed;
                    indexed.stage = "vs-index";
                    auto identities = inspectPostTransform(replay, 150, indexed);
                    QCOMPARE(identities.identities.size(), size_t(signature ? 3 : 1));
                    QCOMPARE(replay.readBuffer(70), before);
                    for (const auto *stage : {"vs-writes", "gs-emits"}) {
                        PostTransformOptions log;
                        log.stage = stage;
                        if (signature && log.stage == "gs-emits") {
                            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                                     inspectPostTransform(replay, 150, log));
                            continue;
                        }
                        const auto history = replay.counts;
                        for (unsigned repeat = 0; repeat < 2; ++repeat) {
                            const auto records = inspectPostTransform(replay, 150, log);
                            const auto key = log.stage == "gs-emits" ? "geometry_emissions" : "vertex_writes";
                            QVERIFY(records.report.at(key).at("records").get<unsigned>() > 0);
                            QCOMPARE(
                                records.report.at(key).at("stream_output").at("strategy").get<std::string>(),
                                std::string("private_buffers_at_tracked_byte_cursors"));
                            QCOMPARE(replay.readBuffer(70), before);
                            QVERIFY(replay.counts == history);
                        }
                    }
                    // A direct producer after the helper must resume the original hidden cursor.
                    replay.inspectNativeState(
                        [&](auto context, const auto &) { context->Draw(signature ? 3 : 1, 0); });
                    auto actual = replay.readBuffer(70);
                    o.before = false;
                    Replay baseline(frame, o);
                    baseline.run();
                    QCOMPARE(actual, baseline.readBuffer(70));
                }
            }
    }
    void overflowFailureRestoresState() {
        QTemporaryDir dir;
        auto capture = streamCapture();
        for (const auto &entry : capture.entries)
            if (entry.id == 100)
                put(capture.bytes, entry.offset + 24, 1025u);
        const auto path = dir.path() + "/overflow.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions options;
            options.warp = warp;
            options.until = 100;
            options.before = true;
            options.shaders[30] =
                compileClassProgram("float4 main():SV_Position{return float4(0,0,0,1);}", "vs_5_0");
            Replay replay(frame, options);
            replay.run();
            const auto before = replay.readBuffer(70);
            PostTransformOptions bounded;
            bounded.maxBytes = 1024 * 16;
            // The first draw fills the private buffer; the expansion then exceeds the limit.
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 100, bounded));
            QCOMPARE(replay.readBuffer(70), before);
            const auto geometry = inspectPostTransform(replay, 100);
            QCOMPARE(geometry.report.at("vertices").get<unsigned>(), 3075u);
            QCOMPARE(geometry.report.at("attempts").size(), size_t(2));
            QCOMPARE(replay.readBuffer(70), before);
            replay.inspectNativeState([](auto context, const auto &) { context->Draw(1025, 0); });
            const auto actual = replay.readBuffer(70);
            options.before = false;
            Replay baseline(frame, options);
            baseline.run();
            QCOMPARE(actual, baseline.readBuffer(70));
            // Restore the same before boundary after the baseline submitted the draw.
            options.before = true;
            Replay identityReplay(frame, options);
            identityReplay.run();
            bounded.stage = "vs-index";
            bounded.maxBytes = 1024 * 24;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(identityReplay, 100, bounded));
            QCOMPARE(identityReplay.readBuffer(70), before);
            bounded.maxBytes = 256ull * 1024 * 1024;
            const auto identity = inspectPostTransform(identityReplay, 100, bounded);
            QCOMPARE(identity.identities.size(), size_t(1025));
            QCOMPARE(identity.report.at("attempts").size(), size_t(2));
            identityReplay.inspectNativeState([](auto context, const auto &) { context->Draw(1025, 0); });
            QCOMPARE(identityReplay.readBuffer(70), actual);
        }
    }
    void outputLogOverflowRestoresCursor() {
        QTemporaryDir dir;
        auto capture = streamCapture();
        for (const auto &entry : capture.entries)
            if (entry.id == 100)
                put(capture.bytes, entry.offset + 24, 400u);
        const auto path = dir.path() + "/log-overflow.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions o;
            o.warp = warp;
            o.before = true;
            o.until = 100;
            Replay replay(frame, o);
            replay.run();
            const auto before = replay.readBuffer(70), pixels = replay.readTexture(20);
            PostTransformOptions bounded;
            bounded.stage = "gs-emits";
            bounded.maxBytes = 16 + 400 * 64;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 100, bounded));
            QCOMPARE(replay.readBuffer(70), before);
            QCOMPARE(replay.readTexture(20), pixels);
            bounded.maxBytes = 256ull * 1024 * 1024;
            const auto log = inspectPostTransform(replay, 100, bounded);
            QCOMPARE(log.report.at("geometry_emissions").at("attempts").size(), size_t(2));
            QCOMPARE(log.report.at("vertices").get<unsigned>(), 1200u);
            QCOMPARE(replay.readBuffer(70), before);
            replay.inspectNativeState([](auto context, const auto &) { context->Draw(400, 0); });
            const auto actual = replay.readBuffer(70);
            o.before = false;
            Replay baseline(frame, o);
            baseline.run();
            QCOMPARE(actual, baseline.readBuffer(70));
        }
    }
    void hullOutputAndIsolation() {
        QTemporaryDir dir;
        for (bool explicitPhase : {false, true})
            for (bool warp : {false, true}) {
                const auto path = dir.path() + "/hull.gpa_frame";
                hullCapture(explicitPhase).save(path);
                Frame frame(path.toStdWString());
                ReplayOptions o;
                o.warp = warp;
                o.before = true;
                o.until = 200;
                Replay replay(frame, o);
                replay.run();
                const auto storage = replay.readBuffer(10), pixels = replay.readTexture(20);
                const auto counter = replay.readCounter(12);
                const auto counts = replay.counts;
                PostTransformOptions options;
                options.stage = "hs";
                options.maxBytes = 1;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 200, options));
                options.maxBytes = 256ull * 1024 * 1024;
                PostTransformOptions dsOptions;
                dsOptions.stage = "ds";
                const auto originalDs = inspectPostTransform(replay, 200, dsOptions);
                options.hullDownstream = true;
                auto full = inspectPostTransform(replay, 200, options);
                QCOMPARE(full.downstreamBytes, originalDs.bytes);
                QCOMPARE(full.report.at("hull").at("patches").get<unsigned>(), 4u);
                const auto ui = postTransformTables(full);
                const auto &rows = ui.at("tables").at("expanded_vertices").at("rows");
                QCOMPARE(rows.size(), size_t(12));
                for (unsigned i = 0; i < 12; ++i) {
                    const auto &row = rows[i];
                    QCOMPARE(row[0].get<unsigned>(), i / 6);
                    QCOMPARE(row[1].get<unsigned>(), (i % 6) / 3);
                    QCOMPARE(row[2].get<unsigned>(), i % 3);
                    QCOMPARE(row[3].get<double>(), double(i % 6));
                    QCOMPARE(row[5].get<double>(), double(i / 6));
                    QCOMPARE(row[7].get<double>(),
                             explicitPhase ? double(i % 3) * .25 + double((i % 6) / 3) : 0.0);
                    for (unsigned j : {4u, 6u, 8u, 10u})
                        QVERIFY(row[j].get<bool>());
                }
                for (unsigned repeat = 0; repeat < 2; ++repeat) {
                    options.instance = 1;
                    auto selected = inspectPostTransform(replay, 200, options);
                    const auto bytes = full.report.at("hull").at("patch_stride").get<unsigned>() * 2;
                    std::vector<uint8_t> expected(full.bytes.begin() + bytes, full.bytes.begin() + 2 * bytes);
                    expected.insert(expected.end(), full.bytes.begin() + 3 * bytes, full.bytes.end());
                    QCOMPARE(selected.bytes, expected);
                    QCOMPARE(replay.readBuffer(10), storage);
                    QCOMPARE(replay.readTexture(20), pixels);
                    QCOMPARE(replay.readCounter(12), counter);
                    QVERIFY(replay.counts == counts);
                }
                replay.inspectNativeState(
                    [](auto context, const auto &) { context->DrawInstanced(6, 2, 0, 9); });
                const auto after = replay.readBuffer(10);
                const auto afterCounter = replay.readCounter(12);
                o.before = false;
                Replay baseline(frame, o);
                baseline.run();
                QCOMPARE(after, baseline.readBuffer(10));
                QCOMPARE(afterCounter, baseline.readCounter(12));
                QCOMPARE(afterCounter, 4u);
            }
    }
    void hullWindowInspection() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/hull.gpa_frame";
        hullCapture().save(path);
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QTimer dialogs;
        QStringList unexpectedDialogs;
        connect(&dialogs, &QTimer::timeout, &window, [&] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(widget)) {
                    unexpectedDialogs.push_back(box->text());
                    qWarning() << "Unexpected HS dialog:" << box->text();
                    box->reject();
                }
        });
        dialogs.start(100);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            const auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 200) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        auto stage = window.findChild<QComboBox *>("geometryStage"),
             choice = window.findChild<QComboBox *>("geometryTableChoice");
        stage->setCurrentIndex(stage->findData("hs"));
        auto table = window.findChild<QTableView *>("geometryTable");
        QVERIFY(table);
        tasks.clear();
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(table->model()->rowCount(), 12);
        QVERIFY(choice->isEnabled());
        choice->setCurrentIndex(choice->findData("patch_constants"));
        QCOMPARE(table->model()->rowCount(), 4);
        const auto evidence = qEnvironmentVariable("FLORA_POST_TRANSFORM_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(window.grab().save(evidence + "/hs-patch-constants.png"));
        }
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            dialog->setDirectory(dir.path());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>("exportGeometry")->trigger();
        for (const auto *file :
             {"geometry.json", "vertices.bin", "vertices.validity.bin", "vertices.csv", "patch_constants.bin",
              "patch_constants.validity.bin", "patch_constants.csv"})
            QVERIFY(QFile::exists(dir.path() + "/FloraGPA-Geometry-200/" + file));
        QVERIFY(!QFile::exists(dir.path() + "/FloraGPA-Geometry-200/geometry.obj"));
        const auto project = dir.path() + "/hull-project.json";
        projectFile(window, "saveExperiment", project);
        stage->setCurrentIndex(stage->findData("vs"));
        projectFile(window, "openExperiment", project);
        QCOMPARE(stage->currentData().toString(), QString("hs"));
        QCOMPARE(choice->currentData().toString(), QString("patch_constants"));
        QVERIFY2(unexpectedDialogs.isEmpty(), qPrintable(unexpectedDialogs.join('\n')));
    }
    void emptyAndNonfiniteExports() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/depth.gpa_frame";
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        ReplayOptions o;
        o.warp = true;
        o.until = 1000;
        o.before = true;
        Replay replay(frame, o);
        replay.run();
        auto geometry = inspectPostTransform(replay, 1000);
        const auto destination = dir.path().toStdWString();
        exportPostTransform(geometry, destination);
        QVERIFY(QFile::exists(dir.path() + "/geometry.obj"));
        o.shaders[30] = compileClassProgram(
            "float4 main(uint id:SV_VertexID):SV_Position {return float4(id,0,0,0);}", "vs_5_0");
        Replay zeroW(frame, o);
        zeroW.run();
        geometry = inspectPostTransform(zeroW, 1000);
        QVERIFY(!geometry.report.at("obj_unavailable_reason").is_null());
        auto tables = postTransformTables(geometry);
        QVERIFY(!tables.at("obj_unavailable_reason").is_null());
        exportPostTransform(geometry, destination);
        QVERIFY(!QFile::exists(dir.path() + "/geometry.obj"));
        QVERIFY(QFile::exists(dir.path() + "/vertices.bin"));
        o.disabled.insert(1000);
        Replay disabled(frame, o);
        disabled.run();
        auto empty = inspectPostTransform(disabled, 1000);
        QVERIFY(empty.bytes.empty());
        QCOMPARE(empty.report.at("vertices").get<unsigned>(), 0u);
        QVERIFY(!empty.report.at("enabled").get<bool>());
    }
    void mainWindowInspection() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/depth.gpa_frame";
        depthStencilCapture().save(path);
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            const auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto stage = window.findChild<QComboBox *>("geometryStage");
        stage->setCurrentIndex(1);
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto table = window.findChild<QTableView *>("geometryTable");
        QCOMPARE(table->model()->rowCount(), 3);
        QCOMPARE(table->model()->headerData(0, Qt::Horizontal).toString(), QString("vertex"));
        window.findChild<QLineEdit *>("geometryInstance")->setText("0");
        QCOMPARE(table->model()->rowCount(), 0);
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(table->model()->rowCount(), 3);
        auto mesh = window.findChild<MeshView *>("iaMesh");
        QVERIFY(mesh);
        const auto rendered = mesh->grab().toImage();
        int minX = rendered.width(), maxX = -1;
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x) {
                auto color = rendered.pixelColor(x, y);
                if (color.blue() > 180 && color.green() > 140 && color.red() < 160) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                }
            }
        QVERIFY2(maxX - minX > rendered.height() / 2, "Triangle preview must include horizontal edges");
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            dialog->setDirectory(dir.path());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>("exportGeometry")->trigger();
        for (const auto *file : {"geometry.json", "vertices.bin", "vertices.csv", "geometry.obj"})
            QVERIFY(QFile::exists(dir.path() + "/FloraGPA-Geometry-1000/" + file));
        const auto evidence = qEnvironmentVariable("FLORA_POST_TRANSFORM_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(window.grab().save(evidence + "/post-transform.png"));
        }
        stage->setCurrentIndex(stage->findData("vs-index"));
        tasks.clear();
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        auto choice = window.findChild<QComboBox *>("geometryTableChoice");
        QVERIFY(choice && choice->isEnabled());
        choice->setCurrentIndex(choice->findData("references"));
        QCOMPARE(table->model()->rowCount(), 3);
        QCOMPARE(table->model()->headerData(5, Qt::Horizontal).toString(), QString("vertex_id"));
        choice->setCurrentIndex(choice->findData("unique_vertices"));
        QCOMPARE(table->model()->rowCount(), 3);
        QCOMPARE(table->model()->headerData(0, Qt::Horizontal).toString(), QString("unique_vertex"));
        if (!evidence.isEmpty())
            QVERIFY(window.grab().save(evidence + "/vs-identities.png"));
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            dialog->setDirectory(dir.path());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>("exportGeometry")->trigger();
        for (const auto *file : {"geometry.json", "vertices.bin", "vertices.csv", "geometry.obj",
                                 "unique_vertices.bin", "unique_vertices.csv", "references.csv"})
            QVERIFY(QFile::exists(dir.path() + "/FloraGPA-Geometry-1000-1/" + file));
        stage->setCurrentIndex(0);
        QCOMPARE(table->model()->rowCount(), 0);
        QVERIFY(!window.findChild<QLineEdit *>("geometryInstance")->isEnabled());
        stage->setCurrentIndex(stage->findData("vs-index"));
        choice->setCurrentIndex(choice->findData("references"));
        window.findChild<QSpinBox *>("geometryStream")->setValue(3);
        window.findChild<QLineEdit *>("geometryInstance")->setText("2");
        const auto project = dir.path() + "/geometry.json";
        projectFile(window, "saveExperiment", project);
        QFile saved(project);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto document = nlohmann::json::parse(saved.readAll().toStdString());
        QCOMPARE(document.at("ui").at("geometry_selection").at("stage"), nlohmann::json("VS索引"));
        saved.close();
        stage->setCurrentIndex(0);
        window.findChild<QSpinBox *>("geometryStream")->setValue(0);
        window.findChild<QLineEdit *>("geometryInstance")->clear();
        tasks.clear();
        projectFile(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(stage->currentData().toString(), QString("vs-index"));
        QCOMPARE(choice->currentData().toString(), QString("references"));
        QCOMPARE(window.findChild<QSpinBox *>("geometryStream")->value(), 3);
        QCOMPARE(window.findChild<QLineEdit *>("geometryInstance")->text(), QString("2"));
        QCOMPARE(table->model()->rowCount(), 0);
        stage->setCurrentIndex(stage->findData("vs-writes"));
        window.findChild<QSpinBox *>("geometryStream")->setValue(0);
        window.findChild<QLineEdit *>("geometryInstance")->clear();
        tasks.clear();
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(table->model()->rowCount(), 3);
        QCOMPARE(table->model()->headerData(0, Qt::Horizontal).toString(), QString("record"));
        QCOMPARE(table->model()->headerData(4, Qt::Horizontal).toString(), QString("SV_Position0.x.written"));
        QVERIFY(!choice->isEnabled());
        if (!evidence.isEmpty())
            QVERIFY(window.grab().save(evidence + "/vs-writes.png"));
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            dialog->setDirectory(dir.path());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>("exportGeometry")->trigger();
        for (const auto *file : {"geometry.json", "vertices.bin", "vertices.validity.bin", "vertices.csv"})
            QVERIFY(QFile::exists(dir.path() + "/FloraGPA-Geometry-1000-2/" + file));
        QVERIFY(!QFile::exists(dir.path() + "/FloraGPA-Geometry-1000-2/geometry.obj"));
        const auto streamPath = dir.path() + "/ui-stream.gpa_frame";
        streamCapture().save(streamPath);
        loaded.clear();
        tasks.clear();
        window.openCapture(streamPath);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            const auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 150) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        stage->setCurrentIndex(stage->findData("gs-emits"));
        window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(table->model()->rowCount(), 4);
        QCOMPARE(table->model()->headerData(7, Qt::Horizontal).toString(), QString("operation"));
        if (!evidence.isEmpty())
            QVERIFY(window.grab().save(evidence + "/gs-emissions.png"));
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>();
            QVERIFY(dialog);
            dialog->setDirectory(dir.path());
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction *>("exportGeometry")->trigger();
        for (const auto *file : {"geometry.json", "vertices.bin", "vertices.validity.bin", "vertices.csv",
                                 "primitives.csv", "geometry.obj"})
            QVERIFY(QFile::exists(dir.path() + "/FloraGPA-Geometry-150/" + file));
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("FloraGPA-PostTransformTests");
    app.setApplicationName("FloraGPA-PostTransformTests");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    applyAppearance(app);
    PostTransformTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "PostTransformTests.moc"
