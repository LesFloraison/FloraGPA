#include "application/ShaderInspector.h"
#include "application/ShaderSourceLines.h"
#include "replay/Replay.h"
#include <QtTest>
#include <d3dcompiler.h>

class ShaderInspectorTests final : public QObject {
    Q_OBJECT
  private slots:
    void staticInterfaceCounts() {
        const std::string source = "float4 main(float4 p:POSITION):SV_Position{return p;}";
        flora::Com<ID3DBlob> code, errors, stripped, signature;
        flora::check(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "vs_4_0", 0,
                                0, &code, &errors),
                     "Compile static shader");
        flora::check(D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                                    D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped),
                     "Strip reflection");
        auto inspect = [](ID3DBlob *blob) {
            return flora::inspectShader(
                {static_cast<const uint8_t *>(blob->GetBufferPointer()), blob->GetBufferSize()});
        };
        QCOMPARE(inspect(stripped.Get())["interface_slots"], nlohmann::json(0));
        flora::check(D3DGetInputSignatureBlob(code->GetBufferPointer(), code->GetBufferSize(), &signature),
                     "Extract signature");
        auto info = inspect(signature.Get());
        QCOMPARE(info["stage"], nlohmann::json("signature"));
        QCOMPARE(info["interface_slots"], nlohmann::json(0));
    }
    void embeddedSources_data() {
        QTest::addColumn<bool>("legacy");
        QTest::newRow("SPDB compiler47") << false;
        QTest::newRow("SDBG compiler43") << true;
    }
    void embeddedSources() {
        QFETCH(bool, legacy);
        const std::string source = "cbuffer Params:register(b0){float4 tint;} float4 main(float4 "
                                   "position:SV_Position):SV_Target { return tint; }\n";
        struct Library {
            HMODULE value{};
            ~Library() {
                if (value)
                    FreeLibrary(value);
            }
        } dll;
        auto fn = &D3DCompile;
        if (legacy) {
            dll.value = LoadLibraryExW(L"d3dcompiler_43.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!dll.value)
                QSKIP("Optional legacy compiler is not installed");
            fn = reinterpret_cast<decltype(fn)>(GetProcAddress(dll.value, "D3DCompile"));
            QVERIFY(fn);
        }
        flora::Com<ID3DBlob> code, errors;
        auto hr = fn(source.data(), source.size(), "fixture.hlsl", nullptr, nullptr, "main", "ps_5_0",
                     D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &errors);
        QVERIFY2(SUCCEEDED(hr),
                 errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Compile failed");
        flora::Bytes bytes(static_cast<const uint8_t *>(code->GetBufferPointer()), code->GetBufferSize());
        auto info = flora::inspectShader(bytes);
        QCOMPARE(info["profile"].get<std::string>(), std::string("ps_5_0"));
        auto &embedded = info["embedded_sources"];
        QCOMPARE(embedded["status"].get<std::string>(), std::string("embedded_source_available"));
        QVERIFY(!embedded["files"].empty());
        QCOMPARE(embedded["files"][0]["text"].get<std::string>(), source);
        QCOMPARE(embedded["environment"]["hlslEntry"].get<std::string>(), std::string("main"));
        auto lines = flora::shaderSourceLines(bytes);
        QCOMPARE(lines.at("status").get<std::string>(), std::string("available"));
        QCOMPARE(lines.at("files").at(0).at("text").get<std::string>(), source);
        QVERIFY(!flora::shaderLinesByOffset(lines).empty());
        QVERIFY(!flora::shaderSourceLines(bytes, "#line 1 \"" +
                                                     lines.at("files").at(0).at("name").get<std::string>() +
                                                     "\"\n0 0x00000009: ret")
                     .at("issues")
                     .empty());
        QCOMPARE(info["constant_buffers"][0]["size"].get<UINT>(), 16u);
        auto shortened = bytes.first(31);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, flora::inspectShader(shortened));
    }
};
QTEST_GUILESS_MAIN(ShaderInspectorTests)
#include "ShaderInspectorTests.moc"
