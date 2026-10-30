#include "NormalizedPredicateCapture.h"
#include "application/Experiment.h"
#include "application/HlslCompilation.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeComputeDebug.h"
#include "application/NativeDebugSession.h"
#include "application/NativeExecutionDebugModel.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
ReplayOptions options(uint32_t vendor, Id event = 100) {
    ReplayOptions r;
    r.until = event;
    r.before = true;
    r.warp = !vendor;
    if (vendor)
        r.vendor = vendor;
    return r;
}
Capture dispatchCapture(bool indirect, bool copy) {
    auto c = computeCapture(true);
    std::erase_if(c.entries, [](const auto &e) { return e.id == 100; });
    if (indirect) {
        c.buffer(18, 19, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, {91, 3, 2, 1});
        c.add(100, 7, 0x36, statePack(Id(17), Id(0), Id(1), Id(18), 4u));
        if (copy) {
            c.buffer(20, 21, 0, 0, {91, 4, 2, 1});
            c.add(95, 7, 0x3e, statePack(Id(0), Id(1), Id(18), Id(20)));
        }
    } else
        c.add(100, 7, 0x35, statePack(Id(17), Id(0), Id(1), 3u, 2u, 1u));
    return c;
}
Json load(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing compute debug test result");
    return Json::parse(f.readAll().toStdString());
}
QString worker() { return QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe"; }
} // namespace
class NativeComputeDebugTests : public QObject {
    Q_OBJECT
  private slots:
    void maskedSadWorkflow_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void maskedSadWorkflow() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir temporary;
            const auto retained = qEnvironmentVariable("FLORA_DXBC_MSAD_EVIDENCE");
            QDir dir(retained.isEmpty() ? temporary.path()
                                        : QDir(retained).filePath("workflow-" + QString::number(vendor)));
            QVERIFY(dir.mkpath("."));
            const auto capture = dir.filePath("frame.gpa_frame");
            computeCapture(true).save(capture);
            Frame frame(capture.toStdWString());
            const std::string source = R"(cbuffer Constants:register(b0){uint4 c;}
StructuredBuffer<uint> input:register(t0);
RWStructuredBuffer<uint> output:register(u0);
[numthreads(1,1,1)]void main(){
    uint4 scores = msad4(c.x,uint2(input[0],input[1]),uint4(1,2,3,4));
    output[0]=scores.x; output[1]=scores.y;
    output[2]=scores.z; output[3]=scores.w;
})";
            Com<ID3DBlob> compiled, errors;
            check(D3DCompile(source.data(), source.size(), "msad-workflow.hlsl", nullptr, nullptr, "main",
                             "cs_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &compiled,
                             &errors),
                  "Compile MSAD workflow");
            const auto begin = static_cast<const uint8_t *>(compiled->GetBufferPointer());
            std::vector<uint8_t> shader(begin, begin + compiled->GetBufferSize());
            Experiment project(frame);
            project.setShader(frame, 15, shader, source, "main");
            const auto experiment = dir.filePath("experiment.json");
            project.save(experiment);
            Json selection{{"gpa_event", 100}, {"max_steps", 1000}, {"max_trace_bytes", 1048576}};
            selection[vendor ? "gpu_vendor" : "warp"] = vendor ? Json(vendor) : Json(true);
            const auto job = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("worker"),
                                                      worker(), selection, experiment);
            const auto result = load(runNativeAnalysisJob(job, worker(), 30));
            validateNativeAnalysisResult(job, result);
            auto artifacts = readNativeArtifacts(dir.filePath("worker"), result.at("artifacts"), 1048576);
            NativeDebugSession session(job, result, artifacts);
            auto &model = session.model();
            QVERIFY(!model.files().empty());
            size_t selected = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                try {
                    if (model.evaluate(i, "scores.x").text() == "6")
                        selected = i;
                } catch (const std::exception &) {
                    // The local variable is unavailable before its assignment.
                }
            }
            QVERIFY(selected > 0);
            model.move(selected);
            model.addWatch("scores.x");
            const auto raw = model.registerValues(), watches = model.watchResults();
            session.exportArchive(dir.filePath("session.zip"), model.exportReport());
            NativeDebugSession restored(job, result, artifacts);
            restored.model().importConfiguration(model.configuration());
            restored.model().move(selected);
            QCOMPARE(restored.model().registerValues(), raw);
            QCOMPARE(restored.model().watchResults(), watches);
            dxbc_exec::ExecutionTraceReader trace(session.tracePath(),
                                                  result.at("artifacts").at("trace.jsonl").at("sha256"));
            trace.moveTo(trace.size());
            const auto output = trace.storage().at({dxbc_exec::TraceSpace::Buffer, 7});
            auto replayOptions = options(vendor);
            replayOptions.before = false;
            project.apply(frame, replayOptions);
            Replay replay(frame, replayOptions);
            replay.run();
            const auto original = replay.readBuffer(7);
            const std::array<uint32_t, 4> expected{6, 4, 5, 6};
            const std::vector<uint8_t> expectedBytes(reinterpret_cast<const uint8_t *>(expected.data()),
                                                     reinterpret_cast<const uint8_t *>(expected.data() + 4));
            QCOMPARE(original, expectedBytes);
            QCOMPARE(output.bytes, original);
            QCOMPARE(output.valid, std::vector<uint8_t>(16, 1));
            trace.moveTo(0);
            const std::array<uint32_t, 4> initial{10, 0, 0, 0};
            QCOMPARE(trace.storage().at({dxbc_exec::TraceSpace::Buffer, 7}).bytes,
                     std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(initial.data()),
                                          reinterpret_cast<const uint8_t *>(initial.data() + 4)));
            QProcess cli;
            QStringList arguments{"analyze",      capture,    "debug-thread", "--gpa-event",      "100",
                                  "--experiment", experiment, "--out",        dir.filePath("cli")};
            if (vendor)
                arguments << "--gpu-vendor" << QString::number(vendor);
            else
                arguments << "--warp";
            cli.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", arguments);
            QVERIFY(cli.waitForFinished(30000));
            QCOMPARE(cli.exitCode(), 0);
            const auto cliResult = load(dir.filePath("cli/result.json"));
            QCOMPARE(cliResult.at("data").at("invocation"), result.at("data").at("invocation"));
            dxbc_exec::ExecutionTraceReader cliTrace(
                dir.filePath("cli/trace.jsonl"), cliResult.at("artifacts").at("trace.jsonl").at("sha256"));
            cliTrace.moveTo(cliTrace.size());
            QCOMPARE(cliTrace.storage().at({dxbc_exec::TraceSpace::Buffer, 7}).bytes, original);
            Json report{{"vendor", vendor},
                        {"selected", selected},
                        {"watches", watches},
                        {"location", model.location()},
                        {"original_output", original},
                        {"native_output", output.bytes},
                        {"initial_output", initial},
                        {"configuration", model.configuration()}};
            QFile file(dir.filePath("workflow.json"));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto data = report.dump(2);
            QCOMPARE(file.write(data.data(), qint64(data.size())), qint64(data.size()));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void feedbackWorkflow_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void feedbackWorkflow() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir temporary;
            const auto retained = qEnvironmentVariable("FLORA_FEEDBACK_WORKER_EVIDENCE");
            QDir dir(retained.isEmpty() ? temporary.path()
                                        : QDir(retained).filePath(QString::number(vendor)));
            QVERIFY(dir.mkpath("."));
            const auto capture = dir.filePath("frame.gpa_frame");
            computeCapture(true).save(capture);
            Frame frame(capture.toStdWString());
            const std::string source =
                "StructuredBuffer<uint> input:register(t0);RWStructuredBuffer<uint> output:register(u0);"
                "[numthreads(1,1,1)]void main(){uint a,b;uint x=input.Load(1,a);uint y=output.Load(0,b);"
                "output[0]=x+y;output[1]=CheckAccessFullyMapped(a);output[2]=CheckAccessFullyMapped(b);"
                "output[3]=123;}";
            const auto shader = compileHlsl(source, "cs_5_0").bytecode;
            Experiment project(frame);
            project.setShader(frame, 15, shader, source, "main");
            const auto experiment = dir.filePath("experiment.json");
            project.save(experiment);
            Json selection{{"gpa_event", 100}, {"max_steps", 1000}, {"max_trace_bytes", 1048576}};
            selection[vendor ? "gpu_vendor" : "warp"] = vendor ? Json(vendor) : Json(true);
            const auto job = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("worker"),
                                                      worker(), selection, experiment);
            const auto result = load(runNativeAnalysisJob(job, worker(), 30));
            validateNativeAnalysisResult(job, result);
            dxbc_exec::ExecutionTraceReader trace(dir.filePath("worker/trace.jsonl"),
                                                  result.at("artifacts").at("trace.jsonl").at("sha256"));
            QCOMPARE(trace.header().at("version"), Json(2));
            QCOMPARE(trace.header().at("adapter").at("vendor"), Json(vendor ? vendor : 5140u));
            unsigned provenance = 0, statuses = 0;
            for (size_t i = 0; i < trace.observationCount(); ++i) {
                const auto row = trace.observation(i);
                if (row.at("record") == "binding" && (row.at("resource") == 4 || row.at("resource") == 7)) {
                    QCOMPARE(row.at("captured_misc_flags"), Json(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED));
                    QCOMPARE(row.at("native_misc_flags"), row.at("captured_misc_flags"));
                    ++provenance;
                }
            }
            QCOMPARE(provenance, 2u);
            for (uint64_t i = 1; i <= trace.size(); ++i) {
                trace.moveTo(i);
                for (const auto &[key, value] : trace.registers())
                    for (unsigned c = 0; c < 4; ++c)
                        if (value.mapping[c] != dxbc_exec::MappingStatus::NotStatus) {
                            QCOMPARE(value.mapping[c], dxbc_exec::MappingStatus::Mapped);
                            QCOMPARE(value.bits[c], 0u);
                            QVERIFY(!(value.valid & (1u << c)));
                            ++statuses;
                        }
            }
            QVERIFY(statuses >= 2);
            const auto &storage = trace.storage().at({dxbc_exec::TraceSpace::Buffer, 7});
            std::array<uint32_t, 4> words{};
            std::memcpy(words.data(), storage.bytes.data(), sizeof(words));
            QCOMPARE(words, (std::array<uint32_t, 4>{18, 1, 1, 123}));
            QCOMPARE(storage.valid, std::vector<uint8_t>(16, 1));
            trace.moveTo(0);
            std::memcpy(words.data(), trace.storage().at({dxbc_exec::TraceSpace::Buffer, 7}).bytes.data(),
                        sizeof(words));
            QCOMPARE(words, (std::array<uint32_t, 4>{10, 0, 0, 0}));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void doubleSourceWorkflow_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void doubleSourceWorkflow() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir temporary;
            const auto retained = qEnvironmentVariable("FLORA_DOUBLE_SOURCE_EVIDENCE");
            const auto directory =
                retained.isEmpty() ? temporary.path() : QDir(retained).filePath(QString::number(vendor));
            QVERIFY(QDir().mkpath(directory));
            QDir dir(directory);
            const auto capture = dir.filePath("frame.gpa_frame");
            computeCapture(true).save(capture);
            Frame frame(capture.toStdWString());
            const std::string source = R"(cbuffer Constants:register(b0){uint4 c;}
RWStructuredBuffer<uint> output:register(u0);
[numthreads(1,1,1)]void main(){
    double total = asdouble(1u, 0x3ff00000u);
    [loop]for(uint i=0;i<3;++i){
        total += double(c.x) * 0.25;
    }
    double2 pair = double2(total, -total);
    uint low,high;
    asuint(pair.x,low,high);
    output[0]=low; output[1]=high;
    asuint(pair.y,low,high);
    output[2]=low; output[3]=high;
})";
            Com<ID3DBlob> compiled, errors;
            check(D3DCompile(source.data(), source.size(), "double-workflow.hlsl", nullptr, nullptr, "main",
                             "cs_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &compiled,
                             &errors),
                  "Compile double source workflow");
            const auto begin = static_cast<const uint8_t *>(compiled->GetBufferPointer());
            std::vector<uint8_t> shader(begin, begin + compiled->GetBufferSize());
            Experiment project(frame);
            project.setShader(frame, 15, shader, source, "main");
            const auto experiment = dir.filePath("experiment.json");
            project.save(experiment);
            Json selection{{"gpa_event", 100}, {"max_steps", 1000}, {"max_trace_bytes", 1048576}};
            selection[vendor ? "gpu_vendor" : "warp"] = vendor ? Json(vendor) : Json(true);
            const auto job = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("worker"),
                                                      worker(), selection, experiment);
            const auto result = load(runNativeAnalysisJob(job, worker(), 30));
            validateNativeAnalysisResult(job, result);
            auto artifacts = readNativeArtifacts(dir.filePath("worker"), result.at("artifacts"), 1048576);
            NativeDebugSession session(job, result, artifacts);
            auto &model = session.model();
            QVERIFY(!model.files().empty());
            QCOMPARE(model.result().at("source_variables").at("status"), Json("available"));
            std::set<uint64_t> totals;
            Json observations = Json::array();
            size_t selected = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                model.move(i);
                for (const auto &value : model.sourceValues()) {
                    if (value.at("name") != "total" || value.at("status") != "available")
                        continue;
                    QCOMPARE(value.at("type"), Json("double"));
                    const auto bits = value.at("bits").get<uint64_t>();
                    const auto evaluated = model.evaluate(i, "total");
                    QCOMPARE(evaluated.kind, std::string("double"));
                    QCOMPARE(std::bit_cast<uint64_t>(std::get<double>(evaluated.scalar())), bits);
                    const auto expected = std::bit_cast<double>(bits) + 0.25;
                    QCOMPARE(std::get<double>(model.evaluate(i, "total + double(0.25)").scalar()), expected);
                    totals.insert(bits);
                    observations.push_back({{"position", i},
                                            {"source", value},
                                            {"watch", evaluated.text()},
                                            {"location", model.location()}});
                    if (bits == 0x3ff0000000000001ull)
                        selected = i;
                }
            }
            QVERIFY(totals.contains(0x3ff0000000000001ull));
            QVERIFY(totals.contains(0x3ff8000000000001ull));
            QVERIFY(totals.contains(0x4000000000000000ull));
            QVERIFY(totals.contains(0x4004000000000000ull));
            QVERIFY(selected > 0);
            model.move(selected);
            const auto raw = model.registerValues();
            const auto display = model.registerRows("double");
            model.addWatch("total");
            QCOMPARE(model.watchResults()[0].at("value"), Json("1.0000000000000002"));
            model.toggleInstruction(model.rows()[selected].at("instruction"));
            model.move(0);
            QCOMPARE(model.seek(0), selected);
            model.toggleInstruction(model.rows()[selected].at("instruction"));
            model.source().setRule(0, 6, "total >= double(2)", "always", 1);
            const auto conditional = model.seek(0);
            QCOMPARE(std::get<double>(model.evaluate(conditional, "total").scalar()), 2.0);
            QCOMPARE(model.source().encounterCount(conditional, {0, 6}), size_t(3));
            model.move(selected);
            auto config = model.configuration();
            model.importConfiguration(config);
            QCOMPARE(model.configuration(), config);
            session.exportArchive(dir.filePath("session.zip"), model.exportReport());
            NativeDebugSession restored(job, result, artifacts);
            restored.model().importConfiguration(config);
            restored.model().move(selected);
            QCOMPARE(restored.model().registerValues(), raw);
            QCOMPARE(restored.model().registerRows("double"), display);
            QCOMPARE(restored.model().watchResults(), model.watchResults());
            dxbc_exec::ExecutionTraceReader trace(session.tracePath(),
                                                  result.at("artifacts").at("trace.jsonl").at("sha256"));
            trace.moveTo(trace.size());
            const auto output = trace.storage().at({dxbc_exec::TraceSpace::Buffer, 7});
            auto replayOptions = options(vendor);
            replayOptions.before = false;
            project.apply(frame, replayOptions);
            Replay replay(frame, replayOptions);
            replay.run();
            const auto original = replay.readBuffer(7);
            QCOMPARE(output.bytes, original);
            QCOMPARE(output.valid, std::vector<uint8_t>(16, 1));
            const std::array<uint32_t, 4> expected{0, 0x40040000, 0, 0xc0040000};
            QCOMPARE(original, std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(expected.data()),
                                                    reinterpret_cast<const uint8_t *>(expected.data() + 4)));
            QProcess cli;
            QStringList arguments{"analyze",      capture,    "debug-thread", "--gpa-event",      "100",
                                  "--experiment", experiment, "--out",        dir.filePath("cli")};
            if (vendor)
                arguments << "--gpu-vendor" << QString::number(vendor);
            else
                arguments << "--warp";
            cli.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", arguments);
            QVERIFY(cli.waitForFinished(30000));
            QCOMPARE(cli.exitCode(), 0);
            const auto cliResult = load(dir.filePath("cli/result.json"));
            QCOMPARE(cliResult.at("data").at("invocation"), result.at("data").at("invocation"));
            dxbc_exec::ExecutionTraceReader cliTrace(
                dir.filePath("cli/trace.jsonl"), cliResult.at("artifacts").at("trace.jsonl").at("sha256"));
            cliTrace.moveTo(cliTrace.size());
            QCOMPARE(cliTrace.storage().at({dxbc_exec::TraceSpace::Buffer, 7}).bytes, original);
            Json report{{"vendor", vendor},
                        {"observations", observations},
                        {"source_values", totals},
                        {"selected", selected},
                        {"conditional_position", conditional},
                        {"conditional_total", model.evaluate(conditional, "total").text()},
                        {"conditional_encounter", 3},
                        {"original_output", original},
                        {"native_output", output.bytes},
                        {"raw_registers", raw},
                        {"double_registers", display},
                        {"configuration", config},
                        {"watch_results", model.watchResults()},
                        {"analysis_context", result.at("context")}};
            QFile reportFile(dir.filePath("workflow.json"));
            QVERIFY(reportFile.open(QIODevice::WriteOnly));
            reportFile.write(QByteArray::fromStdString(report.dump(2)));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void dispatchBoundary_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<int>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (int mode = 0; mode < 4; ++mode)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(mode))) << vendor << mode;
    }
    void dispatchBoundary() {
        QFETCH(uint32_t, vendor);
        QFETCH(int, mode);
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        dispatchCapture(mode != 0, mode >= 2).save(capture);
        Frame frame(capture.toStdWString());
        auto config = options(vendor);
        if (mode == 3)
            config.buffers[100][18] = {{4, word(2)}};
        Replay replay(frame, config);
        dxbc_exec::GroupTraceOptions selection;
        const uint32_t x = mode == 3 ? 2 : mode == 2 ? 4 : 3;
        selection.group = {x - 1, 1, 0};
        const auto result = debugNativeCompute(replay, dir.filePath("trace.jsonl"), selection, 1024);
        QCOMPARE(result.at("invocation").at("dispatch_groups"), Json({x, 2, 1}));
        QCOMPARE(result.at("invocation").at("group"), Json(selection.group));
        QCOMPARE(result.at("invocation").at("dispatch_kind"), Json(mode ? "indirect" : "direct"));
        QCOMPARE(result.at("invocation").at("predicate").at("source"), Json("unbound"));
        QVERIFY(result.at("trace").at("completed") == true);
        QCOMPARE(replay.readCounter(12), 0u);
        QCOMPARE(firstWord(replay, 7), 10u);
        if (mode) {
            const auto bytes = replay.readBuffer(18);
            QCOMPARE(Reader(Bytes(bytes).subspan(4)).read<uint32_t>(), mode >= 2 ? 4u : 3u);
        }
        // A new run must reconstruct identical inputs after diagnostic execution.
        const auto retry = debugNativeCompute(replay, dir.filePath("retry.jsonl"), selection, 1024);
        QCOMPARE(retry.at("invocation"), result.at("invocation"));
        QCOMPARE(retry.at("trace").at("steps"), result.at("trace").at("steps"));
    }
    void predication_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("visible");
        QTest::addColumn<uint32_t>("comparison");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (int kind = 0; kind < 4; ++kind)
                for (uint32_t comparison : {0u, 1u})
                    for (bool visible : {false, true}) {
                        if (kind >= 2 && visible)
                            continue;
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(kind).arg(visible).arg(comparison)))
                            << vendor << kind << visible << comparison;
                    }
    }
    void predication() {
        QFETCH(uint32_t, vendor);
        QFETCH(int, kind);
        QFETCH(bool, visible);
        QFETCH(uint32_t, comparison);
        QTemporaryDir dir;
        auto c = kind == 3 ? normalizedPredicateCapture(comparison)
                           : predicateCapture(visible, comparison, kind == 1, kind == 2);
        const auto capture = dir.filePath("frame.gpa_frame");
        c.save(capture);
        Frame frame(capture.toStdWString());
        auto config = options(vendor, 3000);
        Replay clean(frame, config);
        clean.run();
        const auto original = clean.readBuffer(7), originalAppend = clean.readBuffer(10);
        const auto originalCounter = clean.readCounter(12);
        const auto observed = clean.observeBoundPredicate();
        QCOMPARE(observed.resource, Id(600));
        QCOMPARE(observed.comparison, comparison);
        const bool expected = (kind >= 2 ? false : visible) != bool(comparison);
        if (kind == 1) {
            QVERIFY(!observed.queryValue);
            QVERIFY(!observed.allowsExecution);
        } else {
            QCOMPARE(observed.allowsExecution, std::optional<bool>(expected));
            QCOMPARE(observed.queryValue, std::optional<bool>(kind >= 2 ? false : visible));
        }
        Replay replay(frame, config);
        const auto path = dir.filePath("trace.jsonl");
        if (kind == 1 || !expected) {
            try {
                debugNativeCompute(replay, path, {}, 4096);
                QFAIL("Ineligible or unresolved invocation produced a trace");
            } catch (const std::runtime_error &error) {
                QVERIFY(QString::fromUtf8(error.what()).contains(kind == 1 ? "unknown" : "skipped"));
            }
            QVERIFY(!QFileInfo::exists(path));
        } else {
            const auto result = debugNativeCompute(replay, path, {}, 4096);
            const auto &p = result.at("invocation").at("predicate");
            QCOMPARE(p.at("source"), Json(kind == 3   ? "captured_normalized_condition_carrier"
                                          : kind == 2 ? "native_replay_initialization"
                                                      : "replayed_gpu_query"));
            QCOMPARE(p.at("original_query_recovered"), Json(false));
        }
        QCOMPARE(replay.readBuffer(7), original);
        QCOMPARE(replay.readBuffer(10), originalAppend);
        QCOMPARE(replay.readCounter(12), originalCounter);
        const auto after = replay.observeBoundPredicate();
        QCOMPARE(after.resource, observed.resource);
        QCOMPARE(after.comparison, observed.comparison);
        QCOMPARE(after.queryValue, observed.queryValue);
        QCOMPARE(after.allowsExecution, observed.allowsExecution);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, replay.observeBoundPredicate([] { return true; }));
        if (kind != 1) {
            // Validate against an independently executed original dispatch and
            // its hidden append counter side effect, not just the CPU decision.
            config.before = false;
            Replay executed(frame, config);
            executed.run();
            QCOMPARE(firstWord(executed, 7), Reader(original).read<uint32_t>() + (expected ? 9u : 0u));
            QCOMPARE(executed.readCounter(12), originalCounter + (expected ? 1u : 0u));
        }
    }
    void invalidSelection_data() {
        QTest::addColumn<QString>("failure");
        for (const auto *name : {"group", "thread", "disabled", "zero_dispatch", "oversized_dispatch",
                                 "unaligned_offset", "truncated_arguments", "input_budget", "steps", "bytes"})
            QTest::newRow(name) << QString(name);
    }
    void invalidSelection() {
        QFETCH(QString, failure);
        const bool indirect =
            failure.contains("offset") || failure.contains("arguments") || failure == "input_budget";
        auto c = dispatchCapture(indirect, false);
        for (auto &e : c.entries)
            if (e.id == 100) {
                if (failure == "zero_dispatch")
                    put(c.bytes, size_t(e.offset) + 24, 0u);
                if (failure == "oversized_dispatch")
                    put(c.bytes, size_t(e.offset) + 24, 65536u);
                if (failure == "unaligned_offset")
                    put(c.bytes, size_t(e.offset) + 32, 2u);
                if (failure == "truncated_arguments")
                    put(c.bytes, size_t(e.offset) + 32, 8u);
            }
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        c.save(capture);
        Frame frame(capture.toStdWString());
        auto config = options(0);
        if (failure == "disabled")
            config.disabled.insert(100);
        Replay replay(frame, config);
        dxbc_exec::GroupTraceOptions selection;
        if (failure == "group")
            selection.group[0] = 3;
        if (failure == "thread")
            selection.thread[0] = 1;
        if (failure == "steps")
            selection.maxSteps = 1;
        if (failure == "bytes")
            selection.maxBytes = 100;
        const auto path = dir.filePath("trace.jsonl");
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            debugNativeCompute(replay, path, selection, failure == "input_budget" ? 15 : 1024));
        QVERIFY(!QFileInfo::exists(path));
    }
    void workerAndCli_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void workerAndCli() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        dispatchCapture(true, true).save(capture);
        Frame frame(capture.toStdWString());
        Experiment project(frame);
        project.setBuffer(frame, 100, 18, 4, word(2));
        const std::string source = "RWStructuredBuffer<uint> output:register(u0);"
                                   "[numthreads(2,2,1)]void main(uint i:SV_GroupIndex){output[i]=i+23;}";
        const auto shader = compileHlsl(source, "cs_5_0").bytecode;
        project.setShader(frame, 15, shader, source, "main");
        const auto experiment = dir.filePath("experiment.json");
        project.save(experiment);
        Json selection{{"gpa_event", 100},
                       {"group", {1, 1, 0}},
                       {"thread", {1, 1, 0}},
                       {"max_steps", 1000},
                       {"max_trace_bytes", 1048576}};
        if (vendor)
            selection["gpu_vendor"] = vendor;
        else
            selection["warp"] = true;
        const auto job = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("worker"), worker(),
                                                  selection, experiment);
        const auto result = load(runNativeAnalysisJob(job, worker(), 30));
        validateNativeAnalysisResult(job, result);
        QCOMPARE(result.at("backend"), Json("dx11-native/19"));
        QCOMPARE(result.at("data").at("invocation").at("dispatch_groups"), Json({2, 2, 1}));
        QCOMPARE(result.at("data").at("invocation").at("group_size"), Json({2, 2, 1}));
        QCOMPARE(result.at("data").at("trace_file"), Json("trace.jsonl"));
        QVERIFY(result.at("artifacts").contains("trace.jsonl"));
        QCOMPARE(result.at("data").at("shader_file"), Json("shader.dxbc"));
        QVERIFY(result.at("artifacts").contains("shader.dxbc"));
        QFile shaderFile(dir.filePath("worker/shader.dxbc"));
        QVERIFY(shaderFile.open(QIODevice::ReadOnly));
        const auto shaderBytes = shaderFile.readAll();
        const Bytes effectiveShader(reinterpret_cast<const uint8_t *>(shaderBytes.constData()),
                                    shaderBytes.size());
        QCOMPARE(result.at("data").at("shader_sha256"), Json(sha256(effectiveShader)));
        NativeExecutionDebugModel model(dir.filePath("worker/trace.jsonl"),
                                        result.at("artifacts").at("trace.jsonl").at("sha256"),
                                        effectiveShader);
        QCOMPARE(model.size(), result.at("data").at("trace").at("selected_steps").get<size_t>());
        QCOMPARE(model.result().at("execution_context").at("group_size"), Json({2, 2, 1}));
        model.move(model.size());
        model.move(0);
        for (const auto &module : result.at("execution").at("loaded_modules")) {
            const auto name = QFileInfo(QString::fromStdString(module)).fileName().toLower();
            QVERIFY(!name.contains("renderdoc"));
            QVERIFY(name != "dx11_player.dll" && name != "dx11_playback.dll");
        }
        QProcess cli;
        QStringList args{
            "analyze",  capture, "debug-thread",     "--gpa-event", "100", "--group", "1",
            "1",        "0",     "--thread",         "1",           "1",   "0",       "--experiment",
            experiment, "--out", dir.filePath("cli")};
        if (vendor)
            args << "--gpu-vendor" << QString::number(vendor);
        else
            args << "--warp";
        cli.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
        QVERIFY(cli.waitForFinished(30000));
        QCOMPARE(cli.exitCode(), 0);
        const auto cliResult = load(dir.filePath("cli/result.json"));
        QCOMPARE(cliResult.at("data").at("invocation"), result.at("data").at("invocation"));
        auto stale = job;
        stale["out"] = dir.filePath("stale").toStdString();
        QVERIFY(project.undo());
        project.save(experiment);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(stale, worker(), 30));
        QVERIFY(!QFileInfo::exists(dir.filePath("stale/result.json")));
        QVERIFY(project.redo());
        project.save(experiment);
        selection["max_steps"] = 1;
        const auto bad = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("failed"), worker(),
                                                  selection, experiment);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(bad, worker(), 30));
        QVERIFY(!QFileInfo::exists(dir.filePath("failed/result.json")));
        QVERIFY(!QFileInfo::exists(dir.filePath("failed/trace.jsonl")));
        selection["max_steps"] = 1000;
        const auto retry = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("retry"), worker(),
                                                    selection, experiment);
        QVERIFY(load(runNativeAnalysisJob(retry, worker(), 30)).at("ok") == true);
    }
};
QTEST_GUILESS_MAIN(NativeComputeDebugTests)
#include "NativeComputeDebugTests.moc"
