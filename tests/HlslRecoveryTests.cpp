#include "application/Experiment.h"
#include "application/HlslCompilation.h"
#include "application/HlslRecoveryInternal.h"
#include "application/ShaderProject.h"
#include <QFile>
#include <QtTest>
using Json = nlohmann::json;
using namespace flora;
namespace {
Json metadata(const std::string &stage) {
    return {{"stage", stage},
            {"profile", stage + "_5_0"},
            {"bindings", Json::array()},
            {"constant_buffers", Json::array()},
            {"signatures", Json::object()}};
}
Json dispatch(const Json &job) {
    const auto action = job.at("action").get<std::string>();
    if (action == "split")
        return hlsl::splitArguments(job.at("text"));
    if (action == "parse") {
        const auto p = hlsl::parseInstruction(job.at("text"));
        return Json::array({p.opcode, p.arguments, p.precise});
    }
    if (action == "recover") {
        const auto bytes =
            QByteArray::fromHex(QByteArray::fromStdString(job.at("bytecode").get<std::string>()));
        return recoverHlsl({reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())});
    }
    if (action == "options")
        return hlslCompilationOptions(job.at("source"), job.value("optimization", "auto"));
    if (action == "compile") {
        const auto result = compileHlsl(job.at("source"), job.at("profile"), job.value("entry", "main"),
                                        job.value("name", "edited.hlsl"), job.value("optimization", "auto"));
        return {{"bytecode", QByteArray(reinterpret_cast<const char *>(result.bytecode.data()),
                                        qsizetype(result.bytecode.size()))
                                 .toHex()
                                 .toStdString()},
                {"diagnostics", result.diagnostics},
                {"options", result.options}};
    }
    if (action == "reconstruct") {
        const auto bytes =
            QByteArray::fromHex(QByteArray::fromStdString(job.at("bytecode").get<std::string>()));
        const auto result =
            reconstructHlsl({reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())},
                            job.value("saved", Json{}));
        return {{"source", result.source},
                {"report", result.report},
                {"bytecode", QByteArray(reinterpret_cast<const char *>(result.recompiled.data()),
                                        qsizetype(result.recompiled.size()))
                                 .toHex()
                                 .toStdString()}};
    }
    auto info = job.value("info", metadata(job.value("stage", std::string("cs"))));
    hlsl::Lowerer lower(info, job.value("assembly", std::string{}));
    lower.returnCode = job.value("return_code", std::string("return;"));
    for (const auto &line : job.value("declarations", Json::array()))
        if (!lower.declaration(line))
            throw std::runtime_error("Unsupported declaration");
    lower.serial = job.value("serial", 0u);
    lower.currentOpcode = job.value("current_opcode", std::string{});
    lower.orderedSampleMad = job.value("ordered_sample_mad", false);
    lower.instructionPrecise = job.value("precise", false);
    Json result;
    if (action == "raw")
        return lower.raw(job.at("operand"), job.value("kind", "bits"));
    if (action == "write")
        result = lower.write(job.at("dest"), job.at("expression"), job.value("kind", "bits"),
                             job.value("sat", false));
    else if (action == "declaration")
        result = lower.declaration(job.at("text"));
    else if (action == "instruction")
        result = lower.instruction(job.at("text"));
    else if (action == "prepare")
        return lower.prepareMemory(job.at("body").get<hlsl::Strings>());
    else if (action == "graphics" || action == "geometry") {
        const auto [prefix, local, suffix] =
            action == "graphics" ? lower.setupGraphics() : lower.setupGeometry();
        result = Json::array({prefix, local, suffix});
    } else if (action == "translate")
        return lower.translate();
    else if (action == "offset")
        return lower.sampleOffset(job.at("text"), job.at("dimension"));
    else
        throw std::runtime_error("Unknown recovery probe action");
    return {{"result", result},
            {"globals", lower.globals},
            {"serial", lower.serial},
            {"return_code", lower.returnCode}};
}
} // namespace
class HlslRecoveryTests final : public QObject {
    Q_OBJECT
  private slots:
    void parsing() {
        auto p = hlsl::parseInstruction("sample_indexable [precise(xy)](texture2d)(float,float,float,float) "
                                        "r0.xyzw, r1.xyzw, t0.xyzw, s0");
        QVERIFY(p.precise);
        QCOMPARE(p.arguments.size(), size_t(4));
        QCOMPARE(p.opcode, std::string("sample_indexable(texture2d)(float,float,float,float)"));
        QCOMPARE(hlsl::splitArguments(
                     "r0.xy, cb0[r1.x + 1].xyzw, l(0x00000000, 0x3f800000, 0x00000000, 0x00000000)")
                     .size(),
                 size_t(3));
    }
    void rawBitsAndPrecision() {
        hlsl::Lowerer lower(metadata("ps"), "");
        QCOMPARE(lower.raw("r0", "f"), std::string("asfloat(r0)"));
        QCOMPARE(lower.raw("vPrim"), std::string("vPrim"));
        QVERIFY(lower.declaration("dcl_resource_raw t0"));
        QVERIFY(lower.declaration("dcl_tgsm_raw g0, 256"));
        QCOMPARE(lower.raw("-r0.x"), std::string("(r0.xxxx ^ 0x80000000u)"));
        QCOMPARE(lower.raw("-|r0.x|", "f"), std::string("(-abs(asfloat(r0.xxxx)))"));
        QCOMPARE(lower.write("r1.xy", "foo", "f"),
                 std::string("precise float4 value1 = foo;\nr1.xy = (asuint(value1)).xy;"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, lower.raw("l(1.0)"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, lower.instruction("unknown r0.xyzw, r1.xyzw"));
    }
    void appendEscapes() {
        auto info = metadata("cs");
        info["bindings"].push_back({{"slot", 0}, {"type", 9}});
        hlsl::Lowerer lower(info, "");
        QVERIFY(lower.declaration("dcl_uav_structured u0, 16"));
        const hlsl::Strings body{"imm_atomic_alloc r0.x, u0",
                                 "store_structured u0.xyzw, r0.x, l(0x00000000), r1.xyzw"};
        QCOMPARE(lower.prepareMemory(body), hlsl::Strings({"flora_append u0, r1.xyzw"}));
        auto bad = body;
        bad.push_back("mov r2.x, r0.x");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, lower.prepareMemory(bad));
    }
    void nativeShaderRoundTrip_data() {
        QTest::addColumn<QString>("profile");
        QTest::addColumn<QString>("source");
        QTest::newRow("vertex") << "vs_5_0" << "float4 main(float4 p:POSITION):SV_Position{return p;}";
        QTest::newRow("pixel") << "ps_5_0" << "float4 main():SV_Target{return float4(0.25,0.5,0.75,1);}";
        QTest::newRow("compute") << "cs_5_0"
                                 << "RWBuffer<uint> output:register(u0);[numthreads(4,1,1)]void main(uint3 "
                                    "id:SV_DispatchThreadID){output[id.x]=id.x*2+1;}";
    }
    void nativeShaderRoundTrip() {
        QFETCH(QString, profile);
        QFETCH(QString, source);
        Json p{{"format", "FloraGPA shader project 1"},
               {"root", "source.hlsl"},
               {"entry", "main"},
               {"profile", profile.toStdString()},
               {"files", Json::array({{{"name", "source.hlsl"}, {"text", source.toStdString()}}})}};
        const auto original = compileShaderProject(p);
        const auto recovered = recoverHlsl(original.bytecode);
        QVERIFY(recovered.starts_with("// Reconstructed from DXBC."));
        p["files"][0]["text"] = recovered;
        if (recovered.find("// FloraGPA compiler optimization: preserve") != std::string::npos)
            p["flags"] = 4;
        QVERIFY(!compileShaderProject(p).bytecode.empty());
    }
    void savedSourceBinding() {
        const std::string source = "float4 entry():SV_Target{return float4(1,0,0,1);}";
        const auto code = compileHlsl(source, "ps_5_0", "entry").bytecode;
        Json saved{{"source_language", "hlsl"}, {"source_text", source}, {"source_entry", "entry"}};
        auto result = reconstructHlsl(code, saved);
        QCOMPARE(result.source, source);
        QCOMPARE(result.report.at("source_kind"), Json("saved_applied_hlsl"));
        QCOMPARE(result.report.at("semantic_equivalence"), Json("bytecode_identical"));
        QCOMPARE(result.report.at("entry"), Json("entry"));
        saved["source_text"] = "float4 entry():SV_Target{return float4(0,1,0,1);}";
        result = reconstructHlsl(code, saved);
        QCOMPARE(result.report.at("source_kind"), Json("reconstructed_hlsl_not_original"));
        QCOMPARE(result.report.at("semantic_equivalence"), Json("not_verified"));
        saved["source_text"] = "invalid HLSL";
        QCOMPARE(reconstructHlsl(code, saved).source, result.source);
    }
    void simultaneousBf1Replacements() {
        const auto root = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("External BF1 capture directory is not configured");
        Frame frame((root + "/bf1_2026_01_21__16_53_05.gpa_frame").toStdWString());
        Experiment experiment(frame);
        unsigned count = 0;
        for (const auto &[id, e] : frame.entries()) {
            if (e.category != 5 || e.type < 0x93 || e.type > 0x95)
                continue;
            const auto code = frame.shader(frame.resource(id).data);
            const auto result = reconstructHlsl(code);
            experiment.setShader(frame, id, result.recompiled, result.source, "main");
            ++count;
        }
        QCOMPARE(count, 47u);
        ReplayOptions options;
        experiment.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(sha256(replay.output().rgba),
                 std::string("1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            return 2;
        try {
            Json results = Json::array();
            for (const auto &job : Json::parse(input.readAll().toStdString())) {
                try {
                    results.push_back({{"ok", true}, {"result", dispatch(job)}});
                } catch (const std::exception &e) {
                    results.push_back({{"ok", false}, {"error", e.what()}});
                }
            }
            const auto data = results.dump(2, ' ', true);
            output.write(data.data(), qint64(data.size()));
            return 0;
        } catch (...) {
            return 3;
        }
    }
    HlslRecoveryTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "HlslRecoveryTests.moc"
