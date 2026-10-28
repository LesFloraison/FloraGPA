#include "ClassCapture.h"
#include "application/Experiment.h"
#include "application/HlslCompilation.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeDebugSession.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
Capture fixture(bool created, uint32_t mode) {
    auto c = classCapture(created);
    std::erase_if(c.entries,
                  [](const auto &e) { return e.id == 16 || e.id == 62 || e.id == 64 || e.id == 80; });
    const std::array<std::string, 3> types{"ByteAddressBuffer", "StructuredBuffer<uint4>", "Buffer<uint>"};
    const std::array<std::string, 3> bodies{"uint n;data.GetDimensions(n);return data.Load(i*4)+n;",
                                            "uint n,s;data.GetDimensions(n,s);return data[i].z+n+s;",
                                            "uint n;data.GetDimensions(n);return data[i]+n;"};
    auto other = bodies[mode];
    other.insert(other.find("return ") + 7, "100+");
    const auto source =
        "interface I{uint get(uint i);};class A:I{" + types[mode] + " data;uint get(uint i){" + bodies[mode] +
        "}};class B:I{" + types[mode] + " data;uint get(uint i){" + other +
        "}};A first[2];B second[2];I selected;"
        "RWStructuredBuffer<uint> dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=selected.get(2);}";
    const auto code = compileHlsl(source, "cs_5_0").bytecode;
    auto shader = statePack(uint64_t(code.size()));
    shader.insert(shader.end(), code.begin(), code.end());
    append(shader, Id(0));
    c.add(16, 9, 0x81, shader);
    const uint32_t slot = created ? 6 : 3;
    auto names = statePack(created ? 0u : 7u);
    if (!created)
        for (char ch : std::string("second\0", 7))
            names.push_back(ch);
    append(names, created ? 2u : 0u);
    if (created) {
        names.push_back('B');
        names.push_back(0);
    }
    c.add(64, 9, 0x89, names);
    c.add(62, 5, 0x98, statePack(Id(0), Id(60), 0u, 1u, 0u, 0u, 0u, slot, 0u, uint32_t(created), Id(64)));
    const D3D11_BUFFER_DESC bd{128,
                               D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE,
                               0,
                               mode == 0   ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                               : mode == 1 ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                           : 0u,
                               mode == 1 ? 16u : 0u};
    c.add(200, 5, 0x83, statePack(Id(0), Id(0), bd, Id(201)));
    auto data = word(128);
    for (uint32_t i = 0; i < 32; ++i)
        append(data, 100 + i);
    c.add(201, 9, 1, data);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = mode == 0 ? DXGI_FORMAT_R32_TYPELESS : mode == 1 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
    vd.ViewDimension = mode == 0 ? D3D11_SRV_DIMENSION_BUFFEREX : D3D11_SRV_DIMENSION_BUFFER;
    vd.BufferEx = {mode == 0 ? 8u : 2u, 5, mode == 0 ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0u};
    c.add(202, 5, 0x8c, statePack(Id(0), Id(0), Id(200), vd));
    State state{};
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[5].shader = 15;
    state.stages[5].classes[0] = 62;
    state.stages[5].classCount = 1;
    state.stages[5].srv[slot] = 202;
    state.csUav[0] = 9;
    state.csCount = 1;
    c.add(80, 3, 3, snapshot(state));
    return c;
}
Json load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing class workflow artifact");
    return Json::parse(file.readAll().toStdString());
}
void save(const QString &path, const Json &value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Class workflow evidence must use a new path");
    const auto bytes = QByteArray::fromStdString(value.dump(2));
    if (file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot save class workflow evidence");
}
} // namespace
class NativeClassResourceTests final : public QObject {
    Q_OBJECT
  private slots:
    void workflow_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("created");
        QTest::addColumn<uint32_t>("mode");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto created : {false, true})
                for (auto mode : {0u, 1u, 2u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(created).arg(mode)))
                        << vendor << created << mode;
    }
    void workflow() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, created);
        QFETCH(uint32_t, mode);
        try {
            QTemporaryDir temporary;
            const auto retained = qEnvironmentVariable("FLORA_CLASS_WORKFLOW_EVIDENCE");
            const auto path =
                retained.isEmpty() ? temporary.path() : QDir(retained).filePath(QTest::currentDataTag());
            QVERIFY(QDir().mkpath(path));
            QDir dir(path);
            const auto capture = dir.filePath("frame.gpa_frame");
            fixture(created, mode).save(capture);
            Frame frame(capture.toStdWString());
            const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
            Json selection{{"gpa_event", 100}, {"max_steps", 1000}, {"max_trace_bytes", 1048576}};
            selection[vendor ? "gpu_vendor" : "warp"] = vendor ? Json(vendor) : Json(true);
            const auto job =
                prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("base"), worker, selection);
            const auto result = load(runNativeAnalysisJob(job, worker, 30));
            validateNativeAnalysisResult(job, result);
            NativeDebugSession session(
                job, result, readNativeArtifacts(dir.filePath("base"), result.at("artifacts"), 1048576));
            ExecutionTraceReader reader(session.tracePath(),
                                        result.at("artifacts").at("trace.jsonl").at("sha256"));
            const auto classes = reader.header().at("class_bindings");
            QCOMPARE(classes.size(), size_t(1));
            QCOMPARE(classes[0].at("object"), Json(62));
            QCOMPARE(classes[0].at("this_bits"), Json({0, 0, created ? 6 : 3, 0}));
            QCOMPARE(classes[0].at("this_valid"), Json(created ? 15 : 4));
            const auto initial = reader.storage();
            reader.moveTo(reader.size());
            const auto final = reader.storage();
            const uint32_t expected = mode == 0 ? 230 : mode == 1 ? 239 : 209;
            QCOMPARE(Reader(final.at({TraceSpace::Buffer, 7}).bytes).read<uint32_t>(), expected);
            QCOMPARE(final.at({TraceSpace::Buffer, 7}).valid, std::vector<uint8_t>(16, 1));
            QCOMPARE(final.at({TraceSpace::Buffer, 200}).bytes, initial.at({TraceSpace::Buffer, 200}).bytes);
            reader.moveTo(0);
            QCOMPARE(reader.storage().at({TraceSpace::Buffer, 7}).bytes,
                     initial.at({TraceSpace::Buffer, 7}).bytes);
            auto &model = session.model();
            size_t inside = model.size();
            for (size_t i = 0; i < model.size(); ++i)
                if (model.rows()[i].at("call_depth") == 1) {
                    inside = i;
                    break;
                }
            QVERIFY(inside < model.size());
            model.toggleInstruction(model.rows()[inside].at("instruction"));
            QCOMPARE(model.seek(0), inside);
            model.move(inside);
            QCOMPARE(model.instructionState().at("call_depth"), Json(1));
            const auto config = model.configuration();
            session.exportArchive(dir.filePath("session.zip"), model.exportReport());
            model.move(0);
            model.importConfiguration(config);
            QCOMPARE(model.seek(0), inside);
            Experiment experiment(frame);
            const auto offset = mode == 0 ? 40u : mode == 1 ? 72u : 16u;
            const auto replacement = word(1100 + offset / 4);
            experiment.setBuffer(frame, 100, 200, offset, replacement);
            const auto project = dir.filePath("experiment.json");
            experiment.save(project);
            const auto editedJob = prepareNativeAnalysisJob(capture, "debug-thread", dir.filePath("edited"),
                                                            worker, selection, project);
            QVERIFY_THROWS_EXCEPTION(std::exception, validateNativeAnalysisResult(editedJob, result));
            const auto edited = load(runNativeAnalysisJob(editedJob, worker, 30));
            validateNativeAnalysisResult(editedJob, edited);
            QVERIFY(result.at("context_id") != edited.at("context_id"));
            ExecutionTraceReader modified(dir.filePath("edited/trace.jsonl"),
                                          edited.at("artifacts").at("trace.jsonl").at("sha256"));
            modified.moveTo(modified.size());
            QCOMPARE(Reader(modified.storage().at({TraceSpace::Buffer, 7}).bytes).read<uint32_t>(),
                     expected + 1000);
            QProcess cli;
            QStringList args{"analyze",      capture, "debug-thread", "--gpa-event",      "100",
                             "--experiment", project, "--out",        dir.filePath("cli")};
            if (vendor)
                args << "--gpu-vendor" << QString::number(vendor);
            else
                args << "--warp";
            cli.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
            if (!cli.waitForFinished(30000)) {
                cli.kill();
                cli.waitForFinished(5000);
                QFAIL("Class workflow CLI exceeded its deadline");
            }
            QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
            QCOMPARE(cli.exitCode(), 0);
            const auto cliResult = load(dir.filePath("cli/result.json"));
            ExecutionTraceReader cliTrace(dir.filePath("cli/trace.jsonl"),
                                          cliResult.at("artifacts").at("trace.jsonl").at("sha256"));
            cliTrace.moveTo(cliTrace.size());
            QCOMPARE(cliTrace.storage().at({TraceSpace::Buffer, 7}).bytes,
                     modified.storage().at({TraceSpace::Buffer, 7}).bytes);
            ReplayOptions replayOptions;
            replayOptions.until = 100;
            replayOptions.before = true;
            replayOptions.warp = !vendor;
            if (vendor)
                replayOptions.vendor = vendor;
            Replay clean(frame, replayOptions);
            clean.run();
            QCOMPARE(clean.readBuffer(7), initial.at({TraceSpace::Buffer, 7}).bytes);
            QCOMPARE(clean.readBuffer(200), initial.at({TraceSpace::Buffer, 200}).bytes);
            save(dir.filePath("workflow.json"), {{"vendor", vendor},
                                                 {"created", created},
                                                 {"mode", mode},
                                                 {"expected", expected},
                                                 {"edited_expected", expected + 1000},
                                                 {"class_bindings", classes},
                                                 {"base_context", result.at("context")},
                                                 {"edited_context", edited.at("context")},
                                                 {"configuration", config},
                                                 {"inside", inside},
                                                 {"steps", reader.size()},
                                                 {"passed", true}});
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativeClassResourceTests)
#include "NativeClassResourceTests.moc"
