#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/DiscardRecords.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw bytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing discard producer oracle");
    auto a = f.readAll();
    return {reinterpret_cast<const uint8_t *>(a.data()),
            reinterpret_cast<const uint8_t *>(a.data()) + a.size()};
}
QString base(int mode) {
    const auto root = qEnvironmentVariable("FLORA_DISCARD_CAPTURES");
    return root.isEmpty() ? QString{} : root + QString("/%1/").arg(mode);
}
Id discardEvent(const Frame &frame) {
    for (const auto &[id, e] : frame.entries())
        if (e.category == 7 && isDiscardRecord(e.type))
            return id;
    return 0;
}
void clone(const Frame &frame, const QString &path, Id edit, Raw data) {
    Capture c;
    put(c.bytes, 0x120, frame.width());
    put(c.bytes, 0x124, frame.height());
    for (const auto &[id, e] : frame.entries()) {
        auto raw = frame.payload(id);
        c.add(id, e.category, e.type, id == edit ? data : Raw(raw.begin(), raw.end()));
    }
    c.save(path);
}
} // namespace
class DiscardTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 17; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp))) << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = base(mode);
        if (root.isEmpty())
            QSKIP("Set FLORA_DISCARD_CAPTURES to the original corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto event = discardEvent(frame);
        const auto validation = validateFrame(frame.path());
        const bool blocked = mode == 9 || mode == 16;
        ReplayOptions options;
        options.warp = warp;
        if (blocked) {
            QCOMPARE(validation["errors"], nlohmann::json(1));
            bool found = false;
            for (const auto &issue : validation["findings"])
                found |=
                    issue["kind"] == "discard_rectangle_presence_unresolved" && issue["event_id"] == event;
            QVERIFY(found);
            QCOMPARE(inspectCommand(frame, event)["status"], nlohmann::json("decoded"));
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            options.disabled.insert(event);
            Replay disabled(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, disabled.run());
            return;
        }
        QVERIFY2(validation["errors"] == 0, validation["findings"].dump(2).c_str());
        Id resource = 10;
        if (mode) {
            QVERIFY(event);
            const auto record = readDiscardRecord(frame.entry(event).type, frame.payload(event));
            resource = validateDiscardRecord(frame, record).resource;
            QCOMPARE(inspectCommand(frame, event)["status"], nlohmann::json("decoded"));
        }
        const auto expected = bytes(root + "native/expected.bin");
        const auto image = bytes(root + "native/expected.rgba");
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            const auto actual = frame.entry(resource).type == 0x83 ? replay.readBuffer(resource)
                                                                   : replay.readTexture(resource);
            QCOMPARE(actual, expected);
            QCOMPARE(replay.output().rgba, image);
            QCOMPARE(replay.discardHistory().size(), size_t(mode != 0));
            if (mode) {
                QCOMPARE(replay.discardHistory()[0].first, event);
                QCOMPARE(replay.counts.at(commandName(frame.entry(event).type)), uint64_t(1));
            }
        }
        if (mode == 4) {
            auto prefixOptions = options;
            for (const auto &[id, entry] : frame.entries())
                if (entry.category == 7 && isDraw(entry.type)) {
                    prefixOptions.until = id;
                    break;
                }
            QVERIFY(prefixOptions.until && prefixOptions.until < event);
            Replay prefix(frame, prefixOptions);
            prefix.run();
            QCOMPARE(prefix.output().rgba, bytes(root + "native/sampled-before-discard.rgba"));
            QVERIFY(prefix.discardHistory().empty());
        }
        if (mode) {
            // Discard is advisory: final bytes after defining writes must not be used to
            // infer whether the driver invalidated memory. This is a separate count control.
            options.disabled.insert(event);
            Replay disabled(frame, options);
            disabled.run();
            QVERIFY(disabled.discardHistory().empty());
            QCOMPARE(frame.entry(resource).type == 0x83 ? disabled.readBuffer(resource)
                                                        : disabled.readTexture(resource),
                     expected);
        }
    }
    void checkedWire_data() {
        QTest::addColumn<int>("mode");
        for (int mode : {1, 3, 7, 8, 9, 10})
            QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void checkedWire() {
        QFETCH(int, mode);
        const auto root = base(mode);
        if (root.isEmpty())
            QSKIP("Set FLORA_DISCARD_CAPTURES");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto event = discardEvent(frame);
        auto raw = frame.payload(event);
        const auto type = frame.entry(event).type;
        readDiscardRecord(type, raw);
        for (size_t n = 0; n < raw.size(); ++n)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readDiscardRecord(type, raw.first(n)));
        Raw extra(raw.begin(), raw.end());
        extra.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readDiscardRecord(type, extra));
    }
    void malformedAndReferences() {
        if (base(7).isEmpty())
            QSKIP("Set FLORA_DISCARD_CAPTURES");
        Frame frame((base(7) + "capture.gpa_frame").toStdWString());
        const auto event = discardEvent(frame);
        const auto original = frame.payload(event);
        for (int variant = 0; variant < 9; ++variant) {
            Raw raw(original.begin(), original.end());
            if (variant == 0)
                raw[28] = 2;
            if (variant == 1)
                put(raw, 24, uint32_t(0xffffffff));
            if (variant == 2)
                put(raw, 24, uint32_t(0));
            if (variant == 3)
                put(raw, 0, Id(1));
            if (variant == 4)
                put(raw, 8, Id(3));
            if (variant == 5)
                put(raw, 16, Id(0));
            if (variant == 6)
                put(raw, 16, Id(999999));
            if (variant == 7)
                put(raw, 16, Id(10));
            if (variant == 8)
                put(raw, 29, int32_t(10));
            QTemporaryDir dir;
            const auto path = dir.filePath("bad.gpa_frame");
            clone(frame, path, event, raw);
            Frame bad(path.toStdWString());
            QVERIFY(validateFrame(bad.path())["errors"].get<size_t>() > 0);
            Replay replay(bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
        for (int variant = 0; variant < 3; ++variant) {
            const auto raw = frame.payload(10);
            Raw changed(raw.begin(), raw.end());
            if (variant == 0)
                put(changed, 16 + 7 * 4, uint32_t(3)); // Staging target.
            if (variant == 1)
                changed.pop_back();
            if (variant == 2)
                changed.push_back(0);
            QTemporaryDir dir;
            const auto path = dir.filePath("resource.gpa_frame");
            clone(frame, path, 10, changed);
            Frame bad(path.toStdWString());
            QVERIFY(validateFrame(bad.path())["errors"].get<size_t>() > 0);
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                validateDiscardRecord(bad, readDiscardRecord(0x3563, bad.payload(event))));
        }
    }
    void zeroCountPresenceCollision() {
        if (base(9).isEmpty())
            QSKIP("Set FLORA_DISCARD_CAPTURES");
        Frame nullPointer((base(9) + "capture.gpa_frame").toStdWString());
        Frame nonNull((base(16) + "capture.gpa_frame").toStdWString());
        const auto a = nullPointer.payload(discardEvent(nullPointer));
        const auto b = nonNull.payload(discardEvent(nonNull));
        QCOMPARE(Raw(a.begin(), a.end()), Raw(b.begin(), b.end()));
        QVERIFY(ambiguousDiscardRectangles(readDiscardRecord(0x3563, a)));
        QVERIFY(bytes(base(9) + "native/expected.bin") != bytes(base(16) + "native/expected.bin"));
    }
    void missingViews_data() {
        QTest::addColumn<int>("mode");
        for (int mode : {3, 4, 5, 7, 8, 10, 11, 12, 13})
            QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void missingViews() {
        QFETCH(int, mode);
        const auto root = qEnvironmentVariable("FLORA_DISCARD_MISSING_VIEWS");
        if (root.isEmpty())
            QSKIP("Set FLORA_DISCARD_MISSING_VIEWS to the preserved original corpus");
        Frame frame((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
        const auto event = discardEvent(frame);
        const auto record = readDiscardRecord(frame.entry(event).type, frame.payload(event));
        QVERIFY(record.target && !frame.entries().contains(record.target));
        bool located = false;
        const auto validation = validateFrame(frame.path());
        for (const auto &finding : validation["findings"])
            located |= finding["kind"] == "discard_target_missing" && finding["event_id"] == event &&
                       finding["resource_id"] == record.target;
        QVERIFY(located);
        QCOMPARE(inspectCommand(frame, event)["status"], nlohmann::json("decoded"));
        Replay replay(frame);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
    }
    void malformedViews() {
        if (base(7).isEmpty())
            QSKIP("Set FLORA_DISCARD_CAPTURES");
        Frame frame((base(7) + "capture.gpa_frame").toStdWString());
        const auto event = discardEvent(frame);
        const auto record = readDiscardRecord(frame.entry(event).type, frame.payload(event));
        for (int variant = 0; variant < 4; ++variant) {
            const auto p = frame.payload(record.target);
            Raw changed(p.begin(), p.end());
            if (variant == 0)
                put(changed, 16, Id(999999));
            if (variant == 1)
                put(changed, 28, uint32_t(999));
            if (variant == 2)
                changed.pop_back();
            if (variant == 3)
                changed.push_back(0);
            QTemporaryDir dir;
            const auto path = dir.filePath("view.gpa_frame");
            clone(frame, path, record.target, changed);
            Frame bad(path.toStdWString());
            QVERIFY(validateFrame(bad.path())["errors"].get<size_t>() > 0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateDiscardRecord(bad, record));
        }
    }
};
QTEST_MAIN(DiscardTests)
#include "DiscardTests.moc"
