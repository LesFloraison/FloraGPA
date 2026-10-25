#include "application/DxbcCheckpointModel.h"
#include "application/DxbcExecution.h"
#include "application/DxbcOutputLog.h"
#include "application/DxbcSystemId.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QtTest>
#include <d3dcompiler.h>
using namespace flora;
using Json = nlohmann::json;
class DxbcFormatTests final : public QObject {
    Q_OBJECT
  private slots:
    void samplePositionPadding_data() {
        QTest::addColumn<uint32_t>("sample");
        QTest::addColumn<bool>("vertex");
        for (uint32_t sample : {0u, 1u, 3u, 4u, 5u})
            for (bool vertex : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(sample).arg(vertex))) << sample << vertex;
    }
    void samplePositionPadding() {
        QFETCH(uint32_t, sample);
        QFETCH(bool, vertex);
        try {
            const std::string signature = vertex ? "float4 main(uint id:SV_VertexID):SV_Position{"
                                                 : "float4 main(uint id:SV_PrimitiveID):SV_Target{";
            const auto index = sample == 4 ? "index" : sample == 5 ? "id" : std::to_string(sample);
            const auto compiled =
                compileHlsl("Texture2DMS<float> t:register(t0);cbuffer Params{uint index;}" + signature +
                                "return float4(t.GetSamplePosition(" + index + "),id,1);}",
                            vertex ? "vs_5_0" : "ps_5_0")
                    .bytecode;
            dxbc_exec::Program accepted(compiled);
            bool found = false;
            for (auto parts = readDxbcParts(compiled); auto &[tag, bytes] : parts) {
                if (tag != 0x58454853 && tag != 0x52444853)
                    continue;
                auto code = readDxbcProgram(bytes);
                for (auto &instruction : code.instructions) {
                    if ((instruction[0] & 2047) != 110)
                        continue;
                    found = true;
                    const auto parsed = checkpoint::instructionOperands(instruction);
                    QCOMPARE(parsed.ranges.size(), size_t(3));
                    const auto end = parsed.ranges.back().second;
                    QVERIFY(dxbc_detail::samplePositionPadding(instruction, end));
                    if (sample < 4)
                        QCOMPARE(instruction[end - 1], sample);
                    auto malformed = instruction;
                    malformed.back() = 1;
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpoint::instructionOperands(malformed));
                    malformed = instruction;
                    malformed.resize(end - 1);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpoint::instructionOperands(malformed));
                    malformed = instruction;
                    malformed[0] = (malformed[0] & ~2047u) | 54;
                    QVERIFY(!dxbc_detail::samplePositionPadding(malformed, end));
                    malformed = instruction;
                    malformed.resize(parsed.ranges.back().first);
                    malformed.insert(malformed.end(), {0x4002, 0, 0, 0, 0, 0});
                    QVERIFY(!dxbc_detail::samplePositionPadding(malformed, malformed.size() - 1));
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpoint::instructionOperands(malformed));
                    qInfo().noquote()
                        << QString::fromStdString(Json{{"samplepos_tokens", instruction}}.dump());
                    instruction.back() = 1;
                    bytes = writeDxbcProgram(code);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, dxbc_exec::Program(makeDxbc(parts)));
                }
            }
            QVERIFY(found);
            const auto transformed =
                vertex ? instrumentOutputWrites(offsetSystemId(compiled, 7, 6).bytes, 7, 32).bytes : compiled;
            Com<ID3DBlob> text;
            check(D3DDisassemble(transformed.data(), transformed.size(), 0, nullptr, &text),
                  "Validate transformed sample positions");
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void feedbackOperands_data() {
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<uint32_t>("operands");
        QTest::addColumn<QString>("expression");
        QTest::addColumn<QString>("profile");
        QTest::newRow("bias-clamp-status")
            << 231u << 7u << QString("tex.SampleBias(samp, uv, bias, int2(1,2), clampLod, status)")
            << QString("ps_5_0");
        QTest::newRow("gradient-clamp-status")
            << 232u << 8u << QString("tex.SampleGrad(samp, uv, dx, dy, int2(1,2), clampLod, status)")
            << QString("ps_5_0");
        QTest::newRow("compare-clamp-status")
            << 233u << 7u << QString("tex.SampleCmp(compareSampler, uv, bias, int2(1,2), clampLod, status)")
            << QString("ps_5_0");
        QTest::newRow("vertex-gradient-clamp-status")
            << 232u << 8u << QString("tex.SampleGrad(samp, uv, dx, dy, int2(1,2), clampLod, status)")
            << QString("vs_5_0");
    }
    void feedbackOperands() {
        QFETCH(uint32_t, opcode);
        QFETCH(uint32_t, operands);
        QFETCH(QString, expression);
        QFETCH(QString, profile);
        try {
            const auto signature =
                profile == "vs_5_0"
                    ? "float4 main(uint id:SV_VertexID):SV_Position{float2 uv=float2(id,id+1);"
                    : "float4 main(float2 uv:TEXCOORD0):SV_Target{";
            const auto source =
                std::string("Texture2D<float4> tex:register(t0);SamplerState samp:register(s0);"
                            "SamplerComparisonState compareSampler:register(s1);"
                            "cbuffer Params{float bias;float clampLod;float2 dx;float2 dy;}") +
                signature + "uint status;float4 value=" + expression.toStdString() +
                ";return value+float(CheckAccessFullyMapped(status));}";
            const auto compiled = compileHlsl(source, profile.toStdString()).bytecode;
            bool found = false;
            for (const auto &[tag, bytes] : readDxbcParts(compiled)) {
                if (tag != 0x52444853 && tag != 0x58454853)
                    continue;
                for (const auto &row : readDxbcProgram(bytes).instructions) {
                    if ((row[0] & 2047) != opcode)
                        continue;
                    QVERIFY(!found);
                    found = true;
                    size_t pos = 1;
                    for (auto extended = row[0] >> 31; extended;)
                        extended = row.at(pos++) >> 31;
                    uint32_t actual = 0;
                    std::set<unsigned> unused;
                    while (pos < row.size()) {
                        pos = dxbc_detail::operand(row, pos, unused, 0, false);
                        ++actual;
                    }
                    QCOMPARE(pos, row.size());
                    QCOMPARE(actual, operands);
                    qInfo().noquote() << QString::fromStdString(
                        Json{{"opcode", opcode}, {"operands", actual}, {"tokens", row}}.dump());
                    const auto parsed = checkpoint::instructionOperands(row);
                    QCOMPARE(parsed.ranges.size(), size_t(operands));
                    if (profile == "vs_5_0") {
                        const auto offset = offsetSystemId(compiled, 7, 6);
                        QCOMPARE(offset.metadata.at("changed"), Json(true));
                        const auto logged = instrumentOutputWrites(offset.bytes, 7, 32);
                        bool preserved = false;
                        for (const auto &[modifiedTag, modified] : readDxbcParts(logged.bytes)) {
                            if (modifiedTag != tag)
                                continue;
                            for (const auto &instruction : readDxbcProgram(modified).instructions)
                                if ((instruction[0] & 2047) == opcode) {
                                    QCOMPARE(instruction, row);
                                    preserved = true;
                                }
                        }
                        QVERIFY(preserved);
                        Com<ID3DBlob> transformed;
                        check(D3DDisassemble(logged.bytes.data(), logged.bytes.size(), 0, nullptr,
                                             &transformed),
                              "Validate transformed feedback instruction layout");
                    }
                    auto truncated = row;
                    truncated.pop_back();
                    QVERIFY_THROWS_EXCEPTION(std::exception, checkpoint::instructionOperands(truncated));
                }
            }
            QVERIFY(found);
            Com<ID3DBlob> disassembly;
            check(D3DDisassemble(compiled.data(), compiled.size(), 0, nullptr, &disassembly),
                  "Disassemble independent feedback operand probe");
            const std::string text(static_cast<const char *>(disassembly->GetBufferPointer()),
                                   disassembly->GetBufferSize());
            const auto mnemonic = opcode == 231   ? "sample_b_cl_s"
                                  : opcode == 232 ? "sample_d_cl_s"
                                                  : "sample_c_cl_s";
            QVERIFY(text.find(mnemonic) != std::string::npos);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcFormatTests)
#include "DxbcFormatTests.moc"
