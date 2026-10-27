#include "application/DxbcExecutionTrace.h"
#include "application/NativeAnalysis.h"
#include "application/NativeExecutionDebugModel.h"
#include "application/ShaderDebugData.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
const char source[] = R"(
RWByteAddressBuffer output : register(u0);
[numthreads(2,1,1)] void main(uint index : SV_GroupIndex) {
    uint total = index + 10;
    [loop] for (uint i = 0; i < 3; ++i) {
        total += i;
    }
    output.Store(index * 4, total);
}
)";
InputSnapshot fixture(bool debug = true) {
    InputSnapshot input;
    input.stage = 5;
    input.event = 19;
    input.shader = 7;
    Com<ID3DBlob> code, errors;
    check(D3DCompile(source, sizeof(source) - 1, "native-compute-source.hlsl", nullptr, nullptr, "main",
                     "cs_5_0", (debug ? D3DCOMPILE_DEBUG : 0) | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code,
                     &errors),
          "Compile native debug model fixture");
    const auto first = static_cast<const uint8_t *>(code->GetBufferPointer());
    input.bytecode.assign(first, first + code->GetBufferSize());
    input.inputs.memory =
        std::make_shared<Memory>(std::map<uint64_t, Buffer>{{11, Buffer::known(std::vector<uint8_t>(8))}},
                                 std::map<Register, BufferView>{{{30, {0}}, {11, 0, 8, 0}}});
    input.inputs.constantBuffers[3] = {{{0x80000000, 0x7fc0abcd, 0xffffffff, 7}, 5}};
    return input;
}
void save(const QString &path, const std::string &text) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), text.size()) != qint64(text.size()))
        throw std::runtime_error("Cannot write native model test artifact");
}
} // namespace
class NativeExecutionDebugModelTests : public QObject {
    Q_OBJECT
  private slots:
    void doubleRegisterInterpretation_data() {
        QTest::addColumn<uint32_t>("valid");
        for (uint32_t valid = 0; valid < 16; ++valid)
            QTest::newRow(qPrintable(QString::number(valid))) << valid;
    }
    void doubleRegisterInterpretation() {
        QFETCH(uint32_t, valid);
        auto input = fixture(false);
        input.inputs.constantBuffers[3] = {{{1, 0x3ff00000, 0, 0x80000000}, uint8_t(valid)},
                                           Value::known({0x12345678, 0x7ff81234, 0, 0xfff00000}),
                                           Value::known({1, 0, 0, 0})};
        QTemporaryDir directory;
        const auto path = directory.filePath("double.jsonl");
        exportGroupExecutionTrace(path, input);
        NativeExecutionDebugModel model(path, analysisFileDigest(path), input.bytecode);
        const auto raw = model.registerValues();
        const auto rows = model.registerRows("double");
        std::map<std::string, std::string> text;
        for (const auto &row : rows)
            text[row[0]] = row[1];
        QCOMPARE(text.at("cb3[0]"), std::string((valid & 3) == 3 ? "1.0000000000000002" : "unknown") + ", " +
                                        ((valid & 12) == 12 ? "-0.0" : "unknown"));
        QCOMPARE(text.at("cb3[1]"), std::string("nan, -inf"));
        QCOMPARE(text.at("cb3[2]"), std::string("5e-324, 0.0"));
        QCOMPARE(model.registerValues(), raw);
        const auto snapshot =
            model.inspect(NativeDebugCommand::Position, 0, model.configuration(), "double", true);
        QCOMPARE(snapshot.at("register_rows"), rows);
        QCOMPARE(snapshot.at("report").at("registers"), raw);
        model.move(model.size());
        QCOMPARE(model.registerRows("double").size(), model.registerValues().size());
        model.move(0);
        QCOMPARE(model.registerValues(), raw);
        QCOMPARE(model.registerRows("double"), rows);
    }
    void subroutineInputBoundaries() {
        using Words = std::vector<uint32_t>;
        const auto op = [](uint32_t opcode, Words operands = {}) {
            operands.insert(operands.begin(), opcode | (uint32_t(operands.size() + 1) << 24));
            return operands;
        };
        InputSnapshot input;
        input.stage = 5;
        input.event = 19;
        input.shader = 7;
        input.bytecode = makeDxbc(
            {{0x58454853, writeDxbcProgram({{0x50050, 0},
                                            {op(104, {4}), op(155, {1, 1, 1}), op(4, {0x10a000, 100}), op(62),
                                             op(44, {0x10a000, 100}), op(4, {0x10a000, 200}), op(62),
                                             op(44, {0x10a000, 200}), op(62)}})}});
        QTemporaryDir dir;
        const auto path = dir.filePath("calls.jsonl");
        exportGroupExecutionTrace(path, input);
        NativeExecutionDebugModel model(path, analysisFileDigest(path), input.bytecode);
        const Json instructions = {2, 4, 5, 7, 8, 6, 3};
        const Json stacks = {Json::array(), Json({3}), Json({3}),    Json({3, 6}),
                             Json({3, 6}),  Json({3}), Json::array()};
        QCOMPARE(model.size(), instructions.size());
        for (size_t i = 0; i < model.size(); ++i) {
            model.move(i);
            QCOMPARE(model.instructionState().at("instruction"), instructions[i]);
            QCOMPARE(model.instructionState().at("return_instruction_stack_before"), stacks[i]);
            QCOMPARE(model.instructionState().at("call_depth"), Json(stacks[i].size()));
            QVERIFY(model.stack().at("status") != "available");
        }
        for (size_t i = model.size(); i-- > 0;) {
            model.move(i);
            QCOMPARE(model.instructionState().at("return_instruction_stack_before"), stacks[i]);
        }
    }
    void steppingAndRawValues() {
        QTemporaryDir dir;
        auto input = fixture(false);
        const auto path = dir.filePath("trace.jsonl");
        GroupTraceOptions options;
        options.thread = {1, 0, 0};
        const auto summary = exportGroupExecutionTrace(path, input, options);
        NativeExecutionDebugModel model(path, analysisFileDigest(path), input.bytecode);
        QCOMPARE(model.size(), summary.at("selected_steps").get<size_t>());
        QCOMPARE(model.position(), size_t(0));
        QCOMPARE(model.instructionState().at("state_boundary"), Json("before_instruction"));
        QCOMPARE(model.instructionState().at("global_step"), Json(1));
        const auto initial = model.registerValues();
        bool constants = false, system = false;
        for (const auto &r : initial) {
            if (r.at("name") == "cb3[0]") {
                QCOMPARE(r.at("bits"), Json({0x80000000u, 0x7fc0abcdu, 0xffffffffu, 7u}));
                QCOMPARE(r.at("written"), Json({true, false, true, false}));
                constants = true;
            }
            if (r.at("name") == "vThreadIDInGroupFlattened") {
                QCOMPARE(r.at("bits"), Json({1, 0, 0, 0}));
                QCOMPARE(r.at("written"), Json({true, false, false, false}));
                system = true;
            }
        }
        QVERIFY(constants && system);
        for (const auto *mode : {"float", "int", "uint", "hex"}) {
            const auto rows = model.registerRows(mode);
            for (const auto &row : rows)
                if (row[0] == "cb3[0]") {
                    QVERIFY(QString::fromStdString(row[1]).contains("unknown"));
                    if (std::string(mode) == "hex")
                        QCOMPARE(row[1], Json("0x80000000, unknown, 0xffffffff, unknown"));
                    if (std::string(mode) == "int")
                        QCOMPARE(row[1], Json("-2147483648, unknown, -1, unknown"));
                }
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.registerRows("other"));
        model.move(model.size());
        QVERIFY(model.instructionState().at("instruction").is_null());
        QVERIFY(model.location().is_null());
        QCOMPARE(model.sourceValues(), Json::array());
        QCOMPARE(model.stack().at("status"), Json("terminal"));
        model.move(0);
        QCOMPARE(model.registerValues(), initial);
        const auto target = model.rows().at(1).at("instruction").get<uint64_t>();
        model.toggleInstruction(target);
        QCOMPARE(model.seek(0), size_t(1));
        model.toggleInstruction(target);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.seek(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.toggleInstruction(UINT64_MAX));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.move(model.size() + 1));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, model.move(model.size(), [] { return true; }));
        QCOMPARE(model.position(), size_t(0));
        QCOMPARE(model.registerValues(), initial);
        model.addWatch("missing_value + 1");
        QCOMPARE(model.watchResults()[0].at("status"), Json("unavailable"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.addWatch("missing_value + 1"));
        model.removeWatch(0);
        QCOMPARE(model.watchResults(), Json::array());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.removeWatch(0));
        QCOMPARE(model.exportReport().at("model").at("shader_sha256"), Json(sha256(input.bytecode)));
    }
    void sourceNavigationAndConfiguration() {
        QTemporaryDir dir;
        auto input = fixture();
        const auto path = dir.filePath("trace.jsonl");
        exportGroupExecutionTrace(path, input);
        NativeExecutionDebugModel model(path, analysisFileDigest(path), input.bytecode);
        QVERIFY(!model.files().empty());
        QVERIFY(model.result().at("source_variables").at("status") == "available");
        size_t mapped = 0, totalValues = 0, watchValues = 0;
        Json selectedPoint;
        for (size_t i = 0; i < model.size(); ++i) {
            model.move(i);
            const auto location = model.location();
            if (!location.is_null()) {
                ++mapped;
                selectedPoint = location;
            }
            for (const auto &v : model.sourceValues())
                if (v.at("name") == "total" && v.at("status") == "available") {
                    ++totalValues;
                    const auto before = model.registerValues();
                    const auto evaluated = model.evaluate(i, "total + 1");
                    QCOMPARE(evaluated.kind, std::string("uint"));
                    QCOMPARE(evaluated.text(), std::to_string(v.at("bits").get<uint32_t>() + 1));
                    QCOMPARE(model.registerValues(), before);
                    ++watchValues;
                }
        }
        QVERIFY(mapped > 0);
        QVERIFY(totalValues > 0 && watchValues > 0);
        std::vector<size_t> iterations;
        const SourceTrace::Point loopPoint{0, 6};
        for (size_t i = 0; i < model.size(); ++i)
            if (model.source().lineEntry(i, loopPoint))
                iterations.push_back(i);
        QCOMPARE(iterations.size(), size_t(3));
        model.source().setRule(loopPoint.first, loopPoint.second, "total >= 0", "equal", 2);
        QCOMPARE(model.seek(0), iterations[1]);
        QCOMPARE(model.source().encounterCount(iterations[2], loopPoint), size_t(3));
        model.move(iterations[2]);
        const auto visibleRegisters = model.registerValues();
        QCOMPARE(model.evaluate(iterations[1], "total").text(), std::string("10"));
        QCOMPARE(model.position(), iterations[2]);
        QCOMPARE(model.registerValues(), visibleRegisters);
        model.source().setRule(loopPoint.first, loopPoint.second, "total == 11", "always", 1);
        QCOMPARE(model.seek(0), iterations[2]);
        model.source().setRule(loopPoint.first, loopPoint.second, "missing_value > 0");
        QVERIFY_THROWS_EXCEPTION(ExpressionError, model.seek(0));
        QCOMPARE(model.position(), iterations[2]);
        QCOMPARE(model.registerValues(), visibleRegisters);
        model.source().toggle(loopPoint.first, loopPoint.second);
        model.move(0);
        const auto next = model.source().next(0, 1);
        QVERIFY(next > 0 && next < model.size());
        model.source().setRule(selectedPoint.at("file"), selectedPoint.at("line_start"), "total >= 0",
                               "at_least", 1);
        model.addWatch("total");
        model.toggleInstruction(model.rows().at(1).at("instruction"));
        const auto config = model.configuration();
        QCOMPARE(config.at("format"), Json("FloraGPA native CS execution debug config 1"));
        model.importConfiguration(config);
        QCOMPARE(model.configuration(), config);
        for (const auto *field : {"shader_sha256", "mappings_sha256"}) {
            auto corrupt = config;
            corrupt["identity"][field] = std::string(64, '0');
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.importConfiguration(corrupt));
            QCOMPARE(model.configuration(), config);
        }
        auto legacy = config;
        legacy["format"] = "FloraGPA debug config 1";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.importConfiguration(legacy));
        QCOMPARE(model.configuration(), config);
    }
    void shaderAndMappingIdentity_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto *name : {"shader", "instruction", "source", "mapping", "group"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void shaderAndMappingIdentity() {
        QFETCH(QString, mutation);
        QTemporaryDir dir;
        auto input = fixture();
        const auto path = dir.filePath("trace.jsonl");
        exportGroupExecutionTrace(path, input);
        if (mutation == "shader")
            input.bytecode.back() ^= 1;
        else {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            std::vector<Json> rows;
            while (!file.atEnd())
                rows.push_back(Json::parse(file.readLine().toStdString()));
            file.close();
            if (mutation == "source")
                rows[1]["data"]["issues"].push_back("Modified source metadata");
            else if (mutation == "group") {
                rows[0]["group_size"] = {1, 2, 1};
            } else {
                for (auto &row : rows)
                    if (row.at("record") == "instruction" && !row.at("declaration").get<bool>()) {
                        if (mutation == "mapping")
                            row["source"] = {{"file", 0}, {"line_start", 999}};
                        else
                            row["words"][0] = row["words"][0].get<uint32_t>() ^ 0x800;
                        break;
                    }
            }
            std::string raw;
            for (const auto &row : rows)
                raw += row.dump() + "\n";
            save(path, raw);
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 NativeExecutionDebugModel(path, analysisFileDigest(path), input.bytecode));
    }
};
QTEST_GUILESS_MAIN(NativeExecutionDebugModelTests)
#include "NativeExecutionDebugModelTests.moc"
