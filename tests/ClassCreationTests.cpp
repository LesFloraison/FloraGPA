#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/ClassCreation.h"
#include "core/InspectionRecords.h"
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
class ClassCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void originalCaptures_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode : {0, 1, 2, 3, 4, 10, 11, 12, 13, 20, 21, 22, 23})
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originalCaptures() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_CLASS_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        Frame frame((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
        auto audit = auditClassCreations(frame);
        for (const auto &[id, c] : audit.records) {
            QVERIFY2(c.error.empty(), c.error.c_str());
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
        }
        auto preflight = validateFrame(frame.path());
        QCOMPARE(preflight["errors"], nlohmann::json(0));
        QVERIFY(preflight.contains("class_linkage_aliases"));
        ReplayOptions o;
        o.warp = warp;
        Replay replay(frame, o);
        unsigned creations = 0, dispatches = 0, missing = 0;
        replay.run({}, {}, [&](Id event, bool after, auto *ctx, const auto &objects) {
            if (!after)
                return;
            if (auto it = audit.records.find(event); it != audit.records.end()) {
                const auto &c = it->second;
                ++creations;
                if (!c.note.empty()) {
                    QVERIFY(!objects.contains(c.resource));
                    ++missing;
                } else {
                    QVERIFY(objects.contains(c.canonicalResource));
                    if (c.resource != c.canonicalResource) {
                        QVERIFY(objects.contains(c.resource));
                        QCOMPARE(objects.at(c.resource).Get(), objects.at(c.canonicalResource).Get());
                    }
                }
            }
            if (frame.entry(event).type == 0x35) {
                auto state = frame.state(frame.event(event).state);
                Com<ID3D11ComputeShader> shader;
                ID3D11ClassInstance *raw[256]{};
                UINT count = 256;
                ctx->CSGetShader(&shader, raw, &count);
                QCOMPARE(count, 1u);
                Com<ID3D11ClassInstance> instance;
                instance.Attach(raw[0]);
                QVERIFY(instance);
                QCOMPARE(static_cast<IUnknown *>(instance.Get()),
                         objects.at(state.stages[5].classes[0]).Get());
                Com<ID3D11ClassLinkage> owner;
                instance->GetClassLinkage(&owner);
                QCOMPARE(static_cast<IUnknown *>(owner.Get()),
                         objects.at(readClassRecord(frame, state.stages[5].classes[0]).linkage).Get());
                Reader view(frame.payload(state.csUav[0]));
                view.skip(16);
                auto bytes = replay.readBuffer(view.read<Id>());
                unsigned expectedPhase = (mode == 13 || mode == 23) && dispatches == 0 ? 0 : 1;
                QFile oracle(root + QString("/%1/native/buffer%2.bin").arg(mode).arg(expectedPhase));
                QVERIFY(oracle.open(QIODevice::ReadOnly));
                QCOMPARE(QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())),
                         oracle.readAll());
                auto evidence = qEnvironmentVariable("FLORA_CLASS_CREATION_ARTIFACT_DIR");
                if (!evidence.isEmpty()) {
                    QVERIFY(QDir().mkpath(evidence));
                    QFile out(evidence + QString("/%1-%2-phase%3.bin")
                                             .arg(mode)
                                             .arg(warp ? "warp" : "hardware")
                                             .arg(expectedPhase));
                    QVERIFY(out.open(QIODevice::WriteOnly));
                    QCOMPARE(out.write(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())),
                             qsizetype(bytes.size()));
                }
                ++dispatches;
            }
        });
        QCOMPARE(creations, unsigned(audit.records.size()));
        QCOMPARE(dispatches, mode == 13 || mode == 23 ? 2u : 1u);
        QCOMPARE(missing, mode == 11 || mode == 12 || mode == 21 || mode == 22 ? 1u : 0u);
    }
    void wireBounds() {
        for (uint16_t t : {uint16_t(0x3588), uint16_t(0x3195), uint16_t(0x3196)}) {
            auto b = pack(Id(0), Id(2), int32_t(-1));
            if (t == 0x3195)
                append(b, 1u);
            if (t == 0x3196)
                append(b, std::array<uint32_t, 4>{2, 3, 0, 0});
            append(b, Id(0));
            QCOMPARE(readClassCreation(t, b).result, int32_t(-1));
            for (size_t n = 0; n < b.size(); n++)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readClassCreation(t, Bytes(b).first(n)));
            QTemporaryDir dir;
            for (int32_t hr : {int32_t(-1), int32_t(1), int32_t(2), int32_t(0)}) {
                auto wire = b;
                put(wire, 16, hr);
                Capture cap;
                cap.add(100, 7, t, wire);
                auto path = dir.filePath(QString("result-%1.gpa_frame").arg(hr));
                cap.save(path);
                Frame f(path.toStdWString());
                QCOMPARE(auditClassCreations(f).records.at(100).error.empty(), hr < 0 || hr == 1);
            }
            b.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readClassCreation(t, b));
        }
    }
    void observationBounds() {
        QTemporaryDir dir;
        for (uint16_t t :
             {uint16_t(0x3112), uint16_t(0x3113), uint16_t(0x3114), uint16_t(0x311a), uint16_t(0x311b),
              uint16_t(0x311c), uint16_t(0x318e), uint16_t(0x318f), uint16_t(0x3190)}) {
            auto b = pack(Id(0), Id(1));
            if (t == 0x3112 || t == 0x318e) {
                append(b, int32_t(0));
                b.resize(b.size() + 16);
                append(b, Id(1));
            } else if (t == 0x311a) {
                append(b, uint8_t(1));
                append(b, std::array<uint32_t, 8>{});
            } else if (t == 0x311b || t == 0x311c) {
                append(b, uint8_t(1));
                append(b, uint64_t(2));
                append(b, uint8_t(1));
                append(b, uint8_t('A'));
                append(b, uint8_t(0));
            } else
                append(b, 1u);
            QVERIFY(acceptPassiveObjectRecord(t, b));
            for (size_t n = 0; n < b.size(); n++)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptPassiveObjectRecord(t, Bytes(b).first(n)));
            auto bad = b;
            bad.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptPassiveObjectRecord(t, bad));
            if (t == 0x311a || t == 0x311b || t == 0x311c) {
                bad = b;
                bad[16] = 2;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptPassiveObjectRecord(t, bad));
            }
            Capture cap;
            cap.add(100, 7, t, b);
            auto path = dir.filePath(QString("observation-%1.gpa_frame").arg(t));
            cap.save(path);
            Frame frame(path.toStdWString());
            QCOMPARE(inspectCommand(frame, 100)["status"], nlohmann::json("decoded"));
        }
    }
    void negativeOriginals() {
        auto root = qEnvironmentVariable("FLORA_CLASS_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        QTemporaryDir dir;
        for (int mode : {0, 1, 10, 20}) {
            Frame original((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            const auto audit = auditClassCreations(original);
            auto chosen = std::find_if(audit.records.begin(), audit.records.end(),
                                       [](const auto &v) { return v.second.type != 0x3588; });
            QVERIFY(chosen != audit.records.end());
            auto event = chosen->first;
            auto c = chosen->second;
            for (int mutation = 0; mutation < 7; mutation++) {
                Capture cap;
                for (const auto &[id, e] : original.entries()) {
                    auto b = copy(original.payload(id));
                    if (id == event) {
                        if (mutation == 0)
                            put(b, 0, Id(1));
                        if (mutation == 1)
                            put(b, 8, Id(0));
                        if (mutation == 2)
                            put(b, b.size() - 8, Id(0));
                        if (mutation == 3)
                            put(b, 20, 999u);
                        if (mutation == 4)
                            put(b, 16, int32_t(1));
                    }
                    if (mutation == 5 && id == c.saved.namesData)
                        b.pop_back();
                    if (mutation == 6 && id == c.saved.linkage)
                        put(b, 8, Id(999999));
                    cap.add(id, e.category, e.type, b);
                }
                auto path = dir.filePath(QString("bad-%1-%2.gpa_frame").arg(mode).arg(mutation));
                cap.save(path);
                Frame f(path.toStdWString());
                auto a = auditClassCreations(f);
                QVERIFY2(!a.records.at(event).error.empty() ||
                             validateFrame(f.path())["errors"].get<unsigned>() > 0,
                         qPrintable(path));
            }
        }
    }
    void identityConflictAndMissingUse() {
        const auto root = qEnvironmentVariable("FLORA_CLASS_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        QTemporaryDir dir;
        for (bool conflict : {true, false}) {
            Frame original(
                (root + (conflict ? "/0/capture.gpa_frame" : "/11/capture.gpa_frame")).toStdWString());
            auto audit = auditClassCreations(original);
            auto chosen = std::find_if(audit.records.begin(), audit.records.end(), [&](const auto &v) {
                return conflict ? v.second.type == 0x3195 : !v.second.note.empty();
            });
            QVERIFY(chosen != audit.records.end());
            const auto c = chosen->second;
            Capture cap;
            for (const auto &[id, e] : original.entries()) {
                auto b = copy(original.payload(id));
                if (conflict && id == c.resource)
                    put(b, 8, Id(90000));
                if (!conflict && e.category == 3 && e.type == 3) {
                    auto state = original.state(id);
                    state.stages[5].classCount = 1;
                    state.stages[5].classes[0] = c.resource;
                    b = snapshot(state);
                }
                cap.add(id, e.category, e.type, b);
            }
            if (conflict) {
                Reader r(original.payload(c.canonicalOwner, 5, 0x97));
                r.skip(8);
                cap.add(90000, 5, 0x97, pack(Id(0), r.read<Id>()));
            }
            auto path = dir.filePath(conflict ? "identity-conflict.gpa_frame" : "missing-name-use.gpa_frame");
            cap.save(path);
            Frame f(path.toStdWString());
            auto result = auditClassCreations(f);
            QVERIFY(!result.records.at(chosen->first).error.empty());
            QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
            ReplayOptions o;
            o.warp = true;
            Replay replay(f, o);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
    }
    void futureCreationRejected() {
        auto root = qEnvironmentVariable("FLORA_CLASS_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        QTemporaryDir dir;
        for (int mode : {0, 10, 20}) {
            Frame original((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            auto audit = auditClassCreations(original);
            auto event = audit.records.begin()->first;
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
};
QTEST_GUILESS_MAIN(ClassCreationTests)
#include "ClassCreationTests.moc"
