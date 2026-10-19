#include "rdc/DebugTrace.h"
#include <QFile>
#include <QtTest>
#include <cmath>
#include <cstdlib>
#include <limits>

// These tests create API containers locally and never load RenderDoc. The
// production executable uses allocation bridges to the selected replay DLL.
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return std::malloc(size_t(size)); }
extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *memory) { std::free(memory); }

using namespace flora;
class RdcDebugTests : public QObject {
    Q_OBJECT
  private slots:
    void pixelOutputComparison() {
        using Json = nlohmann::json;
        ShaderReflection reflection;
        SigParameter signature;
        signature.systemValue = ShaderBuiltin::ColorOutput;
        signature.compCount = 4; signature.regChannelMask = 15;
        reflection.outputSignature.push_back(signature);
        ShaderDebugTrace trace;
        SourceVariableMapping mapping;
        mapping.name = "SV_Target0"; mapping.type = VarType::Float;
        mapping.rows = 1; mapping.columns = 4; mapping.signatureIndex = 0;
        mapping.offset = 0; mapping.undefinedValue = false;
        for (uint32_t i = 0; i < 4; ++i) {
            DebugVariableReference variable;
            variable.name = "_OUT.SV_Target0"; variable.type = DebugVariableType::Variable;
            variable.component = i; mapping.variables.push_back(variable);
        }
        trace.sourceVars.push_back(mapping);
        ShaderVariable output;
        output.name = "_OUT"; output.type = VarType::Struct;
        output.members.push_back(ShaderVariable("SV_Target0", 1.f, 2.f, 3.f, 0.f));
        auto steps = Json::array({{{"changes", Json::array({{{"before", rdcVariable({})},
                                                            {"after", rdcVariable(output)}}})}}});
        Json reference{{"status", "read"}, {"interpretation", "float"},
                       {"value", {{"uintValue", Json::array({1065353216u,1073741824u,1077936128u,0u})}}}};
        auto traceJson = rdcDebugTrace(trace);
        auto compare = [&] { return rdcComparePixelOutput(reflection, traceJson, steps, reference); };
        QCOMPARE(compare().at("status"), Json("exact_output_match"));
        // A one-bit difference must not be hidden behind a default tolerance.
        reference["value"]["uintValue"][1] = 1073741825u;
        QCOMPARE(compare().at("status"), Json("output_mismatch"));
        QCOMPARE(compare().at("mismatched_components"), Json::array({1}));
        reference["status"] = "unverified_replay_changed";
        QCOMPARE(compare().at("status"), Json("unavailable"));
        reference["status"] = "read";
        // Matching input signature indices must not become output mappings.
        auto input = traceJson["sourceVars"][0];
        for (auto &v : input["variables"]) v["type"] = uint32_t(DebugVariableType::Input);
        traceJson["sourceVars"].push_back(input);
        QCOMPARE(compare().at("status"), Json("output_mismatch"));
        traceJson["sourceVars"].push_back(traceJson["sourceVars"][0]);
        QCOMPARE(compare().at("status"), Json("unavailable"));
        traceJson["sourceVars"].erase(2);
        steps.push_back({{"changes", Json::array({{{"before", rdcVariable(output)},
                                                   {"after", rdcVariable({})}}})}});
        QCOMPARE(compare().at("status"), Json("unavailable"));
        steps.erase(1);
        reflection.outputSignature[0].regChannelMask = 10;
        QCOMPARE(compare().at("status"), Json("unavailable"));
    }
    void hellbladePixelOutputEvidence() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PIXEL_DEBUG_REPORT");
        if (path.isEmpty()) QSKIP("Set the saved Hellblade2 MainPS report for this CPU evidence check");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        using Json = nlohmann::json;
        const auto report = Json::parse(file.readAll().toStdString());
        QCOMPARE(report.at("execution"), Json("37003"));
        ShaderReflection reflection;
        SigParameter signature;
        signature.systemValue = ShaderBuiltin::ColorOutput;
        signature.compCount = 4; signature.regChannelMask = 15;
        reflection.outputSignature.push_back(signature);
        const auto comparison = rdcComparePixelOutput(reflection, report.at("trace"), report.at("steps"),
                                                      report.at("gpu_invocation_reference"));
        QCOMPARE(comparison.at("status"), Json("output_mismatch"));
        QCOMPARE(comparison.at("mismatched_components"), Json::array({0,1,2}));
        QCOMPARE(comparison.at("trace_bits"), Json::array({1026478034u,1028981137u,1034447827u,0u}));
        QCOMPARE(comparison.at("gpu_bits"), Json::array({1026307110u,1028810213u,1034362365u,0u}));
    }
    void typedValues() {
        ShaderVariable v;
        v.name = "wide";
        v.type = VarType::ULong;
        v.rows = 1;
        v.columns = 2;
        v.value.u64v[0] = UINT64_MAX;
        v.value.u64v[1] = (uint64_t(1) << 63) + 1;
        v.value.u64v[15] = UINT64_MAX - 1;
        const auto j = rdcVariable(v);
        QCOMPARE(j["value"]["u64v"][0].get<uint64_t>(), UINT64_MAX);
        QCOMPARE(j["value"]["u64v"][1].get<uint64_t>(), (uint64_t(1) << 63) + 1);
        QCOMPARE(j["value"]["s64v"][0].get<int64_t>(), int64_t(-1));
        QCOMPARE(j["value"]["s64v"][15].get<int64_t>(), int64_t(-2));
        QCOMPARE(j["value"]["s8v"][0].get<int>(), -1);
        QCOMPARE(j["value"]["u8v"][0].get<unsigned>(), 255u);
        QCOMPARE(j["value"]["s16v"][0].get<int>(), -1);
        QCOMPARE(j["value"]["u16v"][0].get<unsigned>(), 65535u);
        QCOMPARE(j["value"]["u32v"][0].get<uint32_t>(), UINT32_MAX);
        QCOMPARE(j["value"]["s32v"][0].get<int32_t>(), int32_t(-1));
        QCOMPARE(j["value"]["f64v"][0].get<std::string>(), std::string("nan"));
        for (const auto &field : j["value"].items())
            QCOMPARE(field.value().size(), size_t(16));
        QCOMPARE(nlohmann::json::parse(j.dump()), j);
    }
    void nonfiniteAndHalf() {
        ShaderVariable v;
        v.value.f32v[0] = std::numeric_limits<float>::infinity();
        v.value.f32v[1] = -std::numeric_limits<float>::infinity();
        v.value.f32v[2] = std::numeric_limits<float>::quiet_NaN();
        v.value.f32v[3] = -0.f;
        v.value.f32v[4] = std::numeric_limits<float>::denorm_min();
        auto j = rdcVariable(v)["value"]["f32v"];
        QCOMPARE(j[0].get<std::string>(), std::string("inf"));
        QCOMPARE(j[1].get<std::string>(), std::string("-inf"));
        QCOMPARE(j[2].get<std::string>(), std::string("nan"));
        QVERIFY(std::signbit(j[3].get<double>()));
        QCOMPARE(j[4].get<double>(), double(std::numeric_limits<float>::denorm_min()));
        const uint16_t bits[] = {0x0000, 0x8000, 0x0001, 0x03ff, 0x0400, 0x3c00,
                                 0xbc00, 0x7bff, 0x7c00, 0xfc00, 0x7e00, 0x7c01};
        for (size_t i = 0; i < std::size(bits); ++i)
            v.value.u16v[i] = bits[i];
        j = rdcVariable(v)["value"]["f16v"];
        QVERIFY(!std::signbit(j[0].get<double>()));
        QVERIFY(std::signbit(j[1].get<double>()));
        QCOMPARE(j[2].get<double>(), 0.000000059604644775390625);
        QCOMPARE(j[3].get<double>(), 0.000060975551605224609375);
        QCOMPARE(j[4].get<double>(), 0.00006103515625);
        QCOMPARE(j[5].get<double>(), 1.);
        QCOMPARE(j[6].get<double>(), -1.);
        QCOMPARE(j[7].get<double>(), 65504.);
        QCOMPARE(j[8].get<std::string>(), std::string("inf"));
        QCOMPARE(j[9].get<std::string>(), std::string("-inf"));
        QCOMPARE(j[10].get<std::string>(), std::string("nan"));
        QCOMPARE(j[11].get<std::string>(), std::string("nan"));
    }
    void nestedVariablesAndChanges() {
        ShaderVariable root;
        root.name = "constants";
        root.type = VarType::Struct;
        root.members.push_back(ShaderVariable("matrix", 1.f, 2.f, 3.f, 4.f));
        root.members[0].rows = 4;
        root.members[0].flags = ShaderVariableFlags::RowMajorMatrix;
        root.members[0].value.f32v[15] = 123.f;
        auto j = rdcVariable(root);
        QCOMPARE(j["members"][0]["value"]["f32v"][15].get<double>(), 123.);
        QCOMPARE(j["members"][0]["rows"].get<int>(), 4);
        ShaderDebugState s;
        s.callstack.push_back("main");
        s.callstack.push_back("nested");
        s.nextInstruction = 24;
        s.stepIndex = 100;
        ShaderVariableChange birth;
        birth.after = root;
        ShaderVariableChange death;
        death.before = root;
        s.changes.push_back(birth);
        s.changes.push_back(death);
        auto state = rdcDebugState(s);
        QCOMPARE(state["changes"][0]["after"], j);
        QCOMPARE(state["changes"][1]["before"], j);
        QCOMPARE(state["changes"][0]["before"]["name"].get<std::string>(), std::string());
        QCOMPARE(state["changes"][1]["after"]["name"].get<std::string>(), std::string());
        QCOMPARE(state["callstack"], nlohmann::json({"main", "nested"}));
        QCOMPARE(state["stepIndex"].get<int>(), 100);
        QCOMPARE(state["nextInstruction"].get<int>(), 24);
    }
    void sourceMaps() {
        ShaderDebugTrace trace;
        trace.stage = ShaderStage::Compute;
        InstructionSourceInfo instruction;
        instruction.instruction = 42;
        instruction.lineInfo.fileIndex = 2;
        instruction.lineInfo.lineStart = 4;
        instruction.lineInfo.lineEnd = 5;
        SourceVariableMapping mapping;
        mapping.name = "value[2].z";
        mapping.type = VarType::Double;
        mapping.offset = 80;
        mapping.undefinedValue = true;
        mapping.variables.push_back(DebugVariableReference(DebugVariableType::Variable, "r9", 2));
        instruction.sourceVars.push_back(mapping);
        trace.instInfo.push_back(instruction);
        trace.sourceVars.push_back(mapping);
        SourceVariableMapping constant;
        constant.name = "constants.member";
        constant.type = VarType::Float;
        constant.rows = constant.columns = 1;
        constant.offset = 64;
        constant.variables.push_back(DebugVariableReference(DebugVariableType::Constant, "cb0[4]"));
        trace.sourceVars.push_back(constant);
        const auto j = rdcDebugTrace(trace);
        QVERIFY(j["sourceVars"][0]["offset"].is_null());
        QCOMPARE(j["sourceVars"][1]["offset"].get<int>(), 64);
        QCOMPARE(j["stage"].get<unsigned>(), unsigned(ShaderStage::Compute));
        QCOMPARE(j["instInfo"][0]["sourceVars"][0]["offset"].get<int>(), 80);
        QCOMPARE(j["instInfo"][0]["sourceVars"][0]["variables"][0]["component"].get<int>(), 2);
        QVERIFY(j["instInfo"][0]["sourceVars"][0]["undefinedValue"].get<bool>());
        QVERIFY(!j.contains("debugger"));
    }
};
QTEST_GUILESS_MAIN(RdcDebugTests)
#include "RdcDebugTests.moc"
