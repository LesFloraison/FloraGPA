#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/ClearView.h"
#include "replay/Replay.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw bytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing producer oracle");
    auto a = f.readAll();
    return {reinterpret_cast<const uint8_t *>(a.data()),
            reinterpret_cast<const uint8_t *>(a.data()) + a.size()};
}
template <class... T> Raw pack(T... v) {
    Raw b;
    (append(b, v), ...);
    return b;
}
} // namespace
class ClearViewTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 16; mode++)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_CLEAR_VIEW_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original ClearView corpus");
        auto base = root + QString("/%1/").arg(mode);
        Frame frame((base + "capture.gpa_frame").toStdWString());
        auto expected = bytes(base + "native/expected.bin"), rgba = bytes(base + "native/expected.rgba");
        Id event = 0, resource = 0, boundary = 0;
        for (const auto &[id, e] : frame.entries())
            if (e.category == 7 && e.type == 0x257) {
                QVERIFY(!event);
                event = id;
                auto command = readClearView(frame.payload(id));
                resource = validateClearView(frame, command).resource;
                QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
            }
        QVERIFY(event && resource);
        boundary = event;
        if (mode == 9)
            for (const auto &[id, e] : frame.entries())
                if (e.category == 7 && e.type == 0x42) {
                    Reader r(frame.payload(id));
                    r.skip(16);
                    auto dst = r.read<Id>();
                    r.skip(4);
                    if (r.read<Id>() == resource) {
                        boundary = id;
                        resource = dst;
                        break;
                    }
                }
        QVERIFY(mode != 9 || boundary != event);
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        auto storage = [&] {
            return mode == 7 || mode == 8 ? replay.readBuffer(resource) : replay.readTexture(resource);
        };
        for (int repeat = 0; repeat < 2; repeat++) {
            replay.run();
            QCOMPARE(storage(), expected);
            QCOMPARE(replay.output().rgba, rgba);
            unsigned checks = 0;
            replay.run({}, {}, [&](Id id, bool after, auto *, const auto &) {
                if (id == boundary && after) {
                    ++checks;
                    QCOMPARE(storage(), expected);
                }
            });
            QCOMPARE(checks, 1u);
            QCOMPARE(storage(), expected);
            QCOMPARE(replay.output().rgba, rgba);
            QCOMPARE(replay.counts.at("ClearView"), uint64_t(1));
        }
        // Changes to the actual command must affect resource bytes except documented no-ops.
        options.disabled.insert(event);
        Replay disabled(frame, options);
        disabled.run();
        auto control =
            mode == 7 || mode == 8 ? disabled.readBuffer(resource) : disabled.readTexture(resource);
        if (mode == 3 || mode == 14)
            QCOMPARE(control, expected);
        else
            QVERIFY(control != expected);
    }
    void checkedWire() {
        auto b = pack(Id(0), Id(2), Id(8), uint8_t(1), std::array<float, 4>{0, 1, 0, 1}, 1u, uint8_t(1),
                      std::array<int32_t, 4>{-2, -1, 4, 5});
        QCOMPARE(readClearView(b).rectangles[0][0], -2);
        for (size_t n = 0; n < b.size(); n++)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readClearView(Bytes(b).first(n)));
        for (int mode = 0; mode < 7; mode++) {
            auto bad = b;
            if (mode == 0)
                bad.push_back(0);
            if (mode == 1)
                put(bad, 0, Id(9));
            if (mode == 2)
                bad[24] = 0;
            if (mode == 3)
                bad[24] = 2;
            if (mode == 4)
                bad[45] = 2;
            if (mode == 5)
                put(bad, 41, 0u);
            if (mode == 6)
                put(bad, 41, UINT32_MAX);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readClearView(bad));
        }
        // A null rectangle pointer means the whole view; preserve the ignored raw count.
        b.resize(46);
        put(b, 41, UINT32_MAX);
        b[45] = 0;
        auto null = readClearView(b);
        QVERIFY(!null.hasRectangles);
        QCOMPARE(null.count, UINT32_MAX);
    }
    void invalidReferencesAndShapes() {
        auto root = qEnvironmentVariable("FLORA_CLEAR_VIEW_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original ClearView corpus");
        Frame original((root + "/1/capture.gpa_frame").toStdWString());
        Id event = 0;
        for (const auto &[id, e] : original.entries())
            if (e.category == 7 && e.type == 0x257)
                event = id;
        QVERIFY(event);
        auto command = readClearView(original.payload(event));
        QTemporaryDir dir;
        for (int mode = 0; mode < 7; mode++) {
            Capture cap;
            for (const auto &[id, e] : original.entries()) {
                auto source = original.payload(id);
                Raw raw(source.begin(), source.end());
                if (id == event) {
                    if (mode == 0)
                        put(raw, 8, Id(99999));
                    if (mode == 1)
                        put(raw, 16, Id(99999));
                    if (mode == 2)
                        put(raw, 16, command.context);
                    if (mode == 3)
                        put(raw, 46, int32_t(7));
                }
                if (id == command.view) {
                    if (mode == 4)
                        put(raw, 16, Id(99999));
                    if (mode == 5)
                        put(raw, 28, 8u); // Texture3D view over a 2D resource.
                    if (mode == 6)
                        raw.pop_back();
                }
                cap.add(id, e.category, e.type, raw);
            }
            auto path = dir.filePath(QString("bad-%1.gpa_frame").arg(mode));
            cap.save(path);
            Frame f(path.toStdWString());
            QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
            ReplayOptions options;
            options.warp = true;
            Replay replay(f, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
    }
};
QTEST_GUILESS_MAIN(ClearViewTests)
#include "ClearViewTests.moc"
