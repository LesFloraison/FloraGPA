#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/TextureCreation.h"
#include "core/UavCounters.h"
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
Raw fileBytes(QString path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing oracle");
    auto b = f.readAll();
    return Raw(b.begin(), b.end());
}
} // namespace
class BufferViewCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void rangesFlagsAndReferences() {
        QTemporaryDir dir;
        for (uint16_t t : {uint16_t(0x357c), uint16_t(0x357d), uint16_t(0x357e)})
            for (int mode : {0, 1, 2}) {
                if (t == 0x357e && mode)
                    continue;
                auto verify = [&](unsigned mutation, bool good) {
                    std::array<uint32_t, 6> bd{64,
                                               0,
                                               t == 0x357c   ? 8u
                                               : t == 0x357d ? 128u
                                                             : 32u,
                                               0,
                                               mode == 1   ? 32u
                                               : mode == 2 ? 64u
                                                           : 0u,
                                               mode == 2 ? 4u : 0u};
                    std::vector<uint32_t> sd{mode == 1   ? 39u
                                             : mode == 2 ? 0u
                                                         : 42u,
                                             t == 0x357c && mode == 1 ? 11u : 1u, 4, 8, mode == 1 ? 1u : 0u};
                    if (t == 0x357c)
                        sd.push_back(0);
                    if (mutation == 1)
                        sd[2] = UINT32_MAX;
                    if (mutation == 2)
                        sd[3] = 0;
                    if (mutation == 3)
                        sd[3] = 99;
                    if (mutation == 4)
                        sd[1] = 4;
                    if (mutation == 5)
                        bd[2] = 0;
                    if (mutation == 6)
                        sd[4] = 8;
                    if (mutation == 7)
                        bd[4] = 0;
                    if (mutation == 8)
                        bd[5] = 0;
                    Capture c;
                    c.add(1, 5, 0x127, Raw(24));
                    c.add(2, 5, 0x81, Raw(28));
                    c.add(3, 5, 0x83, pack(Id(0), Id(2), bd, Id(0)));
                    auto saved = pack(Id(0), Id(2), Id(3)),
                         call = pack(Id(0), Id(2), int32_t(0), Id(3), uint8_t(1));
                    for (auto v : sd) {
                        append(saved, v);
                        append(call, v);
                    }
                    append(call, Id(4));
                    c.add(4, 5, viewCreationResourceType(t), saved);
                    c.add(100, 7, t, call);
                    const auto path = dir.filePath("view.gpa_frame");
                    c.save(path);
                    Frame frame(path.toStdWString());
                    const auto audit = auditTextureCreations(frame);
                    if (good) {
                        QVERIFY2(audit.records.at(100).error.empty(), audit.records.at(100).error.c_str());
                    } else {
                        QVERIFY(!audit.records.at(100).error.empty());
                        QVERIFY(validateFrame(frame.path())["errors"].get<unsigned>() > 0);
                    }
                };
                verify(0, true);
                for (unsigned m = 1; m <= 5; ++m)
                    verify(m, false);
                if (mode == 1) {
                    verify(6, false);
                    verify(7, false);
                }
                if (mode == 2) {
                    verify(7, false);
                    verify(8, false);
                }
            }
    }
    void srvMetadata() {
        QTemporaryDir dir;
        for (uint16_t t : {uint16_t(0x3026), uint16_t(0x3027), uint16_t(0x3028), uint16_t(0x3029),
                           uint16_t(0x302d), uint16_t(0x3015), uint16_t(0x301c)}) {
            auto b = pack(Id(0), Id(4));
            if (t == 0x3026) {
                append(b, int32_t(0));
                append(b, std::array<uint8_t, 16>{});
                append(b, Id(4));
            } else if (t == 0x301c) {
                append(b, uint8_t(1));
                append(b, std::array<uint32_t, 6>{});
            } else if (t == 0x3027 || t == 0x3028)
                append(b, 0u);
            else
                append(b, Id(3));
            QVERIFY(acceptTextureCreationObservation(t, b));
            for (size_t n = 0; n < b.size(); ++n)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         acceptTextureCreationObservation(t, Bytes(b).first(n)));
            if (t == 0x301c) {
                auto absent = b;
                absent.resize(17);
                absent[16] = 0;
                QVERIFY(acceptTextureCreationObservation(t, absent));
                absent[16] = 2;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptTextureCreationObservation(t, absent));
            }
            auto extra = b;
            extra.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptTextureCreationObservation(t, extra));
            Capture c;
            c.add(100, 7, t, b);
            auto path = dir.filePath("metadata.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            QCOMPARE(inspectCommand(f, 100)["status"], nlohmann::json("decoded"));
        }
    }
    void originalCaptures_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode : {0, 1, 2, 3, 10, 11, 12, 13, 14, 15, 16, 17, 20, 30, 31, 32, 33, 34, 35})
            for (bool warp : {false, true})
                QTest::newRow(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware").toUtf8().constData())
                    << mode << warp;
    }
    void originalCaptures() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_BUFFER_VIEW_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_BUFFER_VIEW_CAPTURES to original corpus");
        const auto folder = root + QString("/%1/").arg(mode);
        Frame frame((folder + "capture.gpa_frame").toStdWString());
        auto expected = fileBytes(folder + "native/expected.bin");
        Id view = 0, resource = 0, creation = 0, write = 0, reset = 0;
        unsigned observations = 0;
        for (const auto &[id, c] : auditTextureCreations(frame).records) {
            QVERIFY2(c.error.empty(), c.error.c_str());
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
            if (c.result)
                ++observations;
            else {
                view = c.resource;
                resource = c.source;
                creation = id;
            }
        }
        QVERIFY(view && resource && creation);
        QCOMPARE(observations, mode >= 30 ? 1u : 0u);
        for (const auto &[id, e] : frame.entries())
            if (e.category == 7) {
                if (id > creation && (e.type == 0x33 || e.type == 0x35))
                    write = id;
                if (id > creation && e.type == 0x32) {
                    Reader r(frame.payload(id));
                    r.skip(16);
                    if (r.read<Id>() == view)
                        write = id;
                }
                if (e.type == 0x25e || e.type == 0x3522) {
                    auto command = readOutputCommand(e.type, frame.payload(id));
                    if (command.initialCounts &&
                        std::find(command.initialCounts->begin(), command.initialCounts->end(), 2u) !=
                            command.initialCounts->end())
                        reset = id;
                }
            }
        if (!write)
            write = creation;
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        ReplayOptions o;
        o.warp = warp;
        Replay replay(frame, o);
        bool seen = false;
        replay.run({}, {}, [&](Id e, bool after, auto *, const auto &) {
            if (e == write && after) {
                seen = true;
                QCOMPARE(replay.readBuffer(resource), expected);
            }
        });
        QVERIFY(seen);
        QCOMPARE(replay.readBuffer(resource), expected);
        if (describeCounter(frame, view)) {
            QVERIFY(reset);
            auto boundary = o;
            boundary.until = creation;
            Replay created(frame, boundary);
            created.run();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, created.readCounter(view));
            boundary.until = reset;
            Replay initialized(frame, boundary);
            initialized.run();
            QCOMPARE(initialized.readCounter(view), 2u);
            QCOMPARE(replay.readCounter(view), mode == 16 || mode == 17 ? 4u : 3u);
            o.disabled.insert(reset);
            Replay missing(frame, o);
            try {
                missing.run();
                QFAIL("Missing initial counter must be rejected");
            } catch (const std::exception &e) {
                QVERIFY(QString::fromUtf8(e.what()).contains("has no defined counter value"));
            }
            o.initialUavCounters[view] = 2;
            Replay override(frame, o);
            override.run();
            QCOMPARE(override.readBuffer(resource), expected);
            QCOMPARE(override.readCounter(view), mode == 16 || mode == 17 ? 4u : 3u);
        }
    }
};
QTEST_GUILESS_MAIN(BufferViewCreationTests)
#include "BufferViewCreationTests.moc"
