#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/Dxbc.h"
#include "core/ResourceLod.h"
#include <QTemporaryDir>
#include <QtTest>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::testing;
namespace {
QString fixture(int mode) {
    const auto root = qEnvironmentVariable(mode < 6 ? "FLORA_MINLOD_CAPTURES" : "FLORA_MINLOD_BOUNDARIES");
    return root.isEmpty() ? QString{} : root + QString("/%1/").arg(mode);
}
std::vector<uint8_t> bytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing LOD oracle");
    auto a = f.readAll();
    return {reinterpret_cast<const uint8_t *>(a.data()),
            reinterpret_cast<const uint8_t *>(a.data()) + a.size()};
}
void copy(const Frame &frame, const QString &path, Id edited, const std::vector<uint8_t> &replacement) {
    Capture out;
    for (const auto &[id, e] : frame.entries()) {
        auto raw = frame.payload(id);
        out.add(id, e.category, e.type,
                id == edited ? replacement : std::vector<uint8_t>(raw.begin(), raw.end()));
    }
    out.save(path);
}
} // namespace
#include "ResourceLodSwitchTests.h"
class ResourceLodTests final : public QObject {
    Q_OBJECT
  private slots:
    void mipCountPrograms_data() {
        QTest::addColumn<QString>("profile");
        QTest::addColumn<int>("mode");
        for (const auto profile : {"ps_4_0", "ps_4_1", "ps_5_0"})
            for (int mode = 0; mode < 4; ++mode)
                QTest::newRow(qPrintable(QString("%1-%2").arg(profile).arg(mode)))
                    << QString(profile) << mode;
    }
    void mipCountPrograms() {
        QFETCH(QString, profile);
        QFETCH(int, mode);
        const std::array<std::string, 4> expressions{"n", "w", "n+tex.Load(int3(0,0,0)).x",
                                                     "n+other.Load(int3(0,0,0)).x"};
        const auto source = "Texture2D<float4> tex:register(t3);Texture2D<float4> other:register(t5);"
                            "float4 main():SV_Target{uint w,h,n;tex.GetDimensions(0,w,h,n);return " +
                            expressions[mode] + ";}";
        Com<ID3DBlob> code, errors, stripped;
        check(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main",
                         profile.toLatin1().constData(), 0, 0, &code, &errors),
              "Compile mip-count proof");
        check(D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                             D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped),
              "Strip mip-count proof");
        for (auto blob : {code.Get(), stripped.Get()}) {
            const Bytes data(static_cast<const uint8_t *>(blob->GetBufferPointer()), blob->GetBufferSize());
            const auto declared = shaderSrvDeclarations(data), required = shaderSrvLodDependencies(data);
            QVERIFY(declared[3]);
            QCOMPARE(required[3], mode == 1 || mode == 2);
            QCOMPARE(required[5], mode == 3);
        }
    }
    void mipCountOriginals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 28; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp))) << mode << warp;
    }
    void mipCountOriginals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = qEnvironmentVariable(mode < 6 ? "FLORA_MIP_COUNT_CAPTURES" : mode < 12 ? "FLORA_SM40_MIP_CAPTURES" : mode < 20 ? "FLORA_BRANCH_MIP_CAPTURES" : "FLORA_SWITCH_MIP_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_MIP_COUNT_CAPTURES for original mip-count captures");
        const auto folder = root + QString("/%1/").arg(mode);
        Frame frame((folder + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        QVERIFY(audit.initial.empty());
        QVERIFY(!audit.clamped.empty());
        const auto behavior = mode % 6;
        const bool missing = mode < 12 ? behavior == 2 || behavior == 4 : mode < 20 ? mode >= 14 && mode <= 16 : mode >= 22 && mode <= 24;
        QCOMPARE(audit.issues.size(), size_t(missing));
        QCOMPARE(validateFrame(frame.path())["errors"].get<unsigned>(), unsigned(missing));
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            if (missing) {
                QVERIFY_EXCEPTION_THROWN(replay.run(), std::runtime_error);
            } else {
                replay.run();
                QCOMPARE(replay.output().rgba, bytes(folder + "hardware/expected.rgba"));
            }
        }
        if ((mode < 12 && (behavior == 0 || behavior == 4)) || mode == 12 || mode == 16) {
            Id shader = 0;
            for (const auto &[id, e] : frame.entries())
                if (e.category == 7 && isDraw(e.type))
                    shader = frame.state(frame.event(id).state).stages[4].shader;
            QVERIFY(shader);
            const auto base = mode < 6 ? 0 : mode < 12 ? 6 : 12;
            options.shaders[shader] = bytes(root + QString("/%1/hardware/%2.dxbc")
                                                       .arg(base + (behavior == 0 ? 4 : 0))
                                                       .arg(mode < 12 ? (behavior == 0 ? "dependent" : "count") : (behavior == 0 ? "branchSample" : "branched")));
            Replay edited(frame, options);
            if (behavior == 0) {
                QVERIFY_EXCEPTION_THROWN(edited.run(), std::runtime_error);
            } else {
                edited.run();
                QCOMPARE(edited.output().rgba, bytes(root + QString("/%1/hardware/expected.rgba").arg(base)));
            }
        }
        if (mode == 20 || mode == 24) {
            Id shader = 0;
            for (const auto &[id, e] : frame.entries())
                if (e.category == 7 && isDraw(e.type))
                    shader = frame.state(frame.event(id).state).stages[4].shader;
            QVERIFY(shader);
            options.shaders[shader] = bytes(root + (mode == 20 ? "/24/hardware/switchSample.dxbc" : "/20/hardware/switched.dxbc"));
            Replay edited(frame, options);
            if (mode == 20) QVERIFY_EXCEPTION_THROWN(edited.run(), std::runtime_error);
            else {
                edited.run();
                QCOMPARE(edited.output().rgba, bytes(root + "/20/hardware/expected.rgba"));
            }
        }
    }
    void sm40SwitchProofBounds() { exerciseSm40SwitchProofBounds(); }
    void sm40BranchProofBounds() {
        const auto root = qEnvironmentVariable("FLORA_BRANCH_MIP_CAPTURES");
        if (root.isEmpty()) QSKIP("Set original SM4.0 branch capture directory");
        const auto source = bytes(root + "/12/hardware/branched.dxbc");
        QVERIFY(!shaderSrvLodDependencies(source)[0]);
        for (size_t length = 0; length < source.size(); ++length)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvLodDependencies(Bytes(source).first(length)));
        auto parts = readDxbcParts(source);
        auto found = std::find_if(parts.begin(), parts.end(), [](auto &p) { return p.first == 0x52444853; });
        QVERIFY(found != parts.end());
        const auto original = readDxbcProgram(found->second);
        auto indexOf = [&](unsigned op) {
            for (size_t i = 0; i < original.instructions.size(); ++i)
                if ((original.instructions[i][0] & 2047) == op) return i;
            throw std::runtime_error("Missing branch proof instruction");
        };
        const auto branch = indexOf(31), alternative = indexOf(18), join = indexOf(21);
        auto required = [&](const DxbcProgram &program) {
            auto code = writeDxbcProgram(program);
            found->second = code;
            return shaderSrvLodDependencies(makeDxbc(parts))[0];
        };
        for (int mode = 0; mode < 18; ++mode) {
            auto p = original;
            auto &r = p.instructions[branch];
            if (mode == 0) r[1] = 0x0010001a; // IF reads still-live y.
            if (mode == 1) p.instructions[branch + 1][1] = 0x00100012; // Only then x overwritten; y survives join.
            if (mode == 2) p.instructions[indexOf(56)][1] = 0x00100042; // Else no longer overwrites y.
            if (mode == 3) p.instructions.erase(p.instructions.begin() + join);
            if (mode == 4) p.instructions[alternative] = {0x01000015}; // Orphan second ENDIF.
            if (mode == 5) p.instructions.insert(p.instructions.begin() + alternative, {0x01000012});
            if (mode == 6) r[1] = 0x00100006; // Four-component condition, not select-one.
            if (mode == 7) r[1] |= 0x80000000u;
            if (mode == 8) r[1] |= 1u << 22;
            if (mode == 9) r[2] = 4096;
            if (mode == 10) r[0] |= 1u << 13;
            if (mode == 11) { r.push_back(0); r[0] += 1u << 24; }
            if (mode == 12) { r.pop_back(); r[0] -= 1u << 24; }
            if (mode == 13) p.instructions[alternative][0] |= 1u << 18;
            if (mode == 14) p.instructions.insert(p.instructions.begin(), {0x01000015});
            if (mode == 15) p.instructions.insert(p.instructions.begin() + branch + 1, {0x0100003e});
            if (mode == 16) {
                p.instructions.insert(p.instructions.begin() + branch, 64, original.instructions[branch]);
                p.instructions.insert(p.instructions.end() - 1, 64, {0x01000015});
            }
            if (mode == 17) {
                // Without ELSE the false path retains y, even though the true
                // path overwrites it. A linear scan would unsafely drop it.
                p.instructions.erase(p.instructions.begin() + alternative, p.instructions.begin() + join);
            }
            QVERIFY2(required(p), qPrintable(QString("branch mutation %1").arg(mode)));
        }
        auto flipped = original;
        flipped.instructions[branch][0] ^= 1u << 18; // Both outcomes have the same non-use proof.
        QVERIFY(!required(flipped));
        auto optional = original;
        // An optional branch is safe if its false edge is also followed by an
        // unconditional overwrite before the queried lanes are consumed.
        optional.instructions.erase(optional.instructions.begin() + alternative, optional.instructions.begin() + join);
        optional.instructions.insert(optional.instructions.begin() + alternative + 1,
                                     {0x08000036, 0x00100032, 0, 0x00004002, 0, 0, 0, 0});
        QVERIFY(!required(optional));
    }
    void sm40DimensionProofBounds() {
        const auto root = qEnvironmentVariable("FLORA_SM40_MIP_CAPTURES");
        if (root.isEmpty()) QSKIP("Set original SM4.0 mip-count capture directory");
        const auto source = bytes(root + "/6/hardware/count.dxbc");
        QVERIFY(!shaderSrvLodDependencies(source)[0]);
        for (size_t length = 0; length < source.size(); ++length)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvLodDependencies(Bytes(source).first(length)));
        auto parts = readDxbcParts(source);
        auto found = std::find_if(parts.begin(), parts.end(), [](auto &p) { return p.first == 0x52444853; });
        QVERIFY(found != parts.end());
        const auto original = readDxbcProgram(found->second);
        auto findOp = [](DxbcProgram &program, unsigned op) -> std::vector<uint32_t> & {
            auto found = std::find_if(program.instructions.begin(), program.instructions.end(),
                                       [op](auto &row) { return (row[0] & 2047) == op; });
            if (found == program.instructions.end()) throw std::runtime_error("Missing proof instruction");
            return *found;
        };
        for (int mode = 0; mode < 18; ++mode) {
            auto changed = original;
            auto &conversion = findOp(changed, 86);
            // Each candidate preserves a dimension dependency or leaves the
            // checked straight-line grammar. No edited file is an original.
            if (mode == 0) conversion[3] = 0x0010000a; // Read x before overwriting x.
            if (mode == 1) conversion[1] = 0x00100062; // Overwrite yz; later MUL still reads old x.
            if (mode == 2) conversion[3] |= 0x80000000u;
            if (mode == 3) conversion[3] |= 1u << 22;
            if (mode == 4) conversion[4] = 4096;
            if (mode == 5) conversion[0] |= 0x80000000u;
            if (mode == 6) conversion[0] = (conversion[0] & ~2047u) | 58u; // Unverified NOP shape.
            if (mode == 7) conversion[1] = 0x00100006u; // Source-swizzle operand in destination.
            if (mode == 8) { conversion.push_back(0); conversion[0] += 1u << 24; }
            if (mode == 9) { conversion.pop_back(); conversion[0] -= 1u << 24; }
            if (mode == 10) changed.instructions.pop_back(); // Missing return.
            if (mode == 11) changed.instructions.push_back({0x0100003eu}); // Multiple returns.
            if (mode == 12) changed.instructions.insert(changed.instructions.begin() + 3, {0x01000030u}); // LOOP.
            if (mode == 13) changed.instructions.back() = {0x0200003eu, 0};
            if (mode == 14) conversion[3] = 0x00208001u; // Unproved CB/relative source.
            if (mode == 15) {
                auto &mul = findOp(changed, 56);
                mul[5] = 0x00004002u; // Four-literal token with one saved literal.
            }
            if (mode == 16) changed.instructions.insert(changed.instructions.begin(), 257, original.instructions[0]);
            if (mode == 17) changed.header[0] = (changed.header[0] & 0xffff0000) | 0x51;
            const auto program = writeDxbcProgram(changed);
            found->second = program;
            QVERIFY2(shaderSrvLodDependencies(makeDxbc(parts))[0], qPrintable(QString::number(mode)));
        }
        const auto program = writeDxbcProgram(original);
        found->second = program;
        parts.emplace_back(0x45434649, Bytes{});
        QVERIFY(shaderSrvLodDependencies(makeDxbc(parts))[0]);
    }
    void mipCountProofBounds() {
        const auto root = qEnvironmentVariable("FLORA_MIP_COUNT_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original mip-count capture directory");
        const auto source = bytes(root + "/0/hardware/count.dxbc");
        QVERIFY(!shaderSrvLodDependencies(source)[0]);
        for (size_t n = 0; n < source.size(); ++n)
            QVERIFY_EXCEPTION_THROWN(shaderSrvLodDependencies(Bytes(source).first(n)), std::runtime_error);
        auto parts = readDxbcParts(source);
        auto found = std::find_if(parts.begin(), parts.end(), [](auto &p) { return p.first == 0x58454853; });
        QVERIFY(found != parts.end());
        const auto original = readDxbcProgram(found->second);
        auto info = std::find_if(original.instructions.begin(), original.instructions.end(),
                                 [](auto &r) { return (r[0] & 2047) == 61; });
        QVERIFY(info != original.instructions.end());
        const auto index = size_t(info - original.instructions.begin());
        for (int mode = 0; mode < 9; ++mode) {
            auto changed = original;
            auto &r = changed.instructions[index];
            QCOMPARE(r.size(), size_t(9));
            if (mode == 0)
                r[7] &= ~0x30u; // x instead of mip-count w.
            if (mode == 1)
                r[7] |= 0x80000000u;
            if (mode == 2)
                r[7] |= 1u << 22;
            if (mode == 3)
                r[8] = 128;
            if (mode == 4)
                r[1] = (r[1] & ~63u) | 4; // Unknown opcode extension.
            if (mode == 5)
                r[0] = (r[0] & ~2047u) | 206; // Unknown instruction.
            if (mode == 6)
                changed.header[0] = (changed.header[0] & 0xffff0000) | 0x51;
            if (mode == 7) {
                r[3] |= 0x20;
            } // Also writes a dimension lane.
            if (mode == 8) {
                r[5] = 0x00208001;
            } // Unverified mip operand form.
            const auto program = writeDxbcProgram(changed);
            found->second = program;
            QVERIFY(shaderSrvLodDependencies(makeDxbc(parts))[0]);
        }
        const auto program = writeDxbcProgram(original);
        found->second = program;
        parts.emplace_back(0x45434649, Bytes{});
        QVERIFY(shaderSrvLodDependencies(makeDxbc(parts))[0]);
    }
    void compiledSrvDeclarations() {
        const std::string source =
            "Texture2D<float4> tex:register(t3); ByteAddressBuffer raw:register(t5);"
            "StructuredBuffer<uint> structured:register(t127); RWStructuredBuffer<uint> dst:register(u0);"
            "[numthreads(1,1,1)] void "
            "main(){dst[0]=asuint(tex.Load(int3(0,0,0)).x)^raw.Load(0)^structured[0];}";
        Com<ID3DBlob> code, errors, stripped;
        check(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                         &code, &errors),
              "Compile declaration counterexample");
        check(D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                             D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped),
              "Strip declaration counterexample");
        for (auto blob : {code.Get(), stripped.Get()}) {
            auto used = shaderSrvDeclarations(
                Bytes(static_cast<const uint8_t *>(blob->GetBufferPointer()), blob->GetBufferSize()));
            QCOMPARE(std::count(used.begin(), used.end(), true), 3);
            QVERIFY(used[3] && used[5] && used[127]);
        }
    }
    void shaderUsage_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 10; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("usage-%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void shaderUsage() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = qEnvironmentVariable("FLORA_MINLOD_USAGE");
        if (root.isEmpty())
            QSKIP("Set original LOD shader usage captures");
        const auto folder = root + QString("/%1/").arg(mode);
        Frame frame((folder + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        QVERIFY(audit.initial.empty());
        QVERIFY(audit.records.empty());
        QVERIFY(!audit.clamped.empty());
        const bool sampled = mode == 4 || mode == 5 || mode == 8;
        const auto validation = validateFrame(frame.path());
        QCOMPARE(validation["errors"].get<int>(), sampled ? 1 : 0);
        QCOMPARE(audit.issues.size(), sampled ? size_t(1) : size_t(0));
        std::vector<Id> draws;
        Id ps = 0;
        for (const auto &[id, entry] : frame.entries())
            if (entry.category == 7 && isDraw(entry.type)) {
                draws.push_back(id);
                ps = frame.state(frame.event(id).state).stages[4].shader;
            }
        QVERIFY(!draws.empty());
        if (sampled)
            QCOMPARE(audit.issues.front().event, draws.back());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            if (sampled) {
                try {
                    replay.run();
                    QFAIL("Missing sampled LOD must not be fabricated");
                } catch (const std::runtime_error &e) {
                    QVERIFY(
                        QString::fromUtf8(e.what()).contains(QString("Event %1 (Draw)").arg(draws.back())));
                    QVERIFY(QString::fromUtf8(e.what()).contains("initial minimum LOD is unresolved"));
                }
            } else {
                replay.run();
                QCOMPARE(replay.output().rgba, bytes(folder + "native/expected.rgba"));
            }
        }
        // Replay decisions must follow the actual replacement shader, not the
        // original capture's reflection or the offline preflight result.
        if (mode == 3 || mode == 5) {
            options.shaders[ps] =
                bytes(root + (mode == 3 ? "/4/native/sampled.dxbc" : "/3/native/constant.dxbc"));
            Replay experiment(frame, options);
            if (mode == 3)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, experiment.run());
            else {
                experiment.run();
                QCOMPARE(experiment.output().rgba, bytes(root + "/3/native/expected.rgba"));
            }
        }
    }
    void shaderDeclarationBounds() {
        const auto root = qEnvironmentVariable("FLORA_MINLOD_USAGE");
        if (root.isEmpty())
            QSKIP("Set original LOD shader usage captures");
        const auto source = bytes(root + "/4/native/sampled.dxbc");
        const auto used = shaderSrvDeclarations(source);
        QVERIFY(used[0]);
        QCOMPARE(std::count(used.begin(), used.end(), true), 1);
        const auto none = shaderSrvDeclarations(bytes(root + "/3/native/constant.dxbc"));
        QCOMPARE(std::count(none.begin(), none.end(), true), 0);
        for (size_t length = 0; length < source.size(); ++length)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvDeclarations(Bytes(source).first(length)));
        auto parts = readDxbcParts(source);
        auto found =
            std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == 0x58454853; });
        QVERIFY(found != parts.end());
        const auto original = readDxbcProgram(found->second);
        auto declared = std::find_if(original.instructions.begin(), original.instructions.end(),
                                     [](const auto &r) { return (r[0] & 0x7ff) == 88; });
        QVERIFY(declared != original.instructions.end());
        const auto index = size_t(declared - original.instructions.begin());
        for (int bad = 0; bad < 5; ++bad) {
            auto changed = original;
            auto &row = changed.instructions[index];
            if (bad == 0)
                row[2] = 128;
            if (bad == 1)
                row[1] |= 0x80000000u;
            if (bad == 2)
                row[1] ^= 1u << 12; // Wrong operand kind.
            if (bad == 3)
                row[1] |= 1u << 22; // Non-immediate index.
            if (bad == 4)
                row.pop_back();
            const auto program = writeDxbcProgram(changed);
            found->second = program;
            const auto rebuilt = makeDxbc(parts);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvDeclarations(rebuilt));
        }
        auto future = original;
        future.header[0] = (future.header[0] & 0xffff0000) | 0x51;
        const auto futureBytes = writeDxbcProgram(future);
        found->second = futureBytes;
        auto conservative = shaderSrvDeclarations(makeDxbc(parts));
        QCOMPARE(std::count(conservative.begin(), conservative.end(), true), 128);
        const auto program = writeDxbcProgram(original);
        found->second = program;
        parts.emplace_back(0x45434649, Bytes{});
        conservative = shaderSrvDeclarations(makeDxbc(parts));
        QCOMPARE(std::count(conservative.begin(), conservative.end(), true), 128);
        parts.emplace_back(0x52444853, program);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvDeclarations(makeDxbc(parts)));
        DxbcParts emptyProgram{{0x58454853, Bytes{}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, shaderSrvDeclarations(makeDxbc(emptyProgram)));
    }
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 12; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = fixture(mode);
        if (root.isEmpty())
            QSKIP("Set original resource LOD corpora");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        const auto validation = validateFrame(frame.path());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        if (mode == 6) {
            QVERIFY(!audit.issues.empty());
            QVERIFY(validation["errors"] != 0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            return;
        }
        QVERIFY2(audit.issues.empty(), audit.issues.empty() ? "" : audit.issues.front().reason.c_str());
        QVERIFY2(validation["errors"] == 0, validation.dump(2).c_str());
        if (mode == 5 || mode == 7 || mode == 8) {
            QCOMPARE(audit.initial.size(), size_t(1));
            QCOMPARE(audit.initial.begin()->second.value, mode == 8 ? 0.0f : 1.0f);
        } else
            QVERIFY(audit.initial.empty());
        for (const auto &[id, record] : audit.records)
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
        const auto expected = bytes(root + "native/expected.rgba");
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(replay.output().rgba, expected);
            unsigned checks = 0;
            replay.run({}, {}, [&](Id id, bool after, auto *context, const auto &objects) {
                auto it = audit.records.find(id);
                if (!after || it == audit.records.end() || !it->second.setter)
                    return;
                Com<ID3D11Resource> resource;
                check(objects.at(it->second.resource).As(&resource), "LOD observation resource");
                QCOMPARE(context->GetResourceMinLOD(resource.Get()), it->second.value);
                ++checks;
            });
            QCOMPARE(checks, unsigned(std::count_if(audit.records.begin(), audit.records.end(),
                                                    [](const auto &v) { return v.second.setter; })));
            QCOMPARE(replay.output().rgba, expected);
        }
        if (mode == 8 || mode == 10) {
            for (const auto &[id, record] : audit.records)
                if (record.setter && record.value == 1)
                    options.disabled.insert(id);
            QVERIFY(!options.disabled.empty());
            Replay disabled(frame, options);
            disabled.run();
            const auto image = disabled.output().rgba;
            QVERIFY(image != expected);
            for (size_t p = 0; p < image.size(); p += 4) {
                QCOMPARE(image[p], uint8_t(255));
                QCOMPARE(image[p + 1], uint8_t(0));
                QCOMPARE(image[p + 2], uint8_t(0));
                QCOMPARE(image[p + 3], uint8_t(255));
            }
        }
        if (mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 7 || mode == 10 || mode == 11)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.readTexture(*audit.clamped.begin()));
        else {
            std::vector<uint8_t> expectedStorage;
            const std::array<std::array<uint8_t, 4>, 4> colors{
                {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}}};
            for (unsigned mip = 0; mip < 4; ++mip)
                for (unsigned pixel = 0; pixel < (8u >> mip) * (8u >> mip); ++pixel)
                    expectedStorage.insert(expectedStorage.end(), colors[mip].begin(), colors[mip].end());
            QCOMPARE(replay.readTexture(*audit.clamped.begin()), expectedStorage);
        }
        if (mode == 1) {
            for (const auto &[id, record] : audit.records)
                if (record.setter)
                    options.disabled.insert(id);
            Replay unknown(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.run());
        }
    }
    void opaquePrefix() {
        const auto root = fixture(7);
        if (root.isEmpty())
            QSKIP("Set original resource LOD boundaries");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        QVERIFY(!auditResourceLod(frame).initial.empty());
        Capture changed;
        bool replaced = false;
        for (const auto &[id, e] : frame.entries()) {
            const auto raw = frame.payload(id);
            auto type = e.type;
            if (!replaced && e.category == 7) {
                type = 0x7ffe;
                replaced = true;
            }
            changed.add(id, e.category, type, {raw.begin(), raw.end()});
        }
        QVERIFY(replaced);
        QTemporaryDir directory;
        const auto path = directory.path() + "/opaque.gpa_frame";
        changed.save(path);
        Frame opaque(path.toStdWString());
        const auto audit = auditResourceLod(opaque);
        QVERIFY(audit.initial.empty());
        QVERIFY(!audit.issues.empty());
    }
    void malformed() {
        const auto root = fixture(1);
        if (root.isEmpty())
            QSKIP("Set original resource LOD corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        QTemporaryDir temp;
        for (const auto &[id, record] : audit.records) {
            const auto payload = frame.payload(id);
            const auto type = frame.entry(id).type;
            for (size_t length = 0; length < payload.size(); ++length)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResourceLod(type, payload.first(length)));
            auto extra = std::vector<uint8_t>(payload.begin(), payload.end());
            extra.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResourceLod(type, extra));
            for (int bad = 0; bad < 9; ++bad) {
                auto edited = std::vector<uint8_t>(payload.begin(), payload.end());
                const auto value = record.setter ? 24u : 16u;
                const auto resource = record.setter ? 16u : 20u;
                if (bad == 0)
                    put(edited, 0, Id(1));
                if (bad == 1)
                    put(edited, 8, Id(0));
                if (bad == 2)
                    put(edited, resource, Id(0));
                if (bad == 3)
                    put(edited, resource, record.context);
                if (bad == 4)
                    put(edited, value, 0x7fc00000u);
                if (bad == 5)
                    put(edited, value, 0x7f800000u);
                if (bad == 6)
                    put(edited, value, -1.0f);
                if (bad == 7)
                    put(edited, value, 1000.0f);
                if (bad == 8)
                    edited.pop_back();
                const auto path = temp.path() + QString("/%1-%2.gpa_frame").arg(id).arg(bad);
                copy(frame, path, id, edited);
                Frame changed(path.toStdWString());
                QVERIFY(validateFrame(changed.path())["errors"] != 0);
                ReplayOptions options;
                options.warp = true;
                Replay replay(changed, options);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            }
        }
    }
    void malformedResourceKeepsDiagnostics() {
        const auto root = fixture(1);
        if (root.isEmpty())
            QSKIP("Set original resource LOD corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        const auto resource = *audit.clamped.begin();
        const auto raw = frame.payload(resource);
        QTemporaryDir directory;
        const auto path = directory.path() + "/bad-resource.gpa_frame";
        copy(frame, path, resource, {raw.begin(), raw.begin() + 17});
        const auto validation = validateFrame(path.toStdWString());
        QVERIFY(validation["errors"] != 0);
        QVERIFY2(validation["completed"] == true, validation.dump(2).c_str());
        QCOMPARE(validation["scanned_records"].get<size_t>(), frame.entries().size());
    }
};
QTEST_GUILESS_MAIN(ResourceLodTests)
#include "ResourceLodTests.moc"
