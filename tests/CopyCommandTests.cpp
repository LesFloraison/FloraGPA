#include "StateCapture.h"
#include "application/FrameValidation.h"
#include "core/CopyCommands.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw whole(Id dst = 2, Id src = 4) { return statePack(Id(0), Id(1), dst, src); }
Raw region(uint32_t x = 0, std::optional<std::array<uint32_t, 6>> box = {}) {
    auto raw = statePack(Id(0), Id(1), Id(2), 0u, x, 0u, 0u, Id(4), 0u, uint8_t(bool(box)));
    if (box)
        append(raw, *box);
    return raw;
}
Raw count(uint32_t offset = 12, Id view = 8) { return statePack(Id(0), Id(1), Id(2), offset, view); }
Capture base() {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    c.buffer(2, 3, 0, 0, {});
    c.buffer(4, 5, 0, 0, {11, 12, 13, 14});
    c.buffer(6, 7, 128, 64, {});
    c.uav(8, 6, 4);
    c.add(90, 7, 0x3522, statePack(Id(0), Id(1), 0u, 1u, uint8_t(1), Id(8), uint8_t(1), 9u));
    return c;
}
void change(Capture &c, Id id, size_t offset, uint32_t value) {
    for (auto &e : c.entries)
        if (e.id == id) {
            put(c.bytes, size_t(e.offset) + offset, value);
            return;
        }
    throw std::runtime_error("Missing fixture entry");
}
} // namespace
class CopyCommandTests : public QObject {
    Q_OBJECT
  private slots:
    void strictWire() {
        for (auto [type, raw] : std::vector<std::pair<uint16_t, Raw>>{
                 {0x3e, whole()},
                 {0x40, region()},
                 {0x40, region(0, std::array<uint32_t, 6>{0, 0, 0, 4, 1, 1})},
                 {0x3f, count()}}) {
            readCopyCommand(type, raw);
            for (size_t n = 0; n < raw.size(); ++n)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readCopyCommand(type, Bytes(raw).first(n)));
            auto extra = raw;
            extra.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readCopyCommand(type, extra));
            put(raw, 0, Id(1));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readCopyCommand(type, raw));
        }
        auto invalid = region();
        invalid[52] = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readCopyCommand(0x40, invalid));
    }
    void bufferResults() {
        QTemporaryDir dir;
        for (bool warp : {false, true})
            for (int mode = 0; mode < 6; ++mode) {
                auto c = base();
                uint16_t type = mode >= 4 ? 0x3f : mode == 0 ? 0x3e : 0x40;
                auto payload = mode >= 4   ? count(mode == 4 ? 0 : 12)
                               : mode == 0 ? whole()
                               : mode == 1 ? region(3, std::array<uint32_t, 6>{1, 0, 0, 12, 1, 1})
                               : mode == 2 ? region()
                                           : region(0, std::array<uint32_t, 6>{9, 0, 0, 4, 1, 1});
                c.add(100, 7, type, payload);
                auto path = dir.filePath("copy.gpa_frame");
                c.save(path);
                Frame f(path.toStdWString());
                QCOMPARE(validateCopyCommand(f, readCopyCommand(type, payload)), CopyValidation::Buffer);
                QVERIFY(validateFrame(path.toStdWString())["status"] != "blocked");
                Raw expected(16);
                auto source = statePack(11u, 12u, 13u, 14u);
                if (mode == 0 || mode == 2)
                    expected = source;
                if (mode == 1)
                    std::copy_n(source.begin() + 1, 11, expected.begin() + 3);
                if (mode >= 4)
                    put(expected, mode == 4 ? 0 : 12, 9u);
                ReplayOptions options;
                options.warp = warp;
                Replay replay(f, options);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    try {
                        replay.run();
                    } catch (const std::exception &e) {
                        QFAIL(qPrintable(QString("mode %1, warp %2: %3").arg(mode).arg(warp).arg(e.what())));
                    }
                    QCOMPARE(replay.readBuffer(2), expected);
                    if (mode == 3)
                        QCOMPARE(replay.counts.at("empty_copy_regions"), uint64_t(1));
                }
                options.disabled.insert(100);
                Replay disabled(f, options);
                try {
                    disabled.run();
                } catch (const std::exception &e) {
                    QFAIL(qPrintable(QString("disabled mode %1: %2").arg(mode).arg(e.what())));
                }
                QCOMPARE(disabled.readBuffer(2), Raw(16));
            }
    }
    void rejectBeforeNativeExecution() {
        QTemporaryDir dir;
        struct Bad {
            uint16_t type;
            Raw raw;
            std::function<void(Capture &)> mutate;
        };
        std::vector<Bad> cases{{0x3e, whole(0, 4), {}},
                               {0x3e, whole(999, 4), {}},
                               {0x3e, whole(2, 1), {}},
                               {0x3e, whole(2, 2), {}},
                               {0x3e, whole(), [](auto &c) { change(c, 4, 16, 8); }},
                               {0x3e, whole(), [](auto &c) { change(c, 2, 20, 1); }},
                               {0x40, region(UINT32_MAX), {}},
                               {0x40, region(1), {}},
                               {0x40, region(UINT32_MAX, std::array<uint32_t, 6>{4, 0, 0, 4, 1, 1}), {}},
                               {0x40, region(0, std::array<uint32_t, 6>{0, 0, 0, 17, 1, 1}), {}},
                               {0x40, region(0, std::array<uint32_t, 6>{0, 0, 0, 4, 2, 1}), {}},
                               {0x3f, count(1), {}},
                               {0x3f, count(16), {}},
                               {0x3f, count(UINT32_MAX), {}},
                               {0x3f, count(0, 4), {}},
                               {0x3f, count(), [](auto &c) { change(c, 8, 40, 0); }},
                               {0x3f, count(), [](auto &c) { change(c, 8, 32, UINT32_MAX); }},
                               {0x3f, count(), [](auto &c) { change(c, 8, 36, UINT32_MAX); }},
                               {0x3f, count(), [](auto &c) { change(c, 6, 24, 0); }}};
        for (auto [offset, value] : std::map<size_t, uint32_t>{{24, 1}, {32, 1}, {36, 1}, {48, 1}}) {
            auto raw = region();
            put(raw, offset, value);
            cases.push_back({0x40, raw, {}});
        }
        for (auto &bad : cases) {
            auto c = base();
            if (bad.mutate)
                bad.mutate(c);
            c.add(100, 7, bad.type, bad.raw);
            auto path = dir.filePath("bad.gpa_frame");
            c.save(path);
            Frame frame(path.toStdWString());
            if (bad.type == 0x3f && readCopyCommand(bad.type, bad.raw).x == 1) {
                const auto evidence = qEnvironmentVariable("FLORA_COPY_ARTIFACT_DIR");
                if (!evidence.isEmpty()) {
                    QDir().mkpath(evidence);
                    c.save(evidence + "/unaligned-counter.gpa_frame");
                }
            }
            auto report = validateFrame(path.toStdWString());
            QCOMPARE(report["status"], nlohmann::json("blocked"));
            bool located = false;
            for (auto &finding : report["findings"])
                if (finding["kind"] == "copy_command_rejected") {
                    QCOMPARE(finding["event_id"], nlohmann::json(100));
                    const auto dst = readCopyCommand(bad.type, bad.raw).destination;
                    QCOMPARE(finding["resource_id"], dst ? nlohmann::json(dst) : nlohmann::json(nullptr));
                    located = true;
                }
            QVERIFY(located);
            ReplayOptions options;
            options.warp = true;
            Replay replay(frame, options);
            bool after = false;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     replay.run({}, {}, [&](Id id, bool done, auto *, const auto &) {
                                         if (id == 100 && done)
                                             after = true;
                                     }));
            QVERIFY(!after);
        }
    }
    void originalCopies() {
        auto root = qEnvironmentVariable("FLORA_COPY_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_COPY_CAPTURES to original copy captures");
        for (int mode = 0; mode < 6; ++mode)
            for (bool warp : {false, true}) {
                Frame f((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
                Raw expected(64), source(64);
                for (unsigned i = 0; i < 64; ++i)
                    source[i] = uint8_t(i + 1);
                if (mode == 0 || mode == 2)
                    expected = source;
                if (mode == 1)
                    std::copy_n(source.begin() + 1, 11, expected.begin() + 3);
                if (mode >= 4)
                    put(expected, mode == 4 ? 60 : 0, 9u);
                Id event = 0, resource = 0;
                for (const auto &[id, e] : f.entries())
                    if (e.category == 7 && isCopyCommand(e.type)) {
                        const auto c = readCopyCommand(e.type, f.payload(id));
                        if (validateCopyCommand(f, c) == CopyValidation::Buffer) {
                            event = id;
                            resource = c.destination;
                            break;
                        }
                    }
                QVERIFY(event && resource);
                ReplayOptions options;
                options.warp = warp;
                Replay replay(f, options);
                bool checked = false;
                replay.run({}, {}, [&](Id id, bool after, auto *, const auto &) {
                    if (id == event && after) {
                        QCOMPARE(replay.readBuffer(resource), expected);
                        checked = true;
                    }
                });
                QVERIFY(checked);
                QCOMPARE(replay.readBuffer(resource), expected);
            }
    }
    void textureScopeIsExplicit() {
        QTemporaryDir dir;
        auto c = base();
        const auto texture = statePack(Id(0), Id(0), 8u, 8u, 1u, 1u, 28u, 1u, 0u, 0u, 0u, 0u, 0u, Id(0));
        c.add(20, 5, 0x85, texture);
        c.add(21, 5, 0x85, texture);
        c.add(100, 7, 0x3e, whole(20, 21));
        auto path = dir.filePath("texture.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        QCOMPARE(validateCopyCommand(f, readCopyCommand(0x3e, f.payload(100))),
                 CopyValidation::TextureReferences);
        const auto report = validateFrame(path.toStdWString());
        bool partial = false;
        for (const auto &finding : report["findings"])
            if (finding["kind"] == "texture_copy_validation_partial") {
                QCOMPARE(finding["event_id"], nlohmann::json(100));
                partial = true;
            }
        QVERIFY(partial);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 validateCopyCommand(f, readCopyCommand(0x3e, whole(2, 20))));
        c.add(22, 5, 0x87, texture);
        const auto referencePath = dir.filePath("reference.gpa_frame");
        c.save(referencePath);
        Frame reference(referencePath.toStdWString());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 validateCopyCommand(reference, readCopyCommand(0x3e, whole(20, 22))));
    }
};
QTEST_GUILESS_MAIN(CopyCommandTests)
#include "CopyCommandTests.moc"
