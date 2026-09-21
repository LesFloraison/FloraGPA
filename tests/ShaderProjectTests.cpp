#include "application/ShaderProject.h"
#include "core/Dxbc.h"
#include <QFile>
#include <QtTest>

using Json = nlohmann::json;
using namespace flora;
namespace {
Json project() {
    return {
        {"format", "FloraGPA shader project 1"},
        {"root", "root/main.hlsl"},
        {"entry", "main"},
        {"profile", "ps_5_0"},
        {"files", Json::array({{{"name", "root/main.hlsl"},
                                {"text", "#include \"value.hlsl\"\nfloat4 main():SV_Target{return VALUE;}"}},
                               {{"name", "root/value.hlsl"}, {"text", "#define VALUE 0.25"}}})}};
}
Json dispatch(const Json &job) {
    const auto action = job.at("action").get<std::string>();
    if (action == "path")
        return shaderProjectPathKey(job.at("value"));
    if (action == "defines")
        return parseShaderProjectDefines(job.at("value").get<std::string>());
    if (action == "sources")
        return shaderProjectFromSources(job.at("value"), job.at("profile"));
    if (action == "validate") {
        const auto p = validateShaderProject(job.at("value"));
        return {{"project", p}, {"sha256", shaderProjectDigest(p)}};
    }
    if (action == "verify") {
        const auto raw =
            QByteArray::fromHex(QByteArray::fromStdString(job.at("bytecode").get<std::string>()));
        return verifyShaderProject({reinterpret_cast<const uint8_t *>(raw.data()), size_t(raw.size())},
                                   job.at("value"));
    }
    if (action == "compile") {
        const auto compiled = compileShaderProject(job.at("value"));
        return {{"report", compiled.report},
                {"bytecode", QByteArray(reinterpret_cast<const char *>(compiled.bytecode.data()),
                                        qsizetype(compiled.bytecode.size()))
                                 .toHex()
                                 .toStdString()},
                {"verify", verifyShaderProject(compiled.bytecode, job.at("value"))}};
    }
    throw std::runtime_error("Unknown probe action");
}
} // namespace
class ShaderProjectTests final : public QObject {
    Q_OBJECT
  private slots:
    void defaultsAndIdentity() {
        auto p = project();
        p["root"] = "ROOT/./MAIN.HLSL";
        auto normalized = validateShaderProject(p);
        QCOMPARE(normalized["root"], Json("root/main.hlsl"));
        QCOMPARE(normalized["flags"], Json(2048));
        QCOMPARE(normalized["defines"], Json::array());
        QCOMPARE(shaderProjectPathKey("dir/Straße.hlsl"), std::string("dir\\strasse.hlsl"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderProjectPathKey("C:foo.hlsl"));
        p["files"].push_back({{"name", "ROOT\\VALUE.HLSL"}, {"text", ""}});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateShaderProject(p));
    }
    void macroSerialization() {
        QCOMPARE(parseShaderProjectDefines(" /DA /DB=hello world /DC=\"a /Dx\" /DD='x\\'y'"),
                 Json::array({{{"name", "A"}, {"value", ""}},
                              {{"name", "B"}, {"value", "hello world"}},
                              {{"name", "C"}, {"value", "\"a /Dx\""}},
                              {{"name", "D"}, {"value", "'x\\'y'"}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, parseShaderProjectDefines("/DX=\"x"));
    }
    void memoryIncludesAndVerification() {
        auto p = project();
        p["flags"] = 2048 | 0x100;
        auto result = compileShaderProject(p);
        QCOMPARE(result.report["includes"].size(), size_t(1));
        QCOMPARE(result.report["includes"][0]["file"], Json("root/value.hlsl"));
        QCOMPARE(result.report["flags"], Json(2048));
        QCOMPARE(result.report["requested_flags"], Json(2304));
        QCOMPARE(verifyShaderProject(result.bytecode, p)["semantic_equivalence"], Json("bytecode_identical"));
        p["files"][1]["text"] = "#define VALUE 0.75";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifyShaderProject(result.bytecode, p));
    }
    void noDiskFallback() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile f(dir.filePath("disk.hlsl"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#define VALUE 1");
        f.close();
        auto p = project();
        p["files"][0]["text"] =
            ("#include \"" + f.fileName() + "\"\nfloat4 main():SV_Target{return VALUE;}").toStdString();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, compileShaderProject(p));
    }
    void localSystemSearch() {
        auto p = project();
        p["include_dirs"] = Json::array({"shared"});
        p["files"].push_back({{"name", "shared/value.hlsl"}, {"text", "#define VALUE 1"}});
        auto local = compileShaderProject(p);
        QCOMPARE(local.report["includes"][0]["file"], Json("root/value.hlsl"));
        p["files"][0]["text"] = "#include <value.hlsl>\nfloat4 main():SV_Target{return VALUE;}";
        auto system = compileShaderProject(p);
        QCOMPARE(system.report["includes"][0]["file"], Json("shared/value.hlsl"));
        QVERIFY(local.bytecode != system.bytecode);
    }
    void parentLifetime() {
        auto p = project();
        p["files"][1]["text"] = "#include \"nested/inner.hlsl\"\n";
        p["files"].push_back(
            {{"name", "root/nested/inner.hlsl"}, {"text", "#include \"../../empty.hlsl\"\n#define VALUE 1"}});
        p["files"].push_back({{"name", "empty.hlsl"}, {"text", ""}});
        const auto r = compileShaderProject(p);
        QCOMPARE(r.report["includes"].size(), size_t(3));
        QCOMPARE(r.report["includes"][2]["parent"], Json("root/nested/inner.hlsl"));
    }
    void boundedRecursion() {
        auto p = project();
        p["files"][1]["text"] = "#include \"value.hlsl\"\n";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, compileShaderProject(p));
    }
    void debugChunkVerification() {
        auto p = project();
        auto result = compileShaderProject(p);
        auto parts = readDxbcParts(result.bytecode);
        const std::vector<uint8_t> debug{1, 2, 3, 4};
        parts.emplace_back(0x47424453, debug);
        auto modified = makeDxbc(parts);
        QCOMPARE(verifyShaderProject(modified, p)["semantic_equivalence"],
                 Json("non_debug_chunks_identical"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
            return 2;
        Json results = Json::array();
        try {
            const auto jobs = Json::parse(input.readAll().toStdString());
            for (const auto &job : jobs) {
                try {
                    results.push_back({{"ok", true}, {"result", dispatch(job)}});
                } catch (const std::exception &e) {
                    results.push_back({{"ok", false}, {"error", e.what()}});
                }
            }
            const auto serialized = results.dump(2, ' ', true);
            output.write(serialized.data(), qint64(serialized.size()));
            return 0;
        } catch (...) {
            return 3;
        }
    }
    ShaderProjectTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ShaderProjectTests.moc"
