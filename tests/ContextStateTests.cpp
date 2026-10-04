#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/ContextStateRecords.h"
#include "core/PipelineGetters.h"
#include "core/ReplayCapabilities.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw wire(uint16_t type) {
    Raw raw(16);
    put(raw, 8, Id(3));
    if (type == 0x3561) {
        put(raw, 8, Id(2));
        append(raw, Id(0));
        append(raw, Id(0));
    } else if (type == 0x359b) {
        append(raw, uint32_t(0));
    } else {
        append(raw, int32_t(0));
        append(raw, uint32_t(0));
        append(raw, uint32_t(1));
        append(raw, uint8_t(1));
        append(raw, uint32_t(0xb000));
        append(raw, uint32_t(7));
        raw.resize(raw.size() + 16);
        append(raw, uint8_t(1));
        append(raw, uint32_t(0xb000));
        append(raw, Id(0));
    }
    return raw;
}
QString original(int mode) {
    auto root = qEnvironmentVariable("FLORA_CONTEXT_STATES");
    return root.isEmpty() ? QString{} : root + QString("/%1/capture.gpa_frame").arg(mode);
}
Raw fileBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing context-state image oracle");
    const auto b = file.readAll();
    return {reinterpret_cast<const uint8_t *>(b.data()),
            reinterpret_cast<const uint8_t *>(b.data()) + b.size()};
}
} // namespace
class ContextStateTests final : public QObject {
    Q_OBJECT
  private slots:
    void wireBounds_data() {
        QTest::addColumn<int>("type");
        for (int type : {0x3561, 0x359b, 0x35a4})
            QTest::newRow(qPrintable(QString::number(type, 16))) << type;
    }
    void wireBounds() {
        QFETCH(int, type);
        const auto raw = wire(type);
        QCOMPARE(readContextStateRecord(type, raw).type, uint16_t(type));
        for (size_t n = 0; n < raw.size(); ++n)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readContextStateRecord(type, Bytes(raw).first(n)));
        auto extra = raw;
        extra.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readContextStateRecord(type, extra));
    }
    void invalidLengthsAndFlags() {
        auto raw = wire(0x35a4);
        for (size_t offset : {size_t(28), size_t(53)}) {
            auto bad = raw;
            bad[offset] = 2;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readContextStateRecord(0x35a4, bad));
        }
        for (uint32_t count : {0u, 65u, 0xffffffffu}) {
            auto bad = raw;
            put(bad, 24, count);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readContextStateRecord(0x35a4, bad));
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readContextStateRecord(0xffff, raw));
        // A null feature-level pointer and omitted chosen-level output are wire observations,
        // even when an invalid API call had a nonzero declared count.
        raw.erase(raw.begin() + 54, raw.begin() + 58);
        raw[53] = 0;
        raw.erase(raw.begin() + 29, raw.begin() + 33);
        raw[28] = 0;
        auto decoded = readContextStateRecord(0x35a4, raw);
        QVERIFY(!decoded.hasLevels && !decoded.chosenLevel && decoded.levels.empty());
        QVERIFY(!contextStateReplayGap(decoded).empty());
    }
    void identityIsNotAState() {
        for (auto type : {0x3561, 0x35a4}) {
            auto raw = wire(type);
            auto decoded = readContextStateRecord(type, raw);
            QCOMPARE(std::string(contextStateGapKind(decoded)),
                     std::string("context_state_identity_unresolved"));
            put(raw, type == 0x3561 ? 16 : raw.size() - 8, Id(9));
            decoded = readContextStateRecord(type, raw);
            QCOMPARE(std::string(contextStateGapKind(decoded)), std::string("implementation_gap"));
            QVERIFY(!contextStateReplayGap(decoded).empty());
            QCOMPARE(std::string(replayCapability(type).handling), std::string("unsupported"));
        }
        auto failed = wire(0x35a4);
        put(failed, 16, int32_t(0x80070057));
        QCOMPARE(std::string(contextStateGapKind(readContextStateRecord(0x35a4, failed))),
                 std::string("implementation_gap"));
    }
    void originals_data() {
        QTest::addColumn<int>("mode");
        for (int mode = 0; mode < 8; ++mode)
            QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void originals() {
        QFETCH(int, mode);
        const auto path = original(mode);
        if (path.isEmpty())
            QSKIP("Set FLORA_CONTEXT_STATES to the unmodified original corpus");
        Frame frame(path.toStdWString());
        size_t swaps = 0, creations = 0, flags = 0;
        nlohmann::json states = nlohmann::json::array();
        for (const auto &[id, e] : frame.entries()) {
            if (e.category != 7)
                continue;
            if (isDraw(e.type)) {
                const auto event = frame.event(id);
                const auto state = frame.state(event.state);
                states.push_back(
                    {{"event", id}, {"state", event.state}, {"pixel_shader", state.stages[4].shader}});
            }
            if (!isContextStateRecord(e.type))
                continue;
            const auto record = readContextStateRecord(e.type, frame.payload(id));
            validateContextStateOwner(frame, id, record);
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
            if (e.type == 0x3561) {
                ++swaps;
                QCOMPARE(record.state, Id(0));
                QCOMPARE(record.previous, Id(0));
            } else if (e.type == 0x35a4) {
                ++creations;
                QCOMPARE(record.result, 0);
                QCOMPARE(record.state, Id(0));
                QCOMPARE(record.sdk, 7u);
                QCOMPARE(record.levels, std::vector<uint32_t>{0xb000});
                QCOMPARE(record.chosenLevel, std::optional<uint32_t>(0xb000));
            } else {
                ++flags;
                QVERIFY(record.link != 0);
                QCOMPARE(record.flags, 0u);
            }
        }
        QCOMPARE(swaps, size_t(mode == 0 ? 0 : mode == 5 ? 3 : mode == 6 ? 1 : 2));
        QCOMPARE(creations, size_t(mode == 3 || mode == 4));
        QCOMPARE(flags, creations);
        if (mode == 1 || mode == 5 || mode == 7) {
            // The producer verifies green (then blue for mode 5), but all saved Draw PS IDs are red.
            for (const auto &state : states)
                QCOMPARE(state["pixel_shader"], nlohmann::json(17));
            const auto getter = inspectCommand(frame, 23);
            bool missingObservedShader = false;
            for (const auto &reference : getter["references"])
                if (reference["id"] == 24 && reference["exists"] == false)
                    missingObservedShader = true;
            QVERIFY(missingObservedShader);
        }
        if (mode == 4) {
            QCOMPARE(states.size(), size_t(2));
            QCOMPARE(states[0]["pixel_shader"], nlohmann::json(35));
            QCOMPARE(states[1]["pixel_shader"], nlohmann::json(35));
        }
        const auto result = validateFrame(frame.path());
        size_t gaps = 0;
        for (const auto &finding : result["findings"])
            if (finding["kind"] == "context_state_identity_unresolved") {
                ++gaps;
                QVERIFY(finding["event_id"].is_number());
                QCOMPARE(finding["severity"], nlohmann::json("error"));
            }
        QCOMPARE(gaps, swaps + creations);
        QCOMPARE(result["errors"], nlohmann::json(gaps));
        qInfo().noquote() << QString::fromStdString(states.dump());
        Replay replay(frame);
        if (!mode) {
            replay.run();
            QCOMPARE(replay.output().rgba, fileBytes(QFileInfo(path).dir().filePath("native/expected.rgba")));
        } else {
            try {
                replay.run();
                QFAIL("A missing state identity was silently admitted");
            } catch (const std::runtime_error &error) {
                QVERIFY(QString(error.what()).contains("Event "));
                QVERIFY(QString(error.what()).contains("identity"));
            }
        }
    }
    void referencesAndNestedObservation() {
        const auto path = original(3);
        if (path.isEmpty())
            QSKIP("Set FLORA_CONTEXT_STATES");
        Frame frame(path.toStdWString());
        auto record = readContextStateRecord(0x359b, frame.payload(23));
        validateContextStateOwner(frame, 23, record);
        for (Id invalid : {Id(0), Id(2), Id(17), Id(999999)}) {
            auto changed = record;
            changed.owner = invalid;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateContextStateOwner(frame, 23, changed));
        }
        for (Id invalid : {Id(23), Id(24), Id(2), Id(999999)}) {
            auto changed = record;
            changed.link = invalid;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateContextStateOwner(frame, 23, changed));
        }
        auto changed = readContextStateRecord(0x3561, frame.payload(24));
        changed.owner = 3;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateContextStateOwner(frame, 24, changed));
        changed.owner = 2;
        changed.link = 22;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateContextStateOwner(frame, 24, changed));
    }
    void metadataDoesNotConfigureDevice() {
        const auto path = original(0);
        if (path.isEmpty())
            QSKIP("Set FLORA_CONTEXT_STATES");
        Frame frame(path.toStdWString());
        Capture capture;
        put(capture.bytes, 0x120, frame.width());
        put(capture.bytes, 0x124, frame.height());
        bool inserted = false;
        for (const auto &[id, e] : frame.entries()) {
            if (!inserted && e.category == 7 && isPipelineGetter(e.type)) {
                auto flags = wire(0x359b);
                put(flags, 16, uint32_t(0xffffffff)); // A return value, not native creation flags.
                capture.add(id, 7, 0x359b, flags);
                inserted = true;
                continue;
            }
            const auto p = frame.payload(id);
            capture.add(id, e.category, e.type, {p.begin(), p.end()});
        }
        QVERIFY(inserted);
        QTemporaryDir dir;
        const auto control = dir.filePath("flags.gpa_frame");
        capture.save(control);
        Frame edited(control.toStdWString());
        const auto validation = validateFrame(edited.path());
        QVERIFY2(validation["errors"] == 0, validation["findings"].dump(2).c_str());
        Replay replay(edited);
        replay.run();
        QCOMPARE(replay.counts["device_creation_flags_observations"], uint64_t(1));
        QCOMPARE(replay.output().rgba, fileBytes(QFileInfo(path).dir().filePath("native/expected.rgba")));
    }
};
QTEST_MAIN(ContextStateTests)
#include "ContextStateTests.moc"
