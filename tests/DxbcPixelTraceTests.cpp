#include "application/DxbcExecutionTrace.h"
#include "application/DxbcExecutionTraceReader.h"
#include "application/DxbcPixelTraceInputs.h"
#include "application/HlslCompilation.h"
#include "application/NativeExecutionDebugModel.h"
#include "core/Dxbc.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
InputSnapshot fixture() {
    InputSnapshot input;
    input.stage = 4;
    input.event = 19;
    input.shader = 7;
    input.bytecode = compileHlsl("uint4 main(nointerpolation uint4 v:DATA,uint c:SV_Coverage):SV_Target{"
                                 "if(v.x==2)discard;return uint4(v.x+1,v.y^c,v.z,v.w);}",
                                 "ps_5_0")
                         .bytecode;
    return input;
}
PixelTraceOptions options(uint32_t lane, uint8_t mask) {
    PixelTraceOptions o;
    o.selectedLane = lane;
    o.activeMask = mask;
    o.inputProvenance = {{"source", "synthetic-explicit-quad"}};
    for (uint32_t i = 0; i < 4; ++i) {
        o.registers[i][{1, {0}}] = Value::known({i, 0x80000000, 0x7fc0abcd, 0xffffffff});
        o.registers[i][{35, {}}] = {{1, 0, 0, 0}, 1};
    }
    return o;
}
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read pixel trace fixture");
    return file.readAll();
}
std::string digest(const QByteArray &bytes) {
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())));
}
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write pixel trace corruption");
}
} // namespace
class DxbcPixelTraceTests : public QObject {
    Q_OBJECT
  private slots:
    void helperMemoryJournal() {
        QTemporaryDir dir;
        auto input = fixture();
        input.bytecode =
            compileHlsl("RWByteAddressBuffer data:register(u1);"
                        "uint4 main():SV_Target{uint old;data.InterlockedAdd(0,1,old);DeviceMemoryBarrier();"
                        "return uint4(old,data.Load(4),7,9);}",
                        "ps_5_0")
                .bytecode;
        const auto path = dir.filePath("memory.jsonl");
        const auto report = exportPixelExecutionTrace(path, input, options(0, 0));
        QCOMPARE(report.at("final_active_mask"), Json(0));
        {
            ExecutionTraceReader reader(path, digest(read(path)));
            reader.moveTo(reader.size());
            for (unsigned i = 0; i < 4; ++i) {
                const auto output = reader.registerValue(i, {2, {0}});
                QCOMPARE(output.valid, uint8_t(12));
                QCOMPARE(output.bits[2], 7u);
                QCOMPARE(output.bits[3], 9u);
            }
            reader.moveTo(0);
        }
        bool fence = false;
        QByteArray changed;
        for (const auto &line : read(path).split('\n')) {
            if (line.isEmpty())
                continue;
            auto row = Json::parse(line.constData(), line.constData() + line.size());
            if (row.at("record") == "step" && row.at("sync_flags") == 0x4000) {
                fence = true;
                row["sync_flags"] = 0;
            }
            changed += QByteArray::fromStdString(row.dump()) + '\n';
        }
        QVERIFY(fence);
        const auto corrupt = dir.filePath("missing-fence.jsonl");
        write(corrupt, changed);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ExecutionTraceReader(corrupt, digest(changed)));
    }
    void previousPixelModel() {
        QTemporaryDir dir;
        const auto path = dir.filePath("previous.jsonl");
        exportPixelExecutionTrace(path, fixture(), options(0, 15));
        auto contents = read(path);
        QVERIFY(contents.contains("coherent-pixel-memory/3"));
        contents.replace("coherent-pixel-memory/3", "coherent-pixel-sampling/2");
        write(path, contents);
        ExecutionTraceReader reader(path, digest(contents));
        reader.moveTo(reader.size());
        reader.moveTo(0);
    }
    void roundtrip_data() {
        QTest::addColumn<uint32_t>("lane");
        QTest::addColumn<uint32_t>("mask");
        for (uint32_t lane = 0; lane < 4; ++lane)
            for (uint32_t mask : {0u, 5u, 15u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(lane).arg(mask))) << lane << mask;
    }
    void roundtrip() {
        QFETCH(uint32_t, lane);
        QFETCH(uint32_t, mask);
        QTemporaryDir dir;
        const auto path = dir.filePath("pixel.jsonl");
        const auto input = fixture();
        const auto report = exportPixelExecutionTrace(path, input, options(lane, uint8_t(mask)));
        QCOMPARE(report.at("final_active_mask"), Json(mask & ~4u));
        ExecutionTraceReader reader(path, digest(read(path)));
        const auto original = reader.registers();
        QCOMPARE(reader.header().at("stage"), Json("ps"));
        QVERIFY(reader.selectedSize() > 0);
        reader.moveTo(reader.size());
        for (uint32_t i = 0; i < 4; ++i)
            QCOMPARE(reader.registerValue(i, {2, {0}}),
                     Value::known({i + 1, 0x80000001, 0x7fc0abcd, 0xffffffff}));
        reader.moveTo(0);
        QVERIFY(reader.registers() == original);
        reader.moveTo(reader.size());
        reader.moveTo(0);
        QVERIFY(reader.registers() == original);
        NativeExecutionDebugModel model(path, digest(read(path)), input.bytecode);
        QCOMPARE(model.result().at("action"), Json("debug-pixel"));
        QCOMPARE(model.result().at("state_boundary"), Json("before_selected_instruction_or_terminal_quad"));
        model.move(model.size());
        QCOMPARE(model.instructionState().at("state_boundary"), Json("terminal_quad"));
        model.move(0);
        model.addWatch("missing_value + 1");
        model.toggleInstruction(model.rows().at(0).at("instruction"));
        const auto config = model.configuration();
        QCOMPARE(config.at("format"), Json("FloraGPA native PS execution debug config 1"));
        model.importConfiguration(config);
        QCOMPARE(model.configuration(), config);
        auto wrong = config;
        wrong["format"] = "FloraGPA native CS execution debug config 1";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.importConfiguration(wrong));
    }
    void rejectsCorruption_data() {
        QTest::addColumn<QString>("change");
        for (const auto *name : {"helper", "active", "terminal", "selected", "model", "scope"})
            QTest::newRow(name) << QString(name);
    }
    void rejectsCorruption() {
        QFETCH(QString, change);
        QTemporaryDir dir;
        const auto path = dir.filePath("pixel.jsonl");
        exportPixelExecutionTrace(path, fixture(), options(0, 15));
        QByteArray changed;
        bool applied = false;
        for (const auto &line : read(path).split('\n')) {
            if (line.isEmpty())
                continue;
            auto row = Json::parse(line.toStdString());
            const auto kind = row.at("record");
            if (!applied && kind == "step" && (change == "helper" || change == "active")) {
                if (change == "helper")
                    row["helper_before"] = !row.at("helper_before").get<bool>();
                else
                    row["active_mask_after"] = row.at("active_mask_after").get<unsigned>() ^ 1;
                applied = true;
            }
            if (kind == "complete" && change == "terminal") {
                row["final_active_mask"] = 15;
                applied = true;
            }
            if (kind == "header") {
                if (change == "selected") {
                    row["selected_lane"] = 4;
                    applied = true;
                }
                if (change == "model") {
                    row["execution_model"] = "unknown";
                    applied = true;
                }
                if (change == "scope") {
                    row["scope"] = "one_vertex";
                    applied = true;
                }
            }
            changed += QByteArray::fromStdString(row.dump()) + '\n';
        }
        QVERIFY(applied);
        const auto corrupt = dir.filePath("corrupt.jsonl");
        write(corrupt, changed);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ExecutionTraceReader(corrupt, digest(changed)));
    }
    void joinsObservedInputs() {
        const auto code =
            compileHlsl(
                "uint4 main(nointerpolation uint4 a:A,nointerpolation uint4 b:B):SV_Target{return a+b;}",
                "ps_5_0")
                .bytecode;
        std::map<uint32_t, PixelRasterInputs> passes;
        for (uint32_t reg = 0; reg < 2; ++reg) {
            PixelRasterInputs p;
            PixelInputRecord r{};
            r.words[4] = {3, 2, 1, 0};
            r.words[6] = {1, 2, 0, 1};
            r.words[8] = {0, 1, 0x3e800000, 1};
            for (uint32_t lane = 0; lane < 4; ++lane)
                r.words[lane] = {reg, lane, 0x7fc0abcd, 0xffffffff};
            p.records.push_back(r);
            p.identity = {{"backend", "dxbc-pixel-raster-inputs/2"},
                          {"shader_sha256", sha256(code)},
                          {"records", 1},
                          {"input_register", reg},
                          {"valid_mask", 15},
                          {"sample_frequency", false},
                          {"scope", "isolated_primitive_pre_depth_inputs"},
                          {"original_fragment_eligibility", false},
                          {"adapter_luid", 42},
                          {"adapter_vendor", 4318},
                          {"adapter_device", 1},
                          {"feature_level", 0xb000},
                          {"warp", false},
                          {"x", 3},
                          {"y", 2},
                          {"sample", 1},
                          {"layer", 0},
                          {"raster_samples", 4},
                          {"target_samples", 4},
                          {"raster_state", Json::object()},
                          {"geometry",
                           {{"primitive_ordinal", 7}, {"vertices_per_primitive", 3}, {"source_stride", 32}}}};
            passes.emplace(reg, std::move(p));
        }
        const auto joined = pixelTraceInputs(code, passes);
        QCOMPARE(joined.activeMask, uint8_t(11)); // Pixel-frequency neighbors may cover another sample.
        QCOMPARE(joined.selectedLane, 1u);
        QCOMPARE(joined.inputProvenance.at("selected_raster"), Json({{"depth_bits", 0x3e800000},
                                                                     {"front_face", true},
                                                                     {"coverage_mask", 2},
                                                                     {"sample", 1},
                                                                     {"lane", 1}}));
        QCOMPARE(joined.registers[3].at({1, {1}}), Value::known({1, 3, 0x7fc0abcd, 0xffffffff}));
        for (const auto *field : {"adapter_luid", "valid_mask", "shader_sha256", "x"}) {
            auto bad = passes;
            bad[1].identity[field] = 0;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        }
        auto bad = passes;
        bad[0].identity["backend"] = "dxbc-pixel-raster-inputs/1";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        for (unsigned component : {2u, 3u}) {
            bad = passes;
            bad[1].records[0].words[8][component] ^= 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        }
        bad = passes;
        bad[0].records[0].words[8][3] = bad[1].records[0].words[8][3] = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        bad = passes;
        bad[1].records[0].words[6][0] ^= 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        bad = passes;
        bad[1].records.push_back(bad[1].records.front());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        bad = passes;
        bad.erase(1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelTraceInputs(code, bad));
        const auto sampled =
            compileHlsl("float4 main(sample float4 a:A):SV_Target{return a;}", "ps_5_0").bytecode;
        passes.erase(1);
        passes[0].identity["shader_sha256"] = sha256(sampled);
        passes[0].identity["sample_frequency"] = true;
        passes[0].records[0].words[5] = {1, 1, 1, 1};
        QCOMPARE(pixelTraceInputs(sampled, passes).activeMask, uint8_t(2));
    }
    void inputFreeAndAtomicFailure() {
        QTemporaryDir dir;
        auto input = fixture();
        input.bytecode = compileHlsl("uint4 main():SV_Target{return uint4(1,2,3,4);}", "ps_5_0").bytecode;
        PixelTraceOptions o;
        const auto path = dir.filePath("constant.jsonl");
        exportPixelExecutionTrace(path, input, o);
        ExecutionTraceReader reader(path, digest(read(path)));
        reader.moveTo(reader.size());
        for (uint32_t lane = 0; lane < 4; ++lane)
            QCOMPARE(reader.registerValue(lane, {2, {0}}), Value::known({1, 2, 3, 4}));
        o.maxSteps = 1;
        const auto failure = dir.filePath("failure.jsonl");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportPixelExecutionTrace(failure, input, o));
        QVERIFY(!QFileInfo::exists(failure));
        o.maxSteps = 1000;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 exportPixelExecutionTrace(failure, input, o, [] { return true; }));
        QVERIFY(!QFileInfo::exists(failure));
        o.maxBytes = 64;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportPixelExecutionTrace(failure, input, o));
        QVERIFY(!QFileInfo::exists(failure));
    }
};
QTEST_GUILESS_MAIN(DxbcPixelTraceTests)
#include "DxbcPixelTraceTests.moc"
