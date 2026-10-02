#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/BufferCreation.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... values) {
    Raw out;
    (append(out, values), ...);
    return out;
}
Raw call(bool initial = true, int32_t result = 0, Id returned = 3) {
    Raw out = pack(Id(0), Id(2), result, uint8_t(1), 16u, 0u, 0u, 0u, 0u, 0u, uint8_t(initial));
    if (initial) {
        append(out, UINT64_MAX);
        append(out, 0u);
        append(out, 0u);
    }
    append(out, returned);
    return out;
}
Capture capture(bool initial = true) {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    c.add(2, 5, 0x81, Raw(28));
    auto resource = pack(Id(0), Id(2), 16u, 0u, 0u, 0u, 0u, 0u, Id(initial ? 4 : 999));
    c.add(3, 5, 0x83, resource);
    c.add(4, 9, 1, pack(16u, std::array<uint32_t, 4>{11, 12, 13, 14}));
    c.add(100, 7, 0x3578, call(initial));
    return c;
}
void replace(Capture &c, Id id, Raw raw) {
    auto it = std::find_if(c.entries.begin(), c.entries.end(), [&](auto &e) { return e.id == id; });
    it->offset = c.bytes.size();
    it->size = uint32_t(raw.size());
    c.bytes.insert(c.bytes.end(), raw.begin(), raw.end());
}
Raw raw(const Capture &c, Id id) {
    auto it = std::find_if(c.entries.begin(), c.entries.end(), [&](auto &e) { return e.id == id; });
    return Raw(c.bytes.begin() + it->offset, c.bytes.begin() + it->offset + it->size);
}
} // namespace
class BufferCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void wireAndResourceValidation() {
        QTemporaryDir dir;
        auto check = [&](Capture c, bool valid) {
            auto path = dir.filePath("buffer.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            auto audit = auditBufferCreations(f);
            if (valid) {
                QVERIFY(audit.records.at(100).error.empty());
            } else {
                QVERIFY_EXCEPTION_THROWN(requireBufferCreation(audit, 100), std::runtime_error);
                auto preflight = validateFrame(path.toStdWString());
                QCOMPARE(preflight["status"], nlohmann::json("blocked"));
                bool found = false;
                for (auto &v : preflight["findings"])
                    found |= v["event_id"] == 100 && v["severity"] == "error";
                QVERIFY(found);
            }
        };
        for (bool initial : {false, true}) {
            auto bytes = call(initial);
            check(capture(initial), true);
            for (size_t n = 0; n < bytes.size(); ++n) {
                auto c = capture(initial);
                replace(c, 100, Raw(bytes.begin(), bytes.begin() + n));
                check(c, false);
            }
            bytes.push_back(0);
            auto c = capture(initial);
            replace(c, 100, bytes);
            check(c, false);
        }
        for (auto [offset, value] : std::vector<std::pair<size_t, uint32_t>>{
                 {0, 1}, {8, 999}, {16, 2}, {20, 2}, {21, 32}, {25, 4}, {45, 2}}) {
            auto c = capture();
            auto b = call();
            put(b, offset, value);
            replace(c, 100, b);
            check(c, false);
        }
        for (auto result : {int32_t(-2147024809), int32_t(1)}) {
            auto c = capture();
            replace(c, 100, call(false, result, 0));
            check(c, true);
            replace(c, 100, call(false, result, 3));
            check(c, false);
        }
        auto c = capture();
        auto r = raw(c, 3);
        put(r, 8, Id(999));
        replace(c, 3, r);
        check(c, false);
        c = capture();
        r = raw(c, 3);
        put(r, 40, Id(0));
        replace(c, 3, r);
        check(c, false);
        c = capture();
        replace(c, 4, pack(4u, 1u));
        check(c, false);
        c = capture();
        c.add(101, 7, 0x3578, call());
        check(c, false);
    }
    void privateDataIsOpaque() {
        auto p =
            pack(Id(0), Id(UINT64_MAX), int32_t(0), std::array<uint8_t, 16>{}, UINT32_MAX, Id(UINT64_MAX));
        auto observation = readPrivateDataObservation(p);
        QCOMPARE(observation.size, UINT32_MAX);
        for (size_t n = 0; n < p.size(); ++n)
            QVERIFY_EXCEPTION_THROWN(readPrivateDataObservation(Bytes(p).first(n)), std::runtime_error);
        auto bad = p;
        bad.push_back(0);
        QVERIFY_EXCEPTION_THROWN(readPrivateDataObservation(bad), std::runtime_error);
        put(p, 40, Id(0));
        QVERIFY_EXCEPTION_THROWN(readPrivateDataObservation(p), std::runtime_error);
        put(p, 36, 0u);
        readPrivateDataObservation(p);
    }
    void productionBoundaries_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void productionBoundaries() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        auto path = dir.filePath("buffer.gpa_frame");
        for (bool initial : {false, true}) {
            auto c = capture(initial);
            c.save(path);
            Frame f(path.toStdWString());
            ReplayOptions options;
            options.warp = warp;
            Replay replay(f, options);
            for (int repeat = 0; repeat < 2; ++repeat) {
                replay.run();
                QCOMPARE(replay.counts.at("CreateBuffer"), uint64_t(1));
                if (initial) {
                    auto bytes = replay.readBuffer(3);
                    QCOMPARE(bytes, pack(11u, 12u, 13u, 14u));
                } else {
                    QCOMPARE(replay.counts.at("buffers_created_without_initial_data"), uint64_t(1));
                }
            }
            options.until = 100;
            options.before = true;
            Replay before(f, options);
            before.run();
            QVERIFY_EXCEPTION_THROWN(before.readBuffer(3), std::runtime_error);
        }
        auto c = capture();
        c.buffer(8, 9, 0, 0, {});
        c.add(50, 7, 0x3e, pack(Id(0), Id(1), Id(8), Id(3)));
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay early(f, options);
        QVERIFY_EXCEPTION_THROWN(early.run(), std::runtime_error);
    }
    void originalCaptures() {
        auto root = qEnvironmentVariable("FLORA_BUFFER_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_BUFFER_CAPTURES to original producer outputs");
        for (int mode = 0; mode < 5; ++mode)
            for (bool warp : {false, true}) {
                auto path = root + QString("/%1/capture.gpa_frame").arg(mode);
                Frame f(path.toStdWString());
                auto audit = auditBufferCreations(f);
                QCOMPARE(audit.records.size(), size_t(1));
                auto [event, creation] = *audit.records.begin();
                QVERIFY2(creation.error.empty(), creation.error.c_str());
                ReplayOptions options;
                options.warp = warp;
                Replay replay(f, options);
                bool boundary = false;
                replay.run({}, {}, [&](Id id, bool after, ID3D11DeviceContext *, const auto &objects) {
                    if (id != event)
                        return;
                    if (!after) {
                        if (creation.result == 0)
                            QVERIFY_EXCEPTION_THROWN(replay.readBuffer(creation.resource), std::runtime_error);
                        return;
                    }
                    QCOMPARE(objects.contains(creation.resource), mode != 2 && mode != 3);
                    if (mode == 0 || mode == 4) {
                        auto bytes = replay.readBuffer(creation.resource);
                        Raw expected;
                        for (unsigned i = 0; i < 16; ++i)
                            append(expected, 0x11110000u + i);
                        QCOMPARE(bytes, expected);
                    }
                    boundary = true;
                });
                QVERIFY(boundary);
                if (mode == 0 || mode == 1 || mode == 4) {
                    Raw expected;
                    for (unsigned i = 0; i < 16; ++i)
                        append(expected, (mode == 0 ? 0x11110000u : 0x22220000u) + i);
                    QCOMPARE(replay.readBuffer(creation.resource), expected);
                } else
                    QCOMPARE(replay.counts.at("buffer_creation_observations"), uint64_t(1));
            }
    }
};
QTEST_GUILESS_MAIN(BufferCreationTests)
#include "BufferCreationTests.moc"
