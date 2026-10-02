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
template <class... T> Raw pack(T... values) {
    Raw out;
    (append(out, values), ...);
    return out;
}
const std::array<uint32_t, 11> desc{4, 4, 1, 1, 28, 1, 0, 0, 8, 0, 0};
Raw call(bool initial = true, int32_t result = 0, Id returned = 3) {
    auto out = pack(Id(0), Id(2), result, uint8_t(1), desc, uint8_t(initial));
    if (initial) {
        append(out, UINT64_MAX);
        append(out, 16u);
        append(out, 64u);
    }
    append(out, returned);
    return out;
}
Capture capture(bool initial = true) {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    c.add(2, 5, 0x81, Raw(28));
    c.add(3, 5, 0x85, pack(Id(0), Id(2), desc, Id(initial ? 4 : 999)));
    auto bytes = pack(64u);
    bytes.insert(bytes.end(), 64, 71);
    c.add(4, 9, 1, bytes);
    c.add(100, 7, 0x357a, call(initial));
    return c;
}
void replace(Capture &c, Id id, Raw bytes) {
    auto it = std::find_if(c.entries.begin(), c.entries.end(), [&](const auto &e) { return e.id == id; });
    it->offset = c.bytes.size();
    it->size = uint32_t(bytes.size());
    c.bytes.insert(c.bytes.end(), bytes.begin(), bytes.end());
}
Raw fileBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing fixture oracle");
    auto b = f.readAll();
    return Raw(b.begin(), b.end());
}
} // namespace
class TextureCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void strictWireAndReferences() {
        QTemporaryDir dir;
        auto verify = [&](Capture c, bool accepted) {
            auto path = dir.filePath("texture.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            const auto audit = auditTextureCreations(f);
            if (accepted) {
                QVERIFY2(audit.records.at(100).error.empty(), audit.records.at(100).error.c_str());
            } else {
                QVERIFY_EXCEPTION_THROWN(requireTextureCreation(audit, 100), std::runtime_error);
                QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
            }
        };
        for (bool initial : {false, true}) {
            const auto bytes = call(initial);
            verify(capture(initial), true);
            for (size_t n = 0; n < bytes.size(); ++n) {
                auto c = capture(initial);
                replace(c, 100, Raw(bytes.begin(), bytes.begin() + n));
                verify(c, false);
            }
            auto c = capture(initial);
            auto extra = bytes;
            extra.push_back(0);
            replace(c, 100, extra);
            verify(c, false);
        }
        for (auto [offset, value] : std::vector<std::pair<size_t, uint32_t>>{
                 {0, 1}, {8, 999}, {16, 2}, {20, 2}, {21, 8}, {29, 33}, {65, 2}, {74, 1}}) {
            auto c = capture();
            auto b = call();
            put(b, offset, value);
            replace(c, 100, b);
            verify(c, false);
        }
        auto c = capture();
        auto nullPointer = call();
        put(nullPointer, 66, Id(0));
        replace(c, 100, nullPointer);
        verify(c, false);
        c = capture();
        replace(c, 4, pack(4u, 0u));
        verify(c, false);
        c = capture();
        c.add(101, 7, 0x357a, call());
        verify(c, false);
        for (auto hr : {int32_t(-2147024809), int32_t(1)}) {
            c = capture();
            replace(c, 100, call(false, hr, 0));
            verify(c, true);
            replace(c, 100, call(false, hr, 3));
            verify(c, false);
        }
        // These wire layouts are recovered; GPU semantics remain explicitly outside accepted scope.
        for (uint16_t t : {uint16_t(0x3579), uint16_t(0x357b)}) {
            auto raw = pack(Id(0), Id(2), int32_t(0), uint8_t(1));
            std::vector<uint32_t> d = t == 0x3579 ? std::vector<uint32_t>{4, 1, 1, 28, 0, 8, 0, 0}
                                                  : std::vector<uint32_t>{4, 4, 4, 1, 28, 0, 8, 0, 0};
            for (auto word : d)
                append(raw, word);
            append(raw, uint8_t(0));
            append(raw, Id(3));
            QCOMPARE(readTextureCreation(t, raw).descriptor, d);
            for (size_t n = 0; n < raw.size(); ++n)
                QVERIFY_EXCEPTION_THROWN(readTextureCreation(t, Bytes(raw).first(n)), std::runtime_error);
        }
    }
    void observationsAreStrictMetadata() {
        std::map<uint16_t, Raw> records{
            {0x313f, pack(Id(123), Id(3), int32_t(0), std::array<uint8_t, 16>{}, Id(3))},
            {0x3140, pack(Id(123), Id(3), UINT32_MAX)},
            {0x3141, pack(Id(123), Id(3), 0u)},
            {0x3142, pack(Id(123), Id(3), Id(2))},
            {0x3149, pack(Id(123), Id(3), uint8_t(1), desc)},
            {0x3592, pack(Id(123), Id(2), int32_t(0), 28u, uint8_t(1), UINT32_MAX)},
            {0x35aa, pack(Id(123), Id(2), int32_t(0), 28u, 1u, 0u, uint8_t(1), 1u)}};
        for (auto [t, b] : records) {
            QVERIFY(acceptTextureCreationObservation(t, b));
            for (size_t n = 0; n < b.size(); ++n)
                QVERIFY_EXCEPTION_THROWN(acceptTextureCreationObservation(t, Bytes(b).first(n)),
                                         std::runtime_error);
            b.push_back(0);
            QVERIFY_EXCEPTION_THROWN(acceptTextureCreationObservation(t, b), std::runtime_error);
        }
        for (auto [t, pos] :
             std::vector<std::pair<uint16_t, size_t>>{{0x3149, 16}, {0x3592, 24}, {0x35aa, 32}}) {
            auto b = records.at(t);
            b[pos] = 2;
            QVERIFY_EXCEPTION_THROWN(acceptTextureCreationObservation(t, b), std::runtime_error);
            b[pos] = 0;
            b.resize(pos + 1);
            QVERIFY(acceptTextureCreationObservation(t, b));
        }
        QVERIFY(!acceptTextureCreationObservation(0xffff, {}));
    }
    void srvCreationAndOverlay() {
        try {
            QTemporaryDir dir;
            const std::array<uint32_t, 6> sd{28, 4, 0, 2, 0, 0};
            auto creation = [&](bool explicitDesc) {
                auto b = pack(Id(0), Id(2), int32_t(0), Id(3), uint8_t(explicitDesc));
                if (explicitDesc)
                    append(b, sd);
                append(b, Id(5));
                return b;
            };
            auto saved = pack(Id(0), Id(2), Id(3), sd);
            for (bool explicitDesc : {false, true}) {
                auto bytes = creation(explicitDesc);
                for (size_t n = 0; n < bytes.size(); ++n)
                    QVERIFY_EXCEPTION_THROWN(readTextureCreation(0x357c, Bytes(bytes).first(n)),
                                             std::runtime_error);
                auto trailing = bytes;
                trailing.push_back(0);
                QVERIFY_EXCEPTION_THROWN(readTextureCreation(0x357c, trailing), std::runtime_error);
                auto c = capture();
                auto mipDesc = desc;
                mipDesc[2] = 2;
                replace(c, 3, pack(Id(0), Id(2), mipDesc, Id(4)));
                auto pixels = pack(80u);
                pixels.insert(pixels.end(), 80, 71);
                replace(c, 4, pixels);
                replace(c, 100,
                        pack(Id(0), Id(2), int32_t(0), uint8_t(1), mipDesc, uint8_t(1), UINT64_MAX, 16u, 64u,
                             UINT64_MAX, 8u, 16u, Id(3)));
                c.add(5, 5, 0x8c, saved);
                c.add(101, 7, 0x357c, bytes);
                c.add(102, 7, 0x34e6, pack(Id(0), Id(1), 0u, 1u, uint8_t(1), Id(5)));
                auto path = dir.filePath("view.gpa_frame");
                c.save(path);
                Frame original(path.toStdWString());
                QVERIFY(auditTextureCreations(original).records.at(101).error.empty());
                for (bool overlay : {false, true}) {
                    auto edited = saved;
                    if (overlay) {
                        put(edited, 32, 1u);
                        put(edited, 36, 1u);
                    }
                    Frame frame(original, {{5, edited}});
                    ReplayOptions options;
                    options.warp = true;
                    Replay replay(frame, options);
                    replay.run();
                    Com<ID3D11DeviceContext> context;
                    replay.nativeDevice()->GetImmediateContext(&context);
                    Com<ID3D11ShaderResourceView> view;
                    context->PSGetShaderResources(0, 1, &view);
                    QVERIFY(view);
                    D3D11_SHADER_RESOURCE_VIEW_DESC actual{};
                    view->GetDesc(&actual);
                    QCOMPARE(UINT(actual.Format), 28u);
                    QCOMPARE(actual.Texture2D.MostDetailedMip, overlay ? 1u : 0u);
                    QCOMPARE(actual.Texture2D.MipLevels, overlay ? 1u : 2u);
                }
                auto invalid = bytes;
                put(invalid, 20, Id(999));
                replace(c, 101, invalid);
                const auto missingPath = dir.filePath("missing.gpa_frame");
                c.save(missingPath);
                Frame missing(missingPath.toStdWString());
                QVERIFY(!auditTextureCreations(missing).records.at(101).error.empty());
            }
            auto inactive = sd;
            inactive[5] = 123;
            QVERIFY(textureSrvDescriptorEqual(sd, inactive));
            inactive[3] = 1;
            QVERIFY(!textureSrvDescriptorEqual(sd, inactive));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void syntheticGpuBoundaries() {
        QTemporaryDir dir;
        for (bool warp : {false, true})
            for (bool initial : {false, true}) {
                auto c = capture(initial);
                auto path = dir.filePath("texture.gpa_frame");
                c.save(path);
                Frame f(path.toStdWString());
                ReplayOptions options;
                options.warp = warp;
                options.until = 100;
                options.before = true;
                Replay before(f, options);
                before.run();
                QVERIFY_EXCEPTION_THROWN(before.readTexture(3), std::runtime_error);
                options.until = 0;
                options.before = false;
                Replay replay(f, options);
                for (int i = 0; i < 2; ++i) {
                    replay.run();
                    QCOMPARE(replay.counts.at("Device5.CreateTexture2D"), uint64_t(1));
                    if (initial)
                        QCOMPARE(replay.readTexture(3), Raw(64, 71));
                    else
                        QCOMPARE(replay.counts.at("textures_created_without_initial_data"), uint64_t(1));
                }
            }
    }
    void originalCaptures() {
        const auto root = qEnvironmentVariable("FLORA_TEXTURE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_TEXTURE_CREATION_CAPTURES to the original creation corpus");
        for (int mode = 0; mode < 7; ++mode)
            for (bool warp : {false, true}) {
                const auto folder = root + QString("/%1/").arg(mode);
                Frame frame((folder + "capture.gpa_frame").toStdWString());
                const auto initial = fileBytes(folder + "native/initial.bin"),
                           expected = fileBytes(folder + "native/expected.bin");
                const auto audit = auditTextureCreations(frame);
                Id texture = 0, creationEvent = 0;
                unsigned views = 0, observations = 0;
                for (const auto &[id, c] : audit.records) {
                    QVERIFY2(c.error.empty(), c.error.c_str());
                    QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
                    if (c.result != 0)
                        ++observations;
                    else if (c.type == 0x357a) {
                        texture = c.resource;
                        creationEvent = id;
                        QCOMPARE(c.hasInitial, mode == 1 || mode == 2 || mode == 6);
                        if (c.hasInitial) {
                            const auto bytes = frame.data(c.data);
                            QCOMPARE(Raw(bytes.begin(), bytes.end()), initial);
                        }
                    } else if (c.type == 0x357c)
                        ++views;
                }
                QVERIFY(texture && creationEvent);
                QCOMPARE(views, 1u);
                QCOMPARE(observations, mode == 4 || mode == 5 ? 1u : 0u);
                QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
                ReplayOptions options;
                options.warp = warp;
                Replay replay(frame, options);
                bool observed = false;
                replay.run({}, {}, [&](Id event, bool after, auto *, const auto &) {
                    if (event == creationEvent && after) {
                        observed = true;
                        if (mode == 1 || mode == 2 || mode == 6)
                            QCOMPARE(replay.readTexture(texture), initial);
                    }
                });
                QVERIFY(observed);
                QCOMPARE(replay.readTexture(texture), expected);
            }
    }
};
QTEST_GUILESS_MAIN(TextureCreationTests)
#include "TextureCreationTests.moc"
