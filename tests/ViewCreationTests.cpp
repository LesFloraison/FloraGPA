#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/TextureCreation.h"
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
std::vector<uint32_t> descriptor(uint16_t t) {
    return t == 0x357f ? std::vector<uint32_t>{40, 4, 0, 1, 1, 1}
                       : std::vector<uint32_t>{t == 0x357d ? 42u : 28u, 5, 1, 1, 1};
}
Raw call(uint16_t t, bool explicitDesc = true, int32_t hr = 0, Id returned = 4) {
    auto b = pack(Id(0), Id(2), hr, Id(3), uint8_t(explicitDesc));
    if (explicitDesc)
        for (auto v : descriptor(t))
            append(b, v);
    append(b, returned);
    return b;
}
Capture capture(uint16_t t, bool explicitDesc = true) {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    c.add(2, 5, 0x81, Raw(28));
    c.add(3, 5, 0x85,
          pack(Id(0), Id(2),
               std::array<uint32_t, 11>{16, 16, 2, 2,
                                        t == 0x357f   ? 40u
                                        : t == 0x357d ? 42u
                                                      : 28u,
                                        1, 0, 0,
                                        t == 0x357f   ? 64u
                                        : t == 0x357d ? 128u
                                                      : 32u,
                                        0, 0},
               Id(0)));
    auto saved = pack(Id(0), Id(2), Id(3));
    for (auto v : descriptor(t))
        append(saved, v);
    c.add(4, 5, viewCreationResourceType(t), saved);
    c.add(100, 7, t, call(t, explicitDesc));
    return c;
}
void replace(Capture &c, Id id, Raw b) {
    auto &e = *std::find_if(c.entries.begin(), c.entries.end(), [&](auto &v) { return v.id == id; });
    e.offset = c.bytes.size();
    e.size = uint32_t(b.size());
    c.bytes.insert(c.bytes.end(), b.begin(), b.end());
}
Raw fileBytes(QString path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing oracle");
    auto b = f.readAll();
    return Raw(b.begin(), b.end());
}
} // namespace
class ViewCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void strictCreationWireAndReferences() {
        QTemporaryDir dir;
        for (uint16_t t : {uint16_t(0x357d), uint16_t(0x357e), uint16_t(0x357f)}) {
            auto verify = [&](Capture c, bool good) {
                const auto path = dir.filePath("view.gpa_frame");
                c.save(path);
                Frame f(path.toStdWString());
                auto a = auditTextureCreations(f);
                if (good) {
                    QVERIFY2(a.records.at(100).error.empty(), a.records.at(100).error.c_str());
                } else {
                    QVERIFY_EXCEPTION_THROWN(requireTextureCreation(a, 100), std::runtime_error);
                    QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
                }
            };
            for (bool explicitDesc : {false, true}) {
                auto b = call(t, explicitDesc);
                verify(capture(t, explicitDesc), true);
                for (size_t n = 0; n < b.size(); ++n)
                    QVERIFY_EXCEPTION_THROWN(readTextureCreation(t, Bytes(b).first(n)), std::runtime_error);
                auto c = capture(t, explicitDesc);
                auto extra = b;
                extra.push_back(0);
                replace(c, 100, extra);
                verify(c, false);
                for (size_t pos : {size_t(0), size_t(8), size_t(20), size_t(28)}) {
                    c = capture(t, explicitDesc);
                    auto bad = b;
                    bad[pos] = 255;
                    replace(c, 100, bad);
                    verify(c, false);
                }
                c = capture(t, explicitDesc);
                auto zero = b;
                put(zero, zero.size() - 8, Id(0));
                replace(c, 100, zero);
                verify(c, false);
            }
            for (int32_t hr : {int32_t(-2147024809), int32_t(1)}) {
                auto c = capture(t);
                replace(c, 100, call(t, true, hr, 0));
                verify(c, true);
                replace(c, 100, call(t, true, hr, 4));
                verify(c, false);
            }
            auto c = capture(t);
            c.add(101, 7, t, call(t));
            verify(c, false);
            c = capture(t);
            auto wrong = call(t);
            put(wrong, 29, 99u);
            replace(c, 100, wrong);
            verify(c, false);
            c = capture(t);
            c.entries[3].type = 0x8c;
            verify(c, false);
            c = capture(t);
            auto saved = pack(Id(0), Id(2), Id(999));
            for (auto v : descriptor(t))
                append(saved, v);
            replace(c, 4, saved);
            verify(c, false);
            c = capture(t);
            c.entries[2].type = 0x83;
            verify(c, false);
        }
    }
    void descriptorUnionSemantics() {
        for (uint16_t t : {uint16_t(0x357d), uint16_t(0x357e), uint16_t(0x357f)}) {
            auto a = descriptor(t), b = a;
            QVERIFY(createdViewDescriptorEqual(t, a, b));
            b.back()++;
            QVERIFY(!createdViewDescriptorEqual(t, a, b));
            a[1] = t == 0x357f ? 3 : 4;
            b = a;
            b.back()++;
            QVERIFY(createdViewDescriptorEqual(t, a, b));
            b[2]++;
            QVERIFY(!createdViewDescriptorEqual(t, a, b));
            a[1] = 99;
            b = a;
            QVERIFY(!createdViewDescriptorEqual(t, a, b));
        }
    }
    void metadataWireAndInspection() {
        QTemporaryDir dir;
        for (uint16_t base : {uint16_t(0x3053), uint16_t(0x301d), uint16_t(0x3185)})
            for (unsigned delta : {0, 1, 2, 3, 7, 8}) {
                const auto t = uint16_t(base + delta);
                auto b = pack(Id(123), Id(4));
                if (delta == 0) {
                    append(b, int32_t(0));
                    append(b, std::array<uint8_t, 16>{});
                    append(b, Id(4));
                } else if (delta < 3)
                    append(b, UINT32_MAX);
                else if (delta == 3 || delta == 7)
                    append(b, Id(3));
                else {
                    append(b, uint8_t(1));
                    for (unsigned i = 0; i < (base == 0x301d ? 6u : 5u); ++i)
                        append(b, i);
                }
                QVERIFY(acceptTextureCreationObservation(t, b));
                for (size_t n = 0; n < b.size(); ++n)
                    QVERIFY_EXCEPTION_THROWN(acceptTextureCreationObservation(t, Bytes(b).first(n)),
                                             std::runtime_error);
                auto bad = b;
                bad.push_back(0);
                QVERIFY_EXCEPTION_THROWN(acceptTextureCreationObservation(t, bad), std::runtime_error);
                if (delta == 8) {
                    bad = b;
                    bad[16] = 2;
                    QVERIFY_EXCEPTION_THROWN(acceptTextureCreationObservation(t, bad), std::runtime_error);
                    bad.resize(17);
                    bad[16] = 0;
                    QVERIFY(acceptTextureCreationObservation(t, bad));
                }
                auto c = capture(0x357e);
                c.add(101, 7, t, b);
                auto path = dir.filePath("observation.gpa_frame");
                c.save(path);
                Frame f(path.toStdWString());
                QCOMPARE(inspectCommand(f, 101)["status"], nlohmann::json("decoded"));
            }
    }
    void futureViewIsRejected() {
        QTemporaryDir dir;
        auto c = capture(0x357e);
        c.add(99, 7, 0x32, pack(Id(0), Id(1), Id(4), uint8_t(1), std::array<float, 4>{0, 1, 0, 1}));
        const auto path = dir.filePath("future.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions o;
        o.warp = true;
        Replay replay(f, o);
        try {
            replay.run();
            QFAIL("Future view must be rejected");
        } catch (const std::exception &error) {
            QVERIFY(QString::fromUtf8(error.what())
                        .contains("not available before texture/view creation event 100"));
        }
    }
    void capturedDescriptorAndExperimentOverlay() {
        try {
            QTemporaryDir dir;
            for (uint16_t t : {uint16_t(0x357d), uint16_t(0x357e), uint16_t(0x357f)}) {
                auto c = capture(t);
                auto &entry = c.entries[2];
                put(c.bytes, size_t(entry.offset + entry.size - 8), Id(5));
                auto data = pack(2560u);
                data.resize(2564);
                c.add(5, 9, 1, data);
                auto clear = pack(Id(0), Id(1), Id(4));
                if (t == 0x357f) {
                    append(clear, 1u);
                    append(clear, 0.25f);
                    append(clear, uint8_t(0));
                } else {
                    append(clear, uint8_t(1));
                    if (t == 0x357d)
                        append(clear, std::array<uint32_t, 4>{123, 0, 0, 0});
                    else
                        append(clear, std::array<float, 4>{0, 1, 0, 1});
                }
                c.add(101, 7, t == 0x357f ? 0x31 : t == 0x357d ? 0x33 : 0x32, clear);
                const auto path = dir.filePath("overlay.gpa_frame");
                c.save(path);
                {
                    Frame original(path.toStdWString());
                    for (bool overlay : {false, true}) {
                        auto raw = original.payload(4);
                        Raw edited(raw.begin(), raw.end());
                        if (overlay)
                            put(edited, t == 0x357f ? 36 : 32, 0u);
                        Frame frame(original, {{4, edited}});
                        ReplayOptions o;
                        o.warp = true;
                        Replay replay(frame, o);
                        replay.run();
                        Raw expected(2560);
                        const size_t offset = overlay ? 1280 : 2304, length = overlay ? 1024 : 256;
                        const uint32_t pixel = t == 0x357f ? 0x3e800000u : t == 0x357d ? 123u : 0xff00ff00u;
                        for (size_t i = offset; i < offset + length; i += 4)
                            put(expected, i, pixel);
                        QCOMPARE(replay.readTexture(3), expected);
                    }
                }
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void defaultDescriptorMismatchCannotBeHiddenByOverlay() {
        QTemporaryDir dir;
        for (uint16_t t : {uint16_t(0x357d), uint16_t(0x357e), uint16_t(0x357f)}) {
            auto c = capture(t, false);
            const auto path = dir.filePath("default.gpa_frame");
            c.save(path);
            Frame original(path.toStdWString());
            auto raw = original.payload(4);
            Raw edited(raw.begin(), raw.end());
            const size_t mip = t == 0x357f ? 36 : 32;
            put(edited, mip, 0u);
            put(edited, mip + 4, 0u);
            put(edited, mip + 8, 2u);
            Frame frame(original, {{4, edited}});
            ReplayOptions o;
            o.warp = true;
            Replay replay(frame, o);
            try {
                replay.run();
                QFAIL("Default descriptor mismatch must fail before applying an experiment");
            } catch (const std::exception &error) {
                QVERIFY(QString::fromUtf8(error.what()).contains("does not reproduce the saved view"));
            }
        }
    }
    void originalCaptures_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int m : {0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23})
            for (bool w : {false, true})
                QTest::newRow(QString("%1-%2").arg(m).arg(w ? "warp" : "hardware").toUtf8().constData())
                    << m << w;
    }
    void originalCaptures() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_VIEW_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_VIEW_CREATION_CAPTURES to original corpus");
        auto folder = root + QString("/%1/").arg(mode);
        Frame f((folder + "capture.gpa_frame").toStdWString());
        auto expected = fileBytes(folder + "native/expected.bin");
        Id view = 0, resource = 0, creation = 0, clear = 0;
        unsigned observations = 0;
        for (auto &[id, c] : auditTextureCreations(f).records) {
            QVERIFY2(c.error.empty(), c.error.c_str());
            QCOMPARE(inspectCommand(f, id)["status"], nlohmann::json("decoded"));
            if (c.result)
                ++observations;
            else {
                view = c.resource;
                resource = c.source;
                creation = id;
            }
        }
        QVERIFY(view && resource && creation);
        QCOMPARE(observations, mode % 10 >= 2 ? 1u : 0u);
        for (auto &[id, e] : f.entries()) {
            if (e.category == 7 && viewCreationObservation(e.type) != ViewObservation::None)
                QCOMPARE(inspectCommand(f, id)["status"], nlohmann::json("decoded"));
            if (id > creation && e.category == 7 && (e.type == 0x31 || e.type == 0x32 || e.type == 0x33)) {
                Reader r(f.payload(id));
                r.skip(16);
                if (r.read<Id>() == view)
                    clear = id;
            }
        }
        QVERIFY(clear);
        QCOMPARE(validateFrame(f.path())["errors"], nlohmann::json(0));
        ReplayOptions o;
        o.warp = warp;
        Replay replay(f, o);
        bool observed = false;
        replay.run({}, {}, [&](Id event, bool after, auto *, const auto &) {
            if (event == clear && after) {
                observed = true;
                QCOMPARE(replay.readTexture(resource), expected);
            }
        });
        QVERIFY(observed);
        QCOMPARE(replay.readTexture(resource), expected);
    }
};
QTEST_GUILESS_MAIN(ViewCreationTests)
#include "ViewCreationTests.moc"
