#include "SyntheticCapture.h"
#include "application/Experiment.h"
#include "application/ExternalShaderTools.h"
#include "application/HlslCompilation.h"
#include "application/ShaderInspector.h"
#include "application/SystemDisassembly.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
void save(const QString &path, const QByteArray &data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("Cannot write test file");
}
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read test file");
    return file.readAll();
}
Json load(const QString &path) { return Json::parse(read(path).toStdString()); }
QString stub(const QString &root, const Json &config) {
    const auto dir = root + "/工具 & space";
    QDir().mkpath(dir);
    const auto target = dir + "/shader tool.exe";
    if (!QFile::copy(QCoreApplication::applicationDirPath() + "/FloraShaderToolStub.exe", target))
        throw std::runtime_error("Cannot copy native tool fixture");
    save(dir + "/shader-tool-stub.json", QByteArray::fromStdString(config.dump()));
    return target;
}
std::vector<uint8_t> unsupported() {
    return compileHlsl("Texture1D<float4> data:register(t0);SamplerState samp:register(s0);float4 main(float "
                       "x:TEXCOORD):SV_Target{return data.Sample(samp,x);}",
                       "ps_5_0")
        .bytecode;
}
std::string hex(Bytes bytes) {
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
        .toHex()
        .toStdString();
}
bool stopped(DWORD pid) {
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!handle)
        return true;
    const bool result = WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0;
    CloseHandle(handle);
    return result;
}
} // namespace
class ExternalShaderTests final : public QObject {
    Q_OBJECT
  private slots:
    void argumentAndLogContract() {
        QTemporaryDir root;
        const auto tool = stub(root.path(), Json::object());
        const auto out = root.filePath("work with spaces");
        QDir().mkpath(out);
        const QStringList args{"two words", "embedded\"quote", "trailing\\", "中文", ""};
        runShaderTool(tool, args, out, "tool.log");
        auto expected = Json::array();
        for (const auto &arg : args)
            expected.push_back(arg.toStdString());
        QCOMPARE(load(out + "/invocation.json").at("arguments"), expected);
        QCOMPARE(read(out + "/tool.log"), QByteArray("tool stdout\ntool stderr"));
    }
    void timeoutAndDescendants() {
        QTemporaryDir root;
        const auto tool = stub(root.path(), {{"child", true}, {"sleep_ms", 60000}});
        const auto out = root.filePath("work");
        QDir().mkpath(out);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runShaderTool(tool, {}, out, "tool.log", 700));
        const auto invocation = load(out + "/invocation.json");
        QVERIFY(stopped(invocation.at("pid")));
        QVERIFY(stopped(invocation.at("child")));
        QCOMPARE(read(out + "/tool.log"), QByteArray("tool stdout\ntool stderr"));
    }
    void completedParentKillsDescendant() {
        QTemporaryDir root;
        const auto tool = stub(root.path(), {{"child", true}});
        const auto out = root.filePath("work");
        QDir().mkpath(out);
        runShaderTool(tool, {}, out, "tool.log");
        QVERIFY(stopped(load(out + "/invocation.json").at("child")));
    }
    void fallbackCompilation_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<bool>("valid");
        QTest::newRow("valid") << "float4 main():SV_Target{return float4(0,1,0,1);}" << true;
        QTest::newRow("invalid") << "invalid HLSL" << false;
    }
    void fallbackCompilation() {
        QFETCH(QString, source);
        QFETCH(bool, valid);
        QTemporaryDir root;
        const auto tool = stub(root.path(), {{"source", source.toStdString()}});
        const auto out = root.filePath("work");
        QDir().mkpath(out);
        const auto report = exportRecoveredHlsl(unsupported(), out, tool);
        QCOMPARE(report.at("native"), Json(false));
        QCOMPARE(report.at("recompiles"), Json(valid));
        QCOMPARE(report.at("semantic_equivalence"), Json("not_verified"));
        QCOMPARE(read(out + "/reconstructed.hlsl"), source.toUtf8());
        QCOMPARE(QFile::exists(out + "/reconstructed.recompiled.dxbc"), valid);
        QCOMPARE(load(out + "/invocation.json").at("arguments")[0], Json("-D"));
    }
    void nativeFirst() {
        QTemporaryDir root;
        const auto code = compileHlsl("float4 main():SV_Target{return 1;}", "ps_5_0").bytecode;
        QCOMPARE(exportRecoveredHlsl(code, root.path(), "missing.exe").at("native"), Json(true));
        QVERIFY(!QFile::exists(root.filePath("decompiler.log")));
    }
    void assemblyValidation_data() {
        QTest::addColumn<QString>("profile");
        QTest::addColumn<bool>("valid");
        QTest::newRow("same-stage") << "ps_5_0" << true;
        QTest::newRow("wrong-stage") << "vs_5_0" << false;
        QTest::newRow("corrupt-bytecode") << "invalid" << false;
    }
    void assemblyValidation() {
        QFETCH(QString, profile);
        QFETCH(bool, valid);
        QTemporaryDir root;
        auto code = compileHlsl("float4 main():SV_Target{return 1;}", "ps_5_0").bytecode;
        const auto replacement =
            profile == "invalid" ? std::vector<uint8_t>{0, 1, 2, 3}
            : profile == "vs_5_0"
                ? compileHlsl("float4 main(float4 p:POSITION):SV_Position{return p;}", "vs_5_0").bytecode
                : code;
        const auto tool = stub(root.path(), {{"bytecode", hex(replacement)}});
        const auto out = root.filePath("work");
        QDir().mkpath(out);
        if (!valid) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, assembleShader(code, "assembly", out, tool));
            QVERIFY(!QFile::exists(out + "/replacement.dxbc"));
        } else {
            const auto report = assembleShader(code, "assembly", out, tool);
            QCOMPARE(report.at("replacement_sha256"), Json(sha256(code)));
            QCOMPARE(read(out + "/original.dxbc").toHex().toStdString(), hex(code));
            auto args = load(out + "/invocation.json").at("arguments");
            QCOMPARE(args[0], Json("-a"));
            QCOMPARE(args[1], Json("--copy-reflection"));
        }
    }
    void missingAndFailedOutput_data() {
        QTest::addColumn<int>("exitCode");
        QTest::newRow("missing-output") << 0;
        QTest::newRow("nonzero-exit") << 7;
    }
    void missingAndFailedOutput() {
        QFETCH(int, exitCode);
        QTemporaryDir root;
        const auto tool = stub(root.path(), {{"exit_code", exitCode}});
        const auto out = root.filePath("work");
        QDir().mkpath(out);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportRecoveredHlsl(unsupported(), out, tool));
        QVERIFY(!QFile::exists(out + "/reconstructed.recompiled.dxbc"));
        QVERIFY(QFile::exists(out + "/decompiler.log"));
    }
    void realAssemblerAndExperiment() {
        const auto tool = qEnvironmentVariable("FLORA_TEST_SHADER_TOOL");
        if (tool.isEmpty())
            QSKIP("Optional cmd_Decompiler.exe is not configured");
        QTemporaryDir root;
        testing::graphicsCounterCapture(false).save(root.filePath("frame.gpa_frame"));
        Frame frame(root.filePath("frame.gpa_frame").toStdWString());
        auto original = frame.shader(frame.resource(32).data);
        auto assembly = QByteArray::fromStdString(systemDisassembly(original, 0x80));
        QVERIFY2(assembly.contains("l(0x3f800000,0x00000000,0x00000000,0x3f800000)"), assembly.constData());
        assembly.replace("l(0x3f800000,0x00000000,0x00000000,0x3f800000)",
                         "l(0x00000000,0x3f800000,0x00000000,0x3f800000)");
        QDir().mkpath(root.filePath("assembled"));
        assembleShader(original, assembly, root.filePath("assembled"), tool);
        const auto replacement = read(root.filePath("assembled/replacement.dxbc"));
        Experiment experiment(frame);
        experiment.setShader(
            frame, 32, {reinterpret_cast<const uint8_t *>(replacement.data()), size_t(replacement.size())},
            assembly.toStdString(), "main", "asm");
        experiment.save(root.filePath("saved.json"));
        Experiment loaded(frame);
        loaded.load(root.filePath("saved.json"), frame);
        QCOMPARE(loaded.shaderSource(32).at("source_language"), Json("asm"));
        ReplayOptions options;
        loaded.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.output().rgba, (std::vector<uint8_t>{0, 255, 0, 255}));
        QVERIFY(loaded.undo());
        QCOMPARE(loaded.shaderBytes(frame, 32), std::vector<uint8_t>(original.begin(), original.end()));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        try {
            auto job = load(args[2]);
            const auto code =
                QByteArray::fromHex(QByteArray::fromStdString(job.at("bytecode").get<std::string>()));
            const Bytes bytes{reinterpret_cast<const uint8_t *>(code.data()), size_t(code.size())};
            const auto out = QString::fromStdString(job.at("out"));
            QDir().mkpath(out);
            const auto tool = QString::fromStdString(job.value("tool", std::string{}));
            Json report;
            if (job.at("action") == "assemble")
                report = assembleShader(bytes, QByteArray::fromStdString(job.at("source")), out, tool);
            else
                report = exportRecoveredHlsl(bytes, out, tool, job.value("saved", Json{}));
            save(args[3], QByteArray::fromStdString(report.dump(2)));
            return 0;
        } catch (const std::exception &e) {
            save(args[3], QByteArray::fromStdString(Json{{"error", e.what()}}.dump()));
            return 1;
        }
    }
    ExternalShaderTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ExternalShaderTests.moc"
