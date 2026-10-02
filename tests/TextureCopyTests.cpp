#include "StateCapture.h"
#include "application/FrameValidation.h"
#include "core/CopyCommands.h"
#include "core/TextureCopies.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
Resource texture(Id id = 2, uint32_t format = 28, uint32_t samples = 1) {
    return {id, 0, 0, 0, 0x85, {16, 16, 3, 2, format, samples, 0, 0, 0, 0, 0}};
}
CopyCommand copy(uint16_t type = 0x40) {
    CopyCommand c;
    c.type = type;
    c.context = 1;
    c.destination = 2;
    c.source = 4;
    return c;
}
} // namespace
class TextureCopyTests : public QObject {
    Q_OBJECT
  private slots:
    void formatsAndSubresources() {
        auto a = texture(4), b = texture();
        auto c = copy();
        c.sourceSubresource = 4;
        c.destinationSubresource = 1;
        c.x = 2;
        c.y = 1;
        c.box = std::array<uint32_t, 6>{1, 2, 0, 5, 6, 1};
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        a.id = b.id;
        c.sourceSubresource = 4;
        c.destinationSubresource = 1;
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        a = texture(4, 27);
        b = texture(2, 29);
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        for (const auto pair :
             std::vector<std::array<uint32_t, 2>>{{87, 91}, {88, 93}, {53, 56}, {70, 72}, {94, 96}}) {
            a = texture(4, pair[0]);
            b = texture(2, pair[1]);
            QCOMPARE(validateTextureCopy(a, b, copy(0x3e)), CopyValidation::Texture);
        }
        a = texture(4, 17);
        b = texture(2, 71);
        QCOMPARE(validateTextureCopy(a, b, copy(0x3e)), CopyValidation::TextureReferences);
        a = texture(4, 103);
        b = texture(2, 103);
        a.desc[2] = b.desc[2] = 1;
        QCOMPARE(validateTextureCopy(a, b, copy(0x3e)), CopyValidation::TextureReferences);
        a = texture(4, 71);
        b = texture(2, 71);
        c = copy();
        c.sourceSubresource = c.destinationSubresource = 2;
        c.box = std::array<uint32_t, 6>{0, 0, 0, 4, 4, 1};
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        a.desc[0] = b.desc[0] = 8;
        a.desc[1] = b.desc[1] = 8;
        a.desc[2] = b.desc[2] = 4;
        c.sourceSubresource = c.destinationSubresource = 3;
        c.box = std::array<uint32_t, 6>{0, 0, 0, 1, 1, 1};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTextureCopy(a, b, c));
        c.box = std::array<uint32_t, 6>{0, 0, 0, 4, 4, 1};
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        c.box.reset();
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        a = texture(4);
        b = texture();
        a.desc[2] = b.desc[2] = 0;
        c = copy();
        c.sourceSubresource = c.destinationSubresource = 9;
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
    }
    void rejectedDescriptorsAndRegions() {
        using Mutation = std::function<void(Resource &, Resource &, CopyCommand &)>;
        const std::vector<Mutation> mutations{
            [](auto &a, auto &, auto &) { a.desc[0] = 0; },
            [](auto &a, auto &, auto &) { a.desc[2] = 33; },
            [](auto &a, auto &, auto &) { a.desc[3] = 0; },
            [](auto &a, auto &, auto &) { a.desc[4] = UINT32_MAX; },
            [](auto &a, auto &, auto &) { a.desc[5] = 0; },
            [](auto &, auto &, auto &c) { c.sourceSubresource = 6; },
            [](auto &, auto &, auto &c) { c.destinationSubresource = UINT32_MAX; },
            [](auto &, auto &, auto &c) { c.x = UINT32_MAX; },
            [](auto &, auto &, auto &c) { c.y = 1; },
            [](auto &, auto &, auto &c) { c.z = 1; },
            [](auto &a, auto &, auto &) { a.desc[4] = 87; },
            [](auto &a, auto &, auto &) {
                a.desc[5] = 2;
                a.desc[2] = 1;
            },
            [](auto &a, auto &, auto &) { a.desc[6] = 1; },
            [](auto &, auto &, auto &c) { c.box = std::array<uint32_t, 6>{0, 0, 0, 17, 16, 1}; },
            [](auto &, auto &, auto &c) { c.box = std::array<uint32_t, 6>{0, 0, 0, 16, 16, 2}; },
            [](auto &a, auto &b, auto &) { a.id = b.id; },
            [](auto &a, auto &b, auto &c) {
                a.desc[8] = b.desc[8] = 64;
                c.box = std::array<uint32_t, 6>{0, 0, 0, 16, 16, 1};
            },
            [](auto &a, auto &b, auto &c) {
                a.desc[4] = b.desc[4] = 71;
                c.box = std::array<uint32_t, 6>{1, 0, 0, 4, 4, 1};
            },
            [](auto &a, auto &b, auto &c) {
                a.desc[4] = b.desc[4] = 71;
                c.box = std::array<uint32_t, 6>{0, 0, 0, 3, 4, 1};
            },
            [](auto &a, auto &b, auto &c) {
                a.desc[4] = b.desc[4] = 71;
                c.box = std::array<uint32_t, 6>{0, 0, 0, 4, 4, 1};
                c.x = 1;
            },
            [](auto &a, auto &, auto &c) {
                c.type = 0x3e;
                a.desc[0] = 8;
            },
            [](auto &a, auto &, auto &c) {
                c.type = 0x3e;
                a.desc[2] = 2;
            },
            [](auto &a, auto &, auto &c) {
                c.type = 0x3e;
                a.desc[3] = 1;
            }};
        QTemporaryDir dir;
        for (const auto &mutate : mutations) {
            auto a = texture(4), b = texture();
            auto c = copy();
            mutate(a, b, c);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTextureCopy(a, b, c));
        }
        auto a = texture(4), b = texture();
        auto c = copy();
        c.box = std::array<uint32_t, 6>{4, 0, 0, 4, 4, 1};
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        c.x = UINT32_MAX;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTextureCopy(a, b, c));
    }
    void resolves() {
        auto a = texture(4, 28, 4), b = texture();
        a.desc[2] = 1;
        b.desc[2] = 1;
        auto c = copy(0x42);
        c.format = 28;
        QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        for (auto pair : std::vector<std::array<uint32_t, 2>>{{27, 28}, {28, 27}, {27, 27}}) {
            a.desc[4] = pair[0];
            b.desc[4] = pair[1];
            QCOMPARE(validateTextureCopy(a, b, c), CopyValidation::Texture);
        }
        using Mutation = std::function<void(Resource &, Resource &, CopyCommand &)>;
        for (const auto &mutate :
             std::vector<Mutation>{[](auto &a, auto &, auto &) { a.desc[5] = 1; },
                                   [](auto &, auto &b, auto &) { b.desc[5] = 4; },
                                   [](auto &, auto &b, auto &) { b.desc[7] = 3; },
                                   [](auto &, auto &b, auto &) { b.desc[0] = 8; },
                                   [](auto &, auto &, auto &c) { c.format = 27; },
                                   [](auto &, auto &, auto &c) { c.format = 87; },
                                   [](auto &, auto &, auto &c) { c.format = 29; },
                                   [](auto &, auto &, auto &c) { c.sourceSubresource = 2; }}) {
            a = texture(4, 28, 4);
            b = texture();
            a.desc[2] = b.desc[2] = 1;
            c = copy(0x42);
            c.format = 28;
            mutate(a, b, c);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTextureCopy(a, b, c));
        }
        auto raw = statePack(Id(0), Id(1), Id(2), 0u, Id(4), 0u, 28u);
        QCOMPARE(readCopyCommand(0x42, raw).format, 28u);
        for (size_t n = 0; n < raw.size(); ++n)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readCopyCommand(0x42, Bytes(raw).first(n)));
        raw.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readCopyCommand(0x42, raw));
    }
    void preflightAndRuntimeReject() {
        QTemporaryDir dir;
        Capture capture;
        capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
        for (Id id : {2, 4}) {
            auto raw = statePack(Id(0), Id(0));
            for (auto v : texture(id).desc)
                append(raw, v);
            append(raw, Id(0));
            capture.add(id, 5, 0x85, raw);
        }
        capture.add(100, 7, 0x40,
                    statePack(Id(0), Id(1), Id(2), 0u, UINT32_MAX, 0u, 0u, Id(4), 0u, uint8_t(0)));
        const auto path = dir.filePath("overflow.gpa_frame");
        capture.save(path);
        Frame f(path.toStdWString());
        const auto result = validateFrame(path.toStdWString());
        QCOMPARE(result["status"], nlohmann::json("blocked"));
        bool found = false;
        for (const auto &v : result["findings"])
            if (v["kind"] == "copy_command_rejected") {
                QCOMPARE(v["event_id"], nlohmann::json(100));
                found = true;
            }
        QVERIFY(found);
        ReplayOptions options;
        options.warp = true;
        Replay replay(f, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        const auto output = qEnvironmentVariable("FLORA_TEXTURE_COPY_ARTIFACT_DIR");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            capture.save(output + "/overflow.gpa_frame");
        }
    }
    void originalBytes() {
        const auto root = qEnvironmentVariable("FLORA_TEXTURE_COPY_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_TEXTURE_COPY_CAPTURES to the original texture corpus");
        for (int mode = 0; mode < 11; ++mode)
            for (bool warp : {false, true}) {
                const auto folder = root + QString("/%1/").arg(mode);
                Frame frame((folder + "capture.gpa_frame").toStdWString());
                QFile file(folder + "native/expected.bin");
                QVERIFY(file.open(QIODevice::ReadOnly));
                const auto bytes = file.readAll();
                const std::vector<uint8_t> expected(bytes.begin(), bytes.end());
                Id event = 0, resource = 0;
                for (const auto &[id, entry] : frame.entries())
                    if (entry.category == 7 && isCopyCommand(entry.type)) {
                        const auto c = readCopyCommand(entry.type, frame.payload(id));
                        auto r = frame.resource(c.destination);
                        if (r.type >= 0x84 && r.type <= 0x86 && textureInfo(r).samples == 1) {
                            event = id;
                            resource = c.destination;
                            break;
                        }
                    }
                QVERIFY(event && resource);
                QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
                ReplayOptions options;
                options.warp = warp;
                Replay replay(frame, options);
                bool observed = false;
                replay.run({}, {}, [&](Id id, bool after, auto *, const auto &) {
                    if (id == event && after) {
                        QCOMPARE(replay.readTexture(resource), expected);
                        observed = true;
                    }
                });
                QVERIFY(observed);
                QCOMPARE(replay.readTexture(resource), expected);
            }
    }
};
QTEST_GUILESS_MAIN(TextureCopyTests)
#include "TextureCopyTests.moc"
