#include "SyntheticCapture.h"
#include "application/DxbcExecutionTrace.h"
#include "application/Experiment.h"
#include "application/NativeAnalysis.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeDebugSession.h"
#include "application/NativeExecutionDebugModel.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
std::string source(const std::string &stage) {
    const std::string loop = "\n [loop]for(uint i=0;i<3;++i){\n total+=i;\n }\n";
    if (stage == "cs")
        return "RWStructuredBuffer<uint> output:register(u0);\n"
               "[numthreads(2,2,1)] void main(uint3 global:SV_DispatchThreadID,uint3 group:SV_GroupID,"
               "uint3 local:SV_GroupThreadID,uint flat:SV_GroupIndex){\n"
               " uint total=global.x+group.y+local.y+flat;" +
               loop + " output[flat]=total;\n}\n";
    if (stage == "vs")
        return "float4 main(float4 position:POSITION,uint vertex:SV_VertexID,uint instance:SV_InstanceID)"
               ":SV_Position{\n float total=position.x+vertex+instance;" +
               loop + " return float4(total,position.yzw);\n}\n";
    return "float4 main(float4 position:SV_Position,uint primitive:SV_PrimitiveID,bool front:SV_IsFrontFace,"
           "uint sample:SV_SampleIndex):SV_Target{\n float total=position.x+primitive+(front?1:0)+sample;" +
           loop + " return float4(total,position.yzw);\n}\n";
}
void save(const QString &path, Bytes bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot save SDBG execution evidence");
}
void save(const QString &path, const std::string &text) {
    save(path, Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
std::vector<uint8_t> compile(const std::string &stage, const std::string &text) {
    struct Library {
        HMODULE module = LoadLibraryExW(L"d3dcompiler_43.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ~Library() {
            if (module)
                FreeLibrary(module);
        }
    } library;
    if (!library.module)
        throw std::runtime_error("SDBG execution fixtures require the legacy Microsoft compiler");
    const auto fn = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library.module, "D3DCompile"));
    if (!fn)
        throw std::runtime_error("Legacy Microsoft compiler entry point is missing");
    Com<ID3DBlob> code, errors;
    const auto profile = stage + "_5_0", filename = stage + ".hlsl";
    const auto hr = fn(text.data(), text.size(), filename.c_str(), nullptr, nullptr, "main", profile.c_str(),
                       D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                        : "Compile failed");
    const auto first = static_cast<const uint8_t *>(code->GetBufferPointer());
    return {first, first + code->GetBufferSize()};
}
} // namespace
class NativeSdbgExecutionTests final : public QObject {
    Q_OBJECT
  private slots:
    void stageSources_data() {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"vs", "ps", "cs"})
            QTest::newRow(stage) << QString(stage);
    }
    void stageSources() {
        QFETCH(QString, stage);
        QTemporaryDir temporary;
        const auto root = qEnvironmentVariable("FLORA_SDBG_EXECUTION_EVIDENCE", temporary.path());
        QVERIFY(QDir().mkpath(root));
        const auto stem = QDir(root).filePath(stage);
        try {
            InputSnapshot input;
            input.stage = stage == "cs" ? 5 : stage == "vs" ? 0 : 4;
            input.event = 100;
            input.shader = 15;
            const auto text = source(stage.toStdString());
            input.bytecode = compile(stage.toStdString(), text);
            save(stem + ".hlsl", text);
            save(stem + ".dxbc", input.bytecode);
            input.inputs.registers[{1, {0}}] =
                Value::known({std::bit_cast<uint32_t>(2.5f), 0, 0, 0x3f800000});
            input.inputs.registers[{1, {1}}] =
                Value::known(stage == "vs" ? std::array<uint32_t, 4>{7, 0, 0, 0}
                                           : std::array<uint32_t, 4>{7, UINT32_MAX, 2, 0});
            input.inputs.registers[{1, {2}}] = Value::known({3, 0, 0, 0});
            const auto path = stem + ".jsonl";
            if (stage == "cs") {
                input.inputs.memory = std::make_shared<Memory>(
                    std::map<uint64_t, Buffer>{{7, Buffer::known(std::vector<uint8_t>(16))}},
                    std::map<Register, BufferView>{{{30, {0}}, {7, 0, 16, 4}}});
                GroupTraceOptions options;
                options.group = {2, 3, 0};
                options.thread = {1, 1, 0};
                exportGroupExecutionTrace(path, input, options);
            } else if (stage == "vs")
                exportVertexExecutionTrace(path, input);
            else {
                PixelTraceOptions options;
                for (auto &registers : options.registers)
                    registers = input.inputs.registers;
                input.inputs.registers.clear();
                exportPixelExecutionTrace(path, input, options);
            }
            NativeExecutionDebugModel model(path, analysisFileDigest(path), input.bytecode);
            save(stem + "-model.json", model.result().dump(2));
            QCOMPARE(model.result().at("source_variables").at("format"), Json("SDBG assignments"));
            QCOMPARE(model.result().at("source_variables").at("status"), Json("available"));
            const std::map<std::string, std::string> expectedInputs =
                stage == "cs" ? std::map<std::string, std::string>{{"global.x", "5"},
                                                                   {"group.y", "3"},
                                                                   {"local.y", "1"},
                                                                   {"flat", "3"}}
                : stage == "vs"
                    ? std::map<std::string, std::string>{{"position.x", "2.5"},
                                                         {"vertex", "7"},
                                                         {"instance", "3"}}
                    : std::map<std::string, std::string>{
                          {"position.x", "2.5"}, {"primitive", "7"}, {"front", "true"}, {"sample", "2"}};
            for (const auto &[name, value] : expectedInputs)
                QCOMPARE(model.evaluate(0, name).text(), value);
            if (stage == "cs") {
                QVERIFY_THROWS_EXCEPTION(ExpressionError, model.evaluate(0, "global.y"));
                QVERIFY_THROWS_EXCEPTION(ExpressionError, model.evaluate(0, "output"));
            }
            Json observations = Json::array();
            std::vector<std::string> totals;
            size_t last = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                model.move(i);
                const auto values = model.sourceValues();
                observations.push_back({{"step", i}, {"location", model.location()}, {"values", values}});
                for (const auto &v : values)
                    if (v.at("name") == "total" && v.at("status") == "available") {
                        const auto value = model.evaluate(i, "total").text();
                        if (totals.empty() || totals.back() != value)
                            totals.push_back(value);
                        last = i;
                    }
            }
            save(stem + "-values.json", observations.dump(2));
            QCOMPARE(totals, stage == "cs" ? std::vector<std::string>({"12", "13", "15"})
                                           : std::vector<std::string>({"12.5", "13.5", "15.5"}));
            model.move(last);
            model.addWatch("total + 1");
            QCOMPARE(model.evaluate(last, "total + 1").text(),
                     stage == "cs" ? std::string("16") : std::string("16.5"));
            const auto registers = model.registerValues();
            model.move(0);
            model.move(last);
            QCOMPARE(model.registerValues(), registers);
            for (size_t i = model.size(); i-- > 0;) {
                model.move(i);
                QCOMPARE(model.sourceValues(), observations.at(i).at("values"));
            }
            const SourceTrace::Point body{0, stage == "cs" ? 5 : 4};
            std::vector<size_t> encounters;
            for (size_t i = 0; i < model.size(); ++i)
                if (model.source().lineEntry(i, body))
                    encounters.push_back(i);
            QCOMPARE(encounters.size(), size_t(3));
            model.source().setRule(body.first, body.second, "total >= 0", "equal", 2);
            QCOMPARE(model.seek(0), encounters.at(1));
            model.source().setRule(body.first, body.second, stage == "cs" ? "total == 13" : "total == 13.5");
            QCOMPARE(model.seek(0), encounters.at(2));
            const auto next = model.source().next(0, 1);
            QVERIFY(next > 0 && next < model.size());
            QCOMPARE(model.source().next(next, -1), next - 1);
            model.move(last);
            const auto config = model.configuration();
            model.importConfiguration(config);
            QCOMPARE(model.configuration(), config);
            save(stem + "-config.json", config.dump(2));
            model.move(model.size());
            if (stage == "cs") {
                const auto &buffer = input.inputs.memory->buffers().at(7);
                std::array<uint32_t, 4> words{};
                std::memcpy(words.data(), buffer.bytes.data(), 16);
                QCOMPARE(words, (std::array<uint32_t, 4>{10, 12, 13, 15}));
            } else {
                bool output = false;
                for (const auto &r : model.registerValues())
                    if (r.at("name") == "o0") {
                        output = true;
                        QCOMPARE(r.at("bits").at(0), Json(std::bit_cast<uint32_t>(15.5f)));
                        QCOMPARE(r.at("valid_components"), Json(15));
                    }
                QVERIFY(output);
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void metadataGuards() {
        const auto shader = compile("cs", source("cs"));
        const auto original = readDxbcParts(shader);
        auto get = [](Bytes bytes, size_t offset) {
            uint32_t word = 0;
            std::memcpy(&word, bytes.data() + offset, 4);
            return word;
        };
        auto put = [](std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
            std::memcpy(bytes.data() + offset, &value, 4);
        };
        for (unsigned mode = 0; mode < 4; ++mode) {
            auto parts = original;
            const auto tag = mode == 0 ? 0x58454853u : 0x47424453u;
            const auto index = size_t(
                std::find_if(parts.begin(), parts.end(), [&](const auto &p) { return p.first == tag; }) -
                parts.begin());
            QVERIFY(index < parts.size());
            std::vector<uint8_t> changed(parts[index].second.begin(), parts[index].second.end());
            if (mode == 0)
                put(changed, 0, 0x10050); // Executable stage disagrees with the debug environment.
            else if (mode == 1) {
                const auto input = 84 + get(changed, 12 * 4);
                put(changed, input + 3 * 4, 0x3ffffff0); // Unverified system register stays unavailable.
            } else {
                const auto instructions = 84 + get(changed, 8 * 4), count = get(changed, 7 * 4);
                bool found = false;
                for (uint32_t i = 0; i < count; ++i) {
                    const auto at = instructions + i * 396;
                    if (get(changed, at + 4) == 168) {
                        found = true;
                        put(changed, at + (mode == 2 ? 2 : 10) * 4, mode == 2 ? 2u : UINT32_MAX - 1);
                    }
                }
                QVERIFY(found);
            }
            parts[index].second = changed;
            const auto symbols = sdbgVariables(makeDxbc(parts));
            if (mode == 1) {
                QCOMPARE(symbols.at("status"), Json("available"));
                QCOMPARE(symbols.at("initial").at(0).at("kind"), Json("unavailable"));
            } else {
                QCOMPARE(symbols.at("status"), Json("invalid_or_unsupported_symbols"));
                QVERIFY(!symbols.at("issues").empty());
            }
        }
    }
    void capturedSources_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void capturedSources() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir temporary;
        const auto root = qEnvironmentVariable("FLORA_SDBG_EXECUTION_EVIDENCE", temporary.path());
        const auto folder = QDir(root).filePath(QString("captured-%1").arg(vendor));
        QVERIFY(QDir().mkpath(folder));
        const QDir dir(folder);
        try {
            const auto capture = dir.filePath("frame.gpa_frame");
            testing::computeCapture(true).save(capture);
            Frame frame(capture.toStdWString());
            const auto text = source("cs");
            const auto code = compile("cs", text);
            Experiment project(frame);
            project.setShader(frame, 15, code, text, "main");
            const auto experiment = dir.filePath("experiment.json");
            project.save(experiment);
            const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
            Json options{{"gpa_event", 100}, {"thread", {1, 1, 0}}, {"max_steps", 1000}};
            options[vendor ? "gpu_vendor" : "warp"] = vendor ? Json(vendor) : Json(true);
            const auto job = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("worker"), worker,
                                                      options, experiment);
            const auto report = runNativeAnalysisJob(job, worker, 30);
            QFile file(report);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto result = Json::parse(file.readAll().toStdString());
            validateNativeAnalysisResult(job, result);
            const auto artifacts =
                readNativeArtifacts(dir.filePath("worker"), result.at("artifacts"), 4 * 1024 * 1024);
            NativeDebugSession session(job, result, artifacts);
            auto &model = session.model();
            QCOMPARE(model.result().at("source_variables").at("status"), Json("available"));
            QCOMPARE(model.evaluate(0, "global.x").text(), std::string("1"));
            QCOMPARE(model.evaluate(0, "group.y").text(), std::string("0"));
            std::set<std::string> totals;
            size_t last = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                model.move(i);
                for (const auto &value : model.sourceValues())
                    if (value.at("name") == "total" && value.at("status") == "available") {
                        totals.insert(model.evaluate(i, "total").text());
                        last = i;
                    }
            }
            QCOMPARE(totals, (std::set<std::string>{"5", "6", "8"}));
            model.move(last);
            model.addWatch("total");
            QCOMPARE(model.watchResults()[0].at("value"), Json("8"));
            session.exportArchive(dir.filePath("session.zip"), model.exportReport());
            ExecutionTraceReader trace(session.tracePath(),
                                       result.at("artifacts").at("trace.jsonl").at("sha256"));
            trace.moveTo(trace.size());
            const auto &output = trace.storage().at({TraceSpace::Buffer, 7});
            ReplayOptions replayOptions;
            replayOptions.until = 100;
            replayOptions.warp = !vendor;
            if (vendor)
                replayOptions.vendor = vendor;
            project.apply(frame, replayOptions);
            Replay replay(frame, replayOptions);
            replay.run();
            const auto original = replay.readBuffer(7);
            save(dir.filePath("original-output.bin"), original);
            QCOMPARE(output.bytes, original);
            QCOMPARE(output.valid, std::vector<uint8_t>(16, 1));
            std::array<uint32_t, 4> words{};
            std::memcpy(words.data(), original.data(), 16);
            QCOMPARE(words, (std::array<uint32_t, 4>{3, 5, 6, 8}));
            save(dir.filePath("model.json"), model.exportReport().dump(2));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativeSdbgExecutionTests)
#include "NativeSdbgExecutionTests.moc"
