#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/PipelineCreation.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... v) {
    Raw b;
    (append(b, v), ...);
    return b;
}
Raw copy(Bytes bytes) { return Raw(bytes.begin(), bytes.end()); }
} // namespace
class PipelineCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void strictRecords() {
        QTemporaryDir dir;
        for (uint16_t t : {uint16_t(0x3580), uint16_t(0x3581), uint16_t(0x3584), uint16_t(0x3587),
                           uint16_t(0x3589), uint16_t(0x358a), uint16_t(0x358b), uint16_t(0x358c)}) {
            auto b = pack(Id(0), Id(2), int32_t(-1));
            if (isStateCreation(t)) {
                append(b, uint8_t(1));
                b.resize(b.size() + (t == 0x3589 ? 264 : t == 0x358b ? 40 : 52));
            } else {
                if (t == 0x3580) {
                    append(b, 1u);
                    append(b, uint8_t(1));
                    append(b, Id(UINT64_MAX));
                    append(b, std::array<uint32_t, 6>{});
                }
                append(b, uint64_t(4));
                append(b, uint8_t(1));
                append(b, 0x43425844u);
                if (t != 0x3580)
                    append(b, Id(0));
            }
            append(b, Id(0));
            auto c = readPipelineCreation(t, b);
            QCOMPARE(c.result, int32_t(-1));
            for (size_t n = 0; n < b.size(); ++n)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, Bytes(b).first(n)));
            auto extra = b;
            extra.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, extra));
            if (isStateCreation(t)) {
                auto invalid = b;
                invalid[20] = 2;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
            } else {
                auto invalid = b;
                put(invalid, t == 0x3580 ? 57 : 20, UINT64_MAX);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
            }
            for (auto hr : {int32_t(-1), int32_t(1), int32_t(2), int32_t(0)}) {
                auto bytes = b;
                put(bytes, 16, hr);
                Capture cap;
                cap.add(2, 5, 0x81, Raw(28));
                cap.add(100, 7, t, bytes);
                auto path = dir.filePath("creation.gpa_frame");
                cap.save(path);
                Frame frame(path.toStdWString());
                const auto audit = auditPipelineCreations(frame);
                QCOMPARE(audit.records.at(100).error.empty(), hr == -1 || hr == 1);
                QCOMPARE(inspectCommand(frame, 100)["status"], nlohmann::json("decoded"));
            }
        }
    }
    void cachedStateBeforeAndDuringCreation() {
        QTemporaryDir dir;
        D3D11_SAMPLER_DESC original{};
        original.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        original.AddressU = original.AddressV = original.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        original.MaxAnisotropy = 1;
        original.ComparisonFunc = D3D11_COMPARISON_NEVER;
        original.MaxLOD = D3D11_FLOAT32_MAX;
        ReplayOptions o;
        o.warp = true;
        Capture empty;
        empty.add(1, 5, 0x127, Raw(24));
        empty.add(2, 5, 0x81, Raw(28));
        auto path = dir.filePath("device.gpa_frame");
        empty.save(path);
        Frame deviceFrame(path.toStdWString());
        Replay native(deviceFrame, o);
        Com<ID3D11SamplerState> sampler;
        check(native.nativeDevice()->CreateSamplerState(&original, &sampler), "Native sampler reference");
        D3D11_SAMPLER_DESC canonical{};
        sampler->GetDesc(&canonical);
        auto build = [&](bool bad) {
            Capture c;
            c.add(1, 5, 0x127, Raw(24));
            c.add(2, 5, 0x81, Raw(28));
            auto saved = canonical;
            if (bad)
                saved.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            c.add(3, 5, 0x88, pack(Id(0), Id(2), saved));
            c.add(99, 7, 0x34e8, pack(Id(0), Id(1), 0u, 1u, uint8_t(1), Id(3)));
            auto create = pack(Id(0), Id(2), int32_t(0), uint8_t(1), original, Id(3));
            c.add(100, 7, 0x358c, create);
            c.add(101, 7, 0x358c, create);
            return c;
        };
        for (bool bad : {false, true}) {
            auto c = build(bad);
            const auto p = dir.filePath(bad ? "bad.gpa_frame" : "cached.gpa_frame");
            c.save(p);
            Frame f(p.toStdWString());
            Replay replay(f, o);
            if (bad) {
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            } else {
                replay.run();
                QCOMPARE(replay.counts.at("Device5.CreateSamplerState"), uint64_t(2));
                Com<ID3D11DeviceContext> ctx;
                replay.nativeDevice()->GetImmediateContext(&ctx);
                Com<ID3D11SamplerState> bound;
                ctx->PSGetSamplers(0, 1, &bound);
                QVERIFY(bound);
            }
        }
    }
    void originalCaptures_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int k = 0; k < 8; ++k)
            for (int v = 0; v < 4; ++v)
                for (bool w : {false, true})
                    QTest::newRow(
                        QString("%1-%2").arg(k * 10 + v).arg(w ? "warp" : "hardware").toUtf8().constData())
                        << k * 10 + v << w;
    }
    void originalCaptures() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_PIPELINE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_PIPELINE_CREATION_CAPTURES to the original corpus");
        Frame frame((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
        auto audit = auditPipelineCreations(frame);
        unsigned calls = 0, observations = 0, unmaterialized = 0;
        for (const auto &[id, c] : audit.records) {
            QVERIFY2(c.error.empty(), c.error.c_str());
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
            if (c.result)
                ++observations;
            else
                ++calls;
            if (!c.note.empty())
                ++unmaterialized;
        }
        QCOMPARE(calls, mode % 10 == 3 ? 2u : 1u);
        QCOMPARE(observations, mode % 10 == 1 || mode % 10 == 2 ? 1u : 0u);
        QCOMPARE(unmaterialized, mode == 33 ? 1u : 0u);
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        ReplayOptions o;
        o.warp = warp;
        Replay replay(frame, o);
        unsigned visited = 0;
        std::map<Id, IUnknown *> seen;
        replay.run({}, {}, [&](Id event, bool after, auto *, const auto &objects) {
            auto it = audit.records.find(event);
            if (it == audit.records.end() || !after)
                return;
            const auto &c = it->second;
            ++visited;
            if (c.result)
                return;
            if (!c.note.empty()) {
                QVERIFY(!objects.contains(c.resource));
                return;
            }
            QVERIFY(objects.contains(c.resource));
            if (isStateCreation(c.type) && seen.contains(c.resource))
                QCOMPARE(objects.at(c.resource).Get(), seen.at(c.resource));
            seen[c.resource] = objects.at(c.resource).Get();
        });
        QCOMPARE(visited, unsigned(audit.records.size()));
        if (mode / 10 == 2) {
            auto draw = std::find_if(frame.entries().begin(), frame.entries().end(), [](const auto &e) {
                return e.second.category == 7 && e.second.type == 0x35;
            });
            QVERIFY(draw != frame.entries().end());
            auto state = frame.state(frame.event(draw->first).state);
            Reader view(frame.payload(state.csUav[0]));
            view.skip(16);
            QCOMPARE(replay.readBuffer(view.read<Id>()), pack(123u));
        }
        if (mode == 33) {
            QCOMPARE(replay.counts.at("unmaterialized_layout_creations"), uint64_t(1));
            QVERIFY(validateFrame(frame.path()).contains("record_handling_overrides"));
        }
    }
    void originalAuditMutations() {
        auto root = qEnvironmentVariable("FLORA_PIPELINE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        QTemporaryDir dir;
        for (int mode : {0, 30, 40, 50, 60, 70}) {
            Frame original((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            const auto audit = auditPipelineCreations(original);
            const auto event = audit.records.begin()->first,
                       resource = audit.records.begin()->second.resource;
            for (int mutation = 0; mutation < 5; ++mutation) {
                Capture c;
                for (const auto &[id, e] : original.entries()) {
                    auto b = copy(original.payload(id));
                    if (id == event) {
                        if (mutation == 0)
                            put(b, 0, Id(1));
                        if (mutation == 1)
                            put(b, 8, Id(999999));
                        if (mutation == 2)
                            put(b, b.size() - 8, Id(0));
                        if (mutation == 3) {
                            if (mode == 0)
                                b[29] = 0;
                            else if (mode == 30)
                                put(b, 25, Id(0));
                            else
                                b[20] = 2;
                        }
                    }
                    uint16_t type = e.type;
                    if (id == resource && mutation == 4)
                        type = 0x83;
                    c.add(id, e.category, type, b);
                }
                auto path = dir.filePath("invalid.gpa_frame");
                c.save(path);
                Frame invalid(path.toStdWString());
                QVERIFY(!auditPipelineCreations(invalid).records.at(event).error.empty());
                QVERIFY(validateFrame(invalid.path())["errors"].get<unsigned>() > 0);
            }
            if (mode >= 40) {
                Capture c;
                for (const auto &[id, e] : original.entries()) {
                    auto b = copy(original.payload(id));
                    if (id == resource) {
                        uint32_t value = mode == 40 ? 1 : mode == 50 ? 0 : mode == 60 ? 2 : 1;
                        put(b, mode == 70 ? 20 : 16, value);
                    }
                    c.add(id, e.category, e.type, b);
                }
                auto path = dir.filePath("canonical.gpa_frame");
                c.save(path);
                Frame invalid(path.toStdWString());
                ReplayOptions o;
                o.warp = true;
                Replay replay(invalid, o);
                try {
                    replay.run();
                    QFAIL("Canonical mismatch must fail");
                } catch (const std::exception &error) {
                    QVERIFY(QString::fromUtf8(error.what())
                                .contains("does not reproduce saved canonical descriptor"));
                }
            }
        }
        Frame original((root + "/33/capture.gpa_frame").toStdWString());
        const auto audit = auditPipelineCreations(original);
        auto missing = std::find_if(audit.records.begin(), audit.records.end(),
                                    [](const auto &v) { return !v.second.note.empty(); });
        QVERIFY(missing != audit.records.end());
        Capture c;
        for (const auto &[id, e] : original.entries()) {
            auto b = copy(original.payload(id));
            if (e.category == 3 && e.type == 3)
                put(b, 144, missing->second.resource);
            c.add(id, e.category, e.type, b);
        }
        auto path = dir.filePath("required-layout.gpa_frame");
        c.save(path);
        Frame invalid(path.toStdWString());
        QVERIFY(!auditPipelineCreations(invalid).records.at(missing->first).error.empty());
        QVERIFY(validateFrame(invalid.path())["errors"].get<unsigned>() > 0);
    }
    void futureShaderUseRejected() {
        auto root = qEnvironmentVariable("FLORA_PIPELINE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        Frame f((root + "/0/capture.gpa_frame").toStdWString());
        Capture c;
        for (auto &[id, e] : f.entries())
            c.add(e.type == 0x3581 ? 100000 : id, e.category, e.type, copy(f.payload(id)));
        QTemporaryDir dir;
        auto p = dir.filePath("future.gpa_frame");
        c.save(p);
        Frame changed(p.toStdWString());
        ReplayOptions o;
        o.warp = true;
        Replay replay(changed, o);
        try {
            replay.run();
            QFAIL("Future shader must fail");
        } catch (const std::exception &e) {
            QVERIFY(QString::fromUtf8(e.what()).contains("unavailable before creation event 100000"));
        }
    }
};
QTEST_GUILESS_MAIN(PipelineCreationTests)
#include "PipelineCreationTests.moc"
