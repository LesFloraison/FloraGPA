#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/PipelineCreation.h"
#include <QDir>
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
class GeometryCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void originalCaptures_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int k = 0; k < 5; ++k)
            for (int v = 0; v < 4; ++v)
                for (bool w : {false, true})
                    QTest::newRow(
                        QString("%1-%2").arg(k * 10 + v).arg(w ? "warp" : "hardware").toUtf8().constData())
                        << k * 10 + v << w;
    }
    void originalCaptures() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_GEOMETRY_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_GEOMETRY_CREATION_CAPTURES to the original corpus");
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
        QCOMPARE(unmaterialized, (mode == 33 || mode == 43) ? 1u : 0u);
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        ReplayOptions o;
        o.warp = warp;
        Replay replay(frame, o);
        unsigned visited = 0, verifiedDraws = 0;
        std::map<Id, IUnknown *> seen;
        replay.run({}, {}, [&](Id event, bool after, auto *context, const auto &objects) {
            if (after && frame.entry(event).type == 0x37) {
                Com<IUnknown> bound;
                if (mode / 10 == 1) {
                    Com<ID3D11HullShader> shader;
                    context->HSGetShader(&shader, nullptr, nullptr);
                    bound = shader;
                } else if (mode / 10 == 2) {
                    Com<ID3D11DomainShader> shader;
                    context->DSGetShader(&shader, nullptr, nullptr);
                    bound = shader;
                } else {
                    Com<ID3D11GeometryShader> shader;
                    context->GSGetShader(&shader, nullptr, nullptr);
                    bound = shader;
                }
                QVERIFY(bound);
                const auto state = frame.state(frame.event(event).state);
                const auto id = state.stages[mode / 10 == 1 ? 1 : mode / 10 == 2 ? 2 : 3].shader;
                QVERIFY(seen.contains(id));
                QCOMPARE(bound.Get(), seen.at(id));
                ++verifiedDraws;
            }
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
        QCOMPARE(verifiedDraws, 1u);
        if (mode / 10 >= 3) {
            auto draw = std::find_if(frame.entries().begin(), frame.entries().end(), [](const auto &e) {
                return e.second.category == 7 && e.second.type == 0x37;
            });
            QVERIFY(draw != frame.entries().end());
            const auto state = frame.state(frame.event(draw->first).state);
            QCOMPARE(state.soCount, 2u);
            for (unsigned i = 0; i < 2; i++) {
                QFile file(root + QString("/%1/native/expected-buffer%2.bin").arg(mode).arg(i));
                QVERIFY(file.open(QIODevice::ReadOnly));
                auto bytes = file.readAll();
                auto result = replay.readBuffer(state.so[i]);
                const auto evidence = qEnvironmentVariable("FLORA_GEOMETRY_ARTIFACT_DIR");
                if (!evidence.isEmpty()) {
                    QVERIFY(QDir().mkpath(evidence));
                    QFile dump(
                        evidence +
                        QString("/%1-%2-buffer%3.bin").arg(mode).arg(warp ? "warp" : "hardware").arg(i));
                    QVERIFY(dump.open(QIODevice::WriteOnly));
                    QCOMPARE(
                        dump.write(reinterpret_cast<const char *>(result.data()), qsizetype(result.size())),
                        qsizetype(result.size()));
                }
                QCOMPARE(QByteArray(reinterpret_cast<const char *>(result.data()), qsizetype(result.size())),
                         bytes);
            }
            for (const auto &[id, c] : audit.records)
                if (!c.result && c.note.empty()) {
                    QCOMPARE(c.streamOutput.strides, (std::vector<uint32_t>{16, 20}));
                    QCOMPARE(c.firstStride.value(), 16u);
                }
        }
        if ((mode == 33 || mode == 43)) {
            QCOMPARE(replay.counts.at("unmaterialized_so_creations"), uint64_t(1));
            QVERIFY(validateFrame(frame.path()).contains("record_handling_overrides"));
        }
    }
    void strictRecords() {
        for (uint16_t t : {uint16_t(0x3582), uint16_t(0x3583), uint16_t(0x3585), uint16_t(0x3586)}) {
            auto b = pack(Id(0), Id(2), int32_t(-1), uint64_t(4), uint8_t(1), 0x43425844u);
            if (t == 0x3583) {
                append(b, 2u);
                append(b, uint8_t(1));
                for (unsigned i = 0; i < 2; i++) {
                    auto e = pack(i, 0u, Id(123), 0u, uint8_t(0), uint8_t(4), uint8_t(i), uint8_t(0));
                    b.insert(b.end(), e.begin(), e.end());
                }
                append(b, uint8_t(1));
                append(b, 16u);
                append(b, 2u);
                append(b, 0u);
            }
            append(b, Id(0));
            append(b, Id(0));
            QCOMPARE(readPipelineCreation(t, b).result, int32_t(-1));
            for (size_t n = 0; n < b.size(); n++)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, Bytes(b).first(n)));
            auto invalid = b;
            invalid.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
            invalid = b;
            put(invalid, 20, UINT64_MAX);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
            invalid = b;
            invalid[28] = 2;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
            if (t == 0x3583) {
                for (size_t offset : {size_t(37), size_t(86)}) {
                    invalid = b;
                    invalid[offset] = 2;
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
                }
                invalid = b;
                put(invalid, 33, 513u);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
                invalid = b;
                put(invalid, 91, 5u);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineCreation(t, invalid));
            }
        }
    }
    void futureCreationRejected() {
        const auto root = qEnvironmentVariable("FLORA_GEOMETRY_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        QTemporaryDir dir;
        for (int mode : {0, 10, 20, 30, 40}) {
            Frame original((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            const auto audit = auditPipelineCreations(original);
            const auto event = audit.records.begin()->first;
            Capture cap;
            for (const auto &[id, e] : original.entries())
                cap.add(id == event ? 100000 : id, e.category, e.type, copy(original.payload(id)));
            auto path = dir.filePath(QString("future-%1.gpa_frame").arg(mode));
            cap.save(path);
            Frame f(path.toStdWString());
            ReplayOptions o;
            o.warp = true;
            Replay replay(f, o);
            bool rejected = false;
            try {
                replay.run();
            } catch (const std::exception &error) {
                rejected = std::string(error.what()).find("unavailable before creation event 100000") !=
                           std::string::npos;
            }
            QVERIFY(rejected);
        }
    }
    void originalMutations() {
        auto root = qEnvironmentVariable("FLORA_GEOMETRY_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        QTemporaryDir dir;
        for (int mode : {0, 10, 20, 30, 40}) {
            Frame original((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            const auto audit = auditPipelineCreations(original);
            const auto event = audit.records.begin()->first;
            const auto &create = audit.records.begin()->second;
            for (int mutation = 0; mutation < (mode >= 30 ? 10 : 5); mutation++) {
                Capture cap;
                for (const auto &[id, e] : original.entries()) {
                    auto b = copy(original.payload(id));
                    if (id == event) {
                        if (mutation == 0)
                            put(b, 0, Id(1));
                        if (mutation == 1)
                            put(b, 8, Id(999999));
                        if (mutation == 2)
                            put(b, b.size() - 8, Id(0));
                        if (mutation == 3)
                            b[29] = 0;
                        const size_t tail = 29 + create.bytecodeLength;
                        if (mutation == 5)
                            put(b, tail + 5 + 8, Id(0));
                        if (mutation == 6)
                            put(b, tail + 5 + 2 * 24 + 1, 17u);
                        if (mutation == 7)
                            put(b, tail + 5 + 2 * 24 + 1 + 4, 1u);
                        if (mutation == 8)
                            put(b, tail + 5 + 2 * 24 + 1 + 8, 1u);
                    }
                    if (mutation == 9 && id == create.streamOutputId)
                        b.pop_back();
                    cap.add(id, e.category, mutation == 4 && id == create.resource ? 0x83 : e.type, b);
                }
                auto path = dir.filePath(QString("negative-%1-%2.gpa_frame").arg(mode).arg(mutation));
                cap.save(path);
                Frame f(path.toStdWString());
                QVERIFY2(!auditPipelineCreations(f).records.at(event).error.empty(), qPrintable(path));
                QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
            }
        }
        // A snapshot using an unmaterialized SO identity must fail before GPU execution.
        Frame original((root + "/33/capture.gpa_frame").toStdWString());
        auto audit = auditPipelineCreations(original);
        const auto event = audit.records.begin()->first;
        const auto resource = audit.records.begin()->second.resource;
        Capture cap;
        for (const auto &[id, e] : original.entries()) {
            auto b = copy(original.payload(id));
            if (e.category == 3 && e.type == 3)
                put(b, 668 + 3 * 3324 + 240, resource);
            cap.add(id, e.category, e.type, b);
        }
        auto path = dir.filePath("missing-so.gpa_frame");
        cap.save(path);
        Frame f(path.toStdWString());
        QVERIFY(!auditPipelineCreations(f).records.at(event).error.empty());
    }
};
QTEST_GUILESS_MAIN(GeometryCreationTests)
#include "GeometryCreationTests.moc"
