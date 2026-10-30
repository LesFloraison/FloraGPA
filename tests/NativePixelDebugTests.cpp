#include "BlendCapture.h"
#include "application/Experiment.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeDebugSession.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
QString worker() { return QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe"; }
Json load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot load pixel debug result");
    return Json::parse(file.readAll().toStdString());
}
Json options() {
    return {{"gpa_event", 100}, {"debug_scope", "private-primitive"},
            {"resource", 1000}, {"primitive", 1},
            {"warp", true},     {"debug_device", true}};
}
void seal(Json &result, NativeArtifactBytes &artifacts) {
    artifacts["debug.json"] = QByteArray::fromStdString(result.at("data").dump(2));
    for (auto it = artifacts.begin(); it != artifacts.end(); ++it)
        result["artifacts"][it.key().toStdString()] = {
            {"bytes", it.value().size()},
            {"sha256",
             sha256(Bytes(reinterpret_cast<const uint8_t *>(it.value().data()), size_t(it.value().size())))}};
}
void preserve(const QString &directory) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_DEBUG_EVIDENCE");
    if (root.isEmpty())
        return;
    const auto target = root + "/" + QString::fromLatin1(QTest::currentDataTag());
    if (QDir(target).exists() || !QDir().mkpath(target))
        throw std::runtime_error("Pixel debug evidence directory must be new");
    for (const auto &name : QDir(directory).entryList(QDir::Files))
        if (!QFile::copy(QDir(directory).filePath(name), QDir(target).filePath(name)))
            throw std::runtime_error("Cannot retain pixel debug evidence");
}
} // namespace
class NativePixelDebugTests final : public QObject {
    Q_OBJECT
  private slots:
    void validation() {
        QCOMPARE(nativeAnalysisRequest("debug-pixel", options()).action, std::string("debug-pixel"));
        for (const auto &patch : {Json{{"debug_scope", "original"}}, Json{{"primitive", -1}},
                                  Json{{"max_steps", 0}}, Json{{"resource", 0}}}) {
            auto value = options();
            value.update(patch);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("debug-pixel", value));
        }
        for (const auto *key : {"primitive", "resource", "debug_scope"}) {
            auto value = options();
            value.erase(key);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("debug-pixel", value));
        }
    }
    void execution_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode :
                 {"constant", "derivative", "sample", "discard", "source", "uav", "eval-centroid",
                  "eval-sample", "eval-snapped", "eval-centroid-min16", "eval-sample-min16",
                  "eval-snapped-min16", "eval-centroid-min10", "eval-sample-min10", "eval-snapped-min10"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void execution() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            QTemporaryDir dir;
            const auto capture = dir.filePath("frame.gpa_frame");
            auto fixture =
                blendCapture(1, false, false, mode == "sample" || mode.startsWith("eval-sample") ? 4 : 1);
            for (const auto &entry : fixture.entries)
                if (entry.id == 100)
                    put(fixture.bytes, size_t(entry.offset) + 24, 9u);
            if (mode == "uav") {
                fixture.buffer(300, 301, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                               {});
                fixture.uav(302, 300, 0);
                std::erase_if(fixture.entries, [](const auto &e) { return e.id == 90 || e.id == 190; });
                State state{};
                state.topology = 4;
                state.sampleMask = UINT32_MAX;
                state.viewports = 50;
                state.rasterizer = 52;
                state.blend = 64;
                state.stages[0].shader = 10;
                state.stages[4].shader = 12;
                state.rtv[0] = 1002;
                state.rtv[1] = 302;
                state.omStart = 1;
                state.rtCount = 2;
                fixture.add(90, 3, 3, snapshot(state));
                fixture.add(190, 3, 3, snapshot(state));
            }
            fixture.save(capture);
            Frame frame(capture.toStdWString());
            Experiment experiment(frame);
            const std::string type = mode.endsWith("min16")   ? "min16float4"
                                     : mode.endsWith("min10") ? "min10float4"
                                                              : "float4";
            const std::string vs =
                mode.startsWith("eval-")
                    ? "struct O{float4 p:SV_Position;" + type +
                          " a:DATA;};O main(uint v:SV_VertexID){"
                          "uint i=v%3;O o;o.p=float4(i==2?3:-1,i==1?3:-1,0,1);o.a=" +
                          (mode.endsWith("min10") ? "float4(.125,.375,-.625,.8125)" : "float4(1,7,3,.5)") +
                          ";return o;}"
                    : "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;"
                      "return float4(i==2?3:-1,i==1?3:-1,0,1);}";
            experiment.setShader(frame, 10, compileClassProgram(vs, "vs_5_0"), vs, "main");
            std::string ps = "float4 main(float4 p:SV_Position,uint id:SV_PrimitiveID";
            if (mode == "sample")
                ps += ",uint sample:SV_SampleIndex";
            ps += "):SV_Target{\n";
            if (mode == "uav") {
                ps = "RWStructuredBuffer<uint> target:register(u1);" + ps;
                ps += "uint old;InterlockedAdd(target[0],1,old);\n";
            }
            if (mode == "discard")
                ps += "if(id==1)discard;\n";
            ps += "return float4(id," + std::string(mode == "sample" ? "sample" : "7") + "," +
                  (mode == "derivative" ? "ddx(p.x)" : "3") + ",.5);\n}";
            if (mode.startsWith("eval-")) {
                const std::string expression =
                    mode.startsWith("eval-centroid") ? "EvaluateAttributeCentroid(a)"
                    : mode.startsWith("eval-sample") ? "EvaluateAttributeAtSample(a,uint(p.x)+2)"
                                                     : "EvaluateAttributeSnapped(a,int2(p.xy)*7)";
                ps = "float4 main(float4 p:SV_Position,linear " + type +
                     " a:DATA,uint id:SV_PrimitiveID)"
                     ":SV_Target{" +
                     type + " value=" + expression +
                     (type == "float4" ? ";value.x=float(id);return value;}"
                                       : ";float4 output=float4(value);output.x=float(id);return output;}");
            }
            auto code = compileClassProgram(ps, "ps_5_0");
            if (mode == "source") {
                Com<ID3DBlob> binary, errors;
                check(D3DCompile(ps.data(), ps.size(), "native_pixel_test.hlsl", nullptr, nullptr, "main",
                                 "ps_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &binary,
                                 &errors),
                      "Compile pixel source fixture");
                const auto *begin = static_cast<const uint8_t *>(binary->GetBufferPointer());
                code.assign(begin, begin + binary->GetBufferSize());
            }
            experiment.setShader(frame, 12, code, ps, "main");
            const auto project = dir.filePath("experiment.json");
            experiment.save(project);
            auto selection = options();
            if (vendor) {
                selection.erase("warp");
                selection["gpu_vendor"] = vendor;
            }
            if (mode == "sample" || mode.startsWith("eval-sample"))
                selection["sample"] = 2;
            const auto output = dir.filePath("out");
            const auto job =
                prepareNativeAnalysisJob(capture, "debug-pixel", output, worker(), selection, project);
            auto result = load(runNativeAnalysisJob(job, worker(), 60));
            auto artifacts = readNativeArtifacts(output, result.at("artifacts"), 256ull * 1024 * 1024);
            NativeDebugSession session(job, result, artifacts);
            const auto &data = result.at("data");
            QCOMPARE(data.at("invocation").at("status"), Json("explicit_private_primitive"));
            QVERIFY(data.at("invocation").at("original_fragment_eligibility").is_null());
            QCOMPARE(data.at("private_history").at("observed_storage_restored"), Json(true));
            dxbc_exec::ExecutionTraceReader reader(output + "/trace.jsonl",
                                                   result.at("artifacts").at("trace.jsonl").at("sha256"));
            const auto initial = reader.registers();
            reader.moveTo(reader.size());
            if (mode.startsWith("eval-")) {
                const auto observations = reader.completion().at("attribute_evaluations");
                QCOMPARE(observations.size(), size_t(mode.startsWith("eval-centroid") ? 1
                                                     : mode.startsWith("eval-sample") ? 2
                                                                                      : 4));
                for (const auto &observation : observations)
                    QCOMPARE(observation.at("backend"), Json("dxbc-pixel-attribute-evaluation/2"));
                QVERIFY(QFile::copy(capture, output + "/frame.gpa_frame"));
                QVERIFY(QFile::copy(project, output + "/experiment.json"));
            }
            const auto lane = reader.header().at("selected_lane").get<uint32_t>();
            if (mode != "discard") {
                const auto value = reader.registerValue(lane, {2, {0}});
                const auto bytes =
                    statePack(1.f, mode == "sample" ? 2.f : 7.f, mode == "derivative" ? 1.f : 3.f, .5f);
                std::array<uint32_t, 4> expected{};
                std::memcpy(expected.data(), bytes.data(), bytes.size());
                if (mode.startsWith("eval-")) {
                    // Check the compiled shader against actual GPU storage. In
                    // particular, this compiler's snapped lowering can insert
                    // a masked MOV whose component mapping differs from a
                    // source-level constant prediction.
                    const auto &records = data.at("private_history").at("records");
                    const auto record = std::find_if(records.begin(), records.end(), [](const auto &row) {
                        return row.at("primitive_ordinal") == 1;
                    });
                    QVERIFY(record != records.end());
                    const auto &gpu = record->at("after");
                    QCOMPARE(gpu.at("exact_storage_bits"), Json(true));
                    const auto observed = gpu.at("bytes").get<std::vector<uint8_t>>();
                    QCOMPARE(observed.size(), sizeof(expected));
                    QCOMPARE(gpu.at("valid_bytes"), Json(std::vector<bool>(16, true)));
                    std::memcpy(expected.data(), observed.data(), observed.size());
                }
                if (value != dxbc_exec::Value::known(expected)) {
                    qWarning().noquote() << QString::fromStdString(Json{
                        {"actual_bits", value.bits},
                        {"valid_mask", value.valid},
                        {"expected_bits", expected}}.dump());
                    preserve(output);
                }
                QCOMPARE(value, dxbc_exec::Value::known(expected));
            } else
                QVERIFY((reader.completion().at("final_active_mask").get<uint32_t>() & (1u << lane)) == 0);
            reader.moveTo(0);
            QVERIFY(reader.registers() == initial);
            if (mode == "uav") {
                const auto initialStorage = reader.storage();
                const auto &before = initialStorage.at({dxbc_exec::TraceSpace::Buffer, 300}).bytes;
                QCOMPARE(Reader(before).read<uint32_t>(), 1u);
                reader.moveTo(reader.size());
                QCOMPARE(
                    Reader(reader.storage().at({dxbc_exec::TraceSpace::Buffer, 300}).bytes).read<uint32_t>(),
                    2u);
                reader.moveTo(0);
                QCOMPARE(reader.storage().at({dxbc_exec::TraceSpace::Buffer, 300}).bytes, before);
                const auto &last =
                    data.at("private_history").at("records").at(2).at("private_shader_outputs").at(0);
                QCOMPARE(last.at("private_memory_changes"), Json(1));
            }
            auto &model = session.model();
            QVERIFY(model.size() > 0);
            const auto initialValues = model.registerValues();
            model.move(model.size());
            QCOMPARE(model.instructionState().at("state_boundary"), Json("terminal_quad"));
            model.move(0);
            QCOMPARE(model.registerValues(), initialValues);
            model.addWatch("o0.x");
            model.toggleInstruction(model.rows().at(0).at("instruction"));
            const auto configuration = model.configuration();
            model.importConfiguration(configuration);
            QCOMPARE(model.configuration(), configuration);
            if (mode == "source")
                QVERIFY(!model.files().empty());
            session.exportArchive(dir.filePath("session.zip"), configuration);
            QVERIFY(QFileInfo(dir.filePath("session.zip")).size() > 0);
            for (const auto *change :
                 {"scope", "ordinal", "sample", "lane", "eligibility", "resource-boundary"}) {
                auto corrupt = result;
                auto bytes = artifacts;
                auto &invocation = corrupt["data"]["invocation"];
                if (std::string(change) == "scope")
                    invocation["status"] = "eligible_in_observed_replay";
                if (std::string(change) == "ordinal")
                    invocation["primitive_ordinal"] = 0;
                if (std::string(change) == "sample")
                    invocation["selection"]["sample"] = 99;
                if (std::string(change) == "lane")
                    invocation["selected_lane"] = 99;
                if (std::string(change) == "eligibility")
                    invocation["original_fragment_eligibility"] = true;
                if (std::string(change) == "resource-boundary")
                    invocation["quad_input_provenance"]["resource_boundary"] = "original";
                seal(corrupt, bytes);
                QVERIFY_THROWS_EXCEPTION(std::exception, NativeDebugSession(job, corrupt, bytes));
            }
            preserve(output);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void failureAndCli() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        blendCapture().save(capture);
        unsigned failure = 0;
        for (const auto &patch :
             {Json{{"primitive", 9}}, Json{{"max_trace_bytes", 16}}, Json{{"max_steps", 1}}}) {
            auto selection = options();
            selection.update(patch);
            const auto output = dir.filePath(QString::number(failure++));
            const auto job = prepareNativeAnalysisJob(capture, "debug-pixel", output, worker(), selection);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(job, worker(), 30));
            QVERIFY(!QFile::exists(output + "/result.json"));
        }
        QProcess cli;
        cli.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                  {"analyze", capture, "debug-pixel", "--debug-scope", "private-primitive", "--primitive",
                   "0", "--resource", "1000", "--gpa-event", "100", "--warp", "--out", dir.filePath("cli")});
        QVERIFY(cli.waitForFinished(30000));
        QVERIFY2(cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0,
                 cli.readAllStandardError().constData());
        const auto result = load(dir.filePath("cli/result.json"));
        auto artifacts =
            readNativeArtifacts(dir.filePath("cli"), result.at("artifacts"), 256ull * 1024 * 1024);
        NativeDebugSession session(load(dir.filePath("cli/job.json")), result, artifacts);
        QVERIFY(session.model().size() > 0);
    }
};
QTEST_GUILESS_MAIN(NativePixelDebugTests)
#include "NativePixelDebugTests.moc"
