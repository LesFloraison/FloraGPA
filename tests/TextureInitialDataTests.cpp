#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/TextureStorage.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
Resource texture(unsigned kind) {
    Resource r{};
    r.type = uint16_t(kind);
    if (kind == 0x84)
        r.desc = {8, 3, 2, 28, 0, 8, 0, 0};
    else if (kind == 0x86)
        r.desc = {8, 4, 4, 3, 28, 0, 8, 0, 0};
    else
        r.desc = {8, 8, 4, 2, kind == 0x71 ? 71u : 28u, 1, 0, 0, 8, 0, 0};
    if (kind == 0x71)
        r.type = 0x85;
    return r;
}
size_t storageSize(const Resource &r) {
    auto subs = textureSubresources(r);
    return size_t(subs.back().offset + subs.back().size);
}
Capture capture(const Resource &r, size_t size, bool present = true) {
    Capture c;
    std::vector<uint8_t> raw(16);
    for (auto d : r.desc)
        append(raw, d);
    append(raw, present ? Id(21) : Id(0));
    c.add(20, 5, r.type, raw);
    if (present) {
        auto bytes = word(uint32_t(size));
        for (size_t i = 0; i < size; ++i)
            bytes.push_back(uint8_t(i * 37 + 11));
        c.add(21, 9, 1, bytes);
    }
    return c;
}
} // namespace
class TextureInitialDataTests : public QObject {
    Q_OBJECT
  private slots:
    void rejectsEmptyBeforeNativeCreation() {
        QTemporaryDir dir;
        auto r = texture(0x85);
        capture(r, 0).save(dir.filePath("empty.gpa_frame"));
        Frame frame(dir.filePath("empty.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.readTexture(20));
    }
    void malformedDataRecords() {
        QTemporaryDir dir;
        const auto r = texture(0x85);
        const auto path = dir.filePath("malformed.gpa_frame");
        for (unsigned mode = 0; mode < 7; ++mode) {
            auto c = capture(r, storageSize(r));
            if (mode < 4)
                c.entries.back().size = mode;
            else if (mode == 4)
                put(c.bytes, size_t(c.entries.back().offset), UINT32_MAX);
            else if (mode == 5)
                c.entries.back().type = 0x100;
            else
                put(c.bytes, size_t(c.entries.front().offset + c.entries.front().size - 8), Id(999));
            c.save(path);
            auto report = validateFrame(path.toStdWString());
            bool located = false;
            for (const auto &finding : report["findings"])
                if (finding["kind"] == "texture_initial_data_invalid") {
                    QVERIFY(finding["resource_id"] == 20);
                    QVERIFY(finding["data_id"] == (mode == 6 ? 999 : 21));
                    located = true;
                }
            QVERIFY(located);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = true;
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.readTexture(20));
        }
    }
    void specialCaptureLayoutsRemainSeparate() {
        QTemporaryDir dir;
        const auto path = dir.filePath("special.gpa_frame");
        for (unsigned mode = 0; mode < 3; ++mode) {
            auto r = texture(0x85);
            r.desc[2] = r.desc[3] = 1;
            if (mode == 0)
                r.desc[5] = 4;
            else
                r.desc[4] = mode == 1 ? 104 : 105;
            capture(r, 0).save(path);
            auto report = validateFrame(path.toStdWString());
            QVERIFY(report["errors"] == 0 && report["warnings"].get<size_t>() > 0);
            for (const auto &finding : report["findings"])
                QVERIFY(finding["kind"] != "texture_initial_data_invalid");
        }
    }
    void emptyReplacementIsNotAbsentData() {
        QTemporaryDir dir;
        const auto r = texture(0x85);
        const auto path = dir.filePath("replacement.gpa_frame");
        capture(r, storageSize(r)).save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.textures[20] = {};
        Replay replay(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.readTexture(20));
    }
    void storageBoundaries_data() {
        QTest::addColumn<unsigned>("kind");
        QTest::addColumn<bool>("warp");
        for (unsigned kind : {0x84u, 0x85u, 0x86u, 0x71u})
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(kind).arg(warp))) << kind << warp;
    }
    void storageBoundaries() {
        QFETCH(unsigned, kind);
        QFETCH(bool, warp);
        QTemporaryDir dir;
        const auto r = texture(kind);
        const auto size = storageSize(r);
        const auto path = dir.filePath("texture.gpa_frame");
        ReplayOptions options;
        options.warp = warp;
        for (size_t length : {size_t(0), size_t(1), size - 1, size + 1}) {
            capture(r, length).save(path);
            const auto report = validateFrame(path.toStdWString());
            QVERIFY(report["completed"] == true && report["errors"].get<size_t>() > 0);
            bool located = false;
            for (const auto &finding : report["findings"])
                if (finding["kind"] == "texture_initial_data_invalid") {
                    QVERIFY(finding["resource_id"] == 20 && finding["data_id"] == 21);
                    located = true;
                }
            QVERIFY(located);
            Frame frame(path.toStdWString());
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.readTexture(20));
        }
        capture(r, size).save(path);
        {
            Frame frame(path.toStdWString());
            QVERIFY(validateFrame(path.toStdWString())["errors"] == 0);
            Replay replay(frame, options);
            auto data = frame.data(21);
            QCOMPARE(replay.readTexture(20), std::vector<uint8_t>(data.begin(), data.end()));
        }
        capture(r, 0, false).save(path);
        {
            Frame frame(path.toStdWString());
            QVERIFY(validateFrame(path.toStdWString())["errors"] == 0);
            Replay replay(frame, options);
            // No saved data is different from a saved empty initializer. Do not
            // assert values for undefined GPU storage.
            QCOMPARE(replay.readTexture(20).size(), size);
        }
    }
};
QTEST_GUILESS_MAIN(TextureInitialDataTests)
#include "TextureInitialDataTests.moc"
