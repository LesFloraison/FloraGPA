#include "StreamCapture.h"
#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/Commands.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing SO lifetime oracle");
    const auto data = file.readAll();
    return {reinterpret_cast<const uint8_t *>(data.data()),
            reinterpret_cast<const uint8_t *>(data.data()) + data.size()};
}
QString fixture(unsigned mode) {
    const auto root = qEnvironmentVariable("FLORA_SO_LIFETIMES");
    return root.isEmpty() ? QString{} : root + QString("/%1/").arg(mode);
}
void clone(const Frame &frame, const QString &path, Id edited = 0, const Raw &replacement = {},
           uint16_t type = 0, Id removed = 0) {
    Capture capture;
    for (const auto &[id, entry] : frame.entries()) {
        if (id == removed)
            continue;
        auto raw = frame.payload(id);
        capture.add(id, entry.category, id == edited && type ? type : entry.type,
                    id == edited ? replacement : Raw(raw.begin(), raw.end()));
    }
    capture.save(path);
}
Id firstSetter(const Frame &frame) {
    for (const auto &[id, entry] : frame.entries())
        if (entry.category == 7 && isStreamOutputTargets(entry.type))
            return id;
    throw std::runtime_error("Missing original SO setter");
}
} // namespace
class SoLifetimeTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 9; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp))) << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = fixture(mode);
        if (root.isEmpty())
            QSKIP("Set FLORA_SO_LIFETIMES to the original corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto first = firstSetter(frame);
        const auto binding = readStreamOutputTargets(frame.payload(first));
        const auto validation = validateFrame(frame.path());
        QCOMPARE(validation["errors"], nlohmann::json(0));
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(replay.output().rgba, bytes(root + "native/expected.rgba"));
            QCOMPARE(replay.unusedStreamOutputLifetimes().size(), size_t(mode < 5 ? (mode == 3 ? 2 : 1) : 0));
            if (mode >= 5)
                QCOMPARE(replay.readBuffer(binding.buffers->at(0)), bytes(root + "native/so.bin"));
            QCOMPARE(replay.streamOutputHistory.size(), size_t(mode >= 6 ? 1 : 0));
            if (mode >= 6) {
                QCOMPARE(replay.streamOutputHistory[0].written, uint64_t(1));
                QCOMPARE(replay.streamOutputHistory[0].needed, uint64_t(1));
            }
            if (mode < 5)
                replay.inspectNativeState([&](auto *context, const auto &objects) {
                    QVERIFY(!objects.contains(binding.buffers->at(0)));
                    ID3D11Buffer *buffers[4]{};
                    context->SOGetTargets(4, buffers);
                    for (auto buffer : buffers) {
                        QVERIFY(!buffer);
                        if (buffer)
                            buffer->Release();
                    }
                });
        }
        if (mode >= 5)
            return;
        const auto proof = proveUnusedStreamOutputLifetime(frame, first);
        // The prefix cannot manufacture native state inside the absent binding.
        for (auto stop : {first, proof.closingEvent}) {
            options.until = stop;
            options.before = stop == proof.closingEvent;
            Replay prefix(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, prefix.run());
        }
        options.before = false;
        Replay closed(frame, options);
        closed.run();
        closed.inspectNativeState([](auto *, const auto &) {});
        options.until = 0;
        options.disabled.insert(proof.closingEvent);
        Replay noClose(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, noClose.run());
        options.disabled.clear();
        auto closing = frame.payload(proof.closingEvent);
        options.commandPayloads[proof.closingEvent] = Raw(closing.begin(), closing.end());
        put(options.commandPayloads[proof.closingEvent], 0, Id(1));
        Replay editedClose(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, editedClose.run());
        options.commandPayloads.clear();
        bool observedUnresolved = false;
        Replay observed(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 observed.run({}, {}, [&](Id id, bool after, auto *, const auto &) {
                                     observedUnresolved |= id == first && after;
                                 }));
        QVERIFY(!observedUnresolved);
        Replay scoped(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 scoped.run({}, {}, {}, [](Id, const auto &body) { body(); }));
        // Draw-only observation occurs after the interval has closed.
        Replay drawObserved(frame, options);
        drawObserved.run({}, [](Id, bool, auto *, const auto &) {});
        QCOMPARE(drawObserved.output().rgba, bytes(root + "native/expected.rgba"));
    }
    void malformedAndConsumption() {
        auto root = fixture(0);
        if (root.isEmpty())
            QSKIP("Set FLORA_SO_LIFETIMES to the original corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto first = firstSetter(frame);
        const auto proof = proveUnusedStreamOutputLifetime(frame, first);
        const Id getter = 21;
        for (auto event : {first, getter, Id(22), proof.closingEvent}) {
            auto original = frame.payload(event);
            for (size_t length = 0; length < original.size(); ++length) {
                std::map<Id, Raw> edits{{event, Raw(original.begin(), original.begin() + length)}};
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         proveUnusedStreamOutputLifetime(frame, first, {}, edits));
            }
        }
        QTemporaryDir directory;
        const auto path = directory.path() + "/negative.gpa_frame";
        auto assertRejected = [&] {
            Frame invalid(path.toStdWString());
            QVERIFY(validateFrame(invalid.path())["errors"].get<unsigned>() > 0);
            ReplayOptions options;
            options.warp = true;
            Replay replay(invalid, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        };
        const auto original = frame.payload(first);
        for (unsigned variant = 0; variant < 8; ++variant) {
            Raw raw(original.begin(), original.end());
            if (variant == 0)
                put(raw, 0, Id(1));
            if (variant == 1)
                put(raw, 8, Id(0));
            if (variant == 2)
                put(raw, 8, Id(5)); // Texture is not a context.
            if (variant == 3)
                put(raw, 16, uint32_t(5));
            if (variant == 4)
                raw[20] = 2;
            if (variant == 5)
                put(raw, 30, uint32_t(3));
            if (variant == 6)
                raw.push_back(0);
            if (variant == 7)
                raw.resize(raw.size() - 1);
            clone(frame, path, first, raw);
            assertRejected();
        }
        // A saved deferred context cannot be reinterpreted as an immediate lifetime.
        auto context = frame.payload(2);
        Raw deferred(context.begin(), context.end());
        put(deferred, 16, uint32_t(1));
        clone(frame, path, 2, deferred);
        assertRejected();
        // Replacing the observation with an actual Draw must block, even if its
        // snapshot would rebind a complete pipeline and hide the earlier target.
        for (const auto &[id, entry] : frame.entries())
            if (entry.category == 7 && isDraw(entry.type)) {
                auto raw = frame.payload(id);
                clone(frame, path, getter, Raw(raw.begin(), raw.end()), entry.type);
                assertRejected();
                break;
            }
        clone(frame, path, 0, {}, 0, proof.closingEvent);
        assertRejected();
        for (unsigned variant = 0; variant < 3; ++variant) {
            auto payload = frame.payload(22);
            Raw raw(payload.begin(), payload.end());
            if (variant == 0)
                put(raw, 0, Id(1));
            if (variant == 1)
                put(raw, 8, Id(90000));
            if (variant == 2)
                put(raw, 16, uint32_t(0));
            clone(frame, path, 22, raw);
            assertRejected();
        }
        Frame mixed((fixture(6) + "capture.gpa_frame").toStdWString());
        auto targets = readStreamOutputTargets(mixed.payload(firstSetter(mixed)));
        clone(mixed, path, 0, {}, 0, targets.buffers->at(1));
        assertRejected(); // Saved target cursor reset cannot be discarded.
    }
    void retainedSavedCursor() {
        for (bool warp : {false, true})
            for (bool dirty : {false, true}) {
                auto capture = streamCapture(false, dirty);
                capture.add(110, 7, 0x3503,
                            statePack(Id(0), Id(1), 1u, uint8_t(1), Id(99999), uint8_t(1), 4u));
                capture.add(111, 7, 0x353b, statePack(Id(0), Id(1), 1u, uint8_t(1), Id(99999)));
                capture.add(112, 7, 0x3014, statePack(Id(0), Id(99999), 2u));
                capture.add(113, 7, 0x3503, statePack(Id(0), Id(1), 0u, uint8_t(0), uint8_t(0)));
                QTemporaryDir directory;
                const auto path = directory.path() + "/cursor.gpa_frame";
                capture.save(path);
                Frame frame(path.toStdWString());
                ReplayOptions options;
                options.warp = warp;
                options.until = 200;
                Replay replay(frame, options);
                replay.run();
                QCOMPARE(replay.unusedStreamOutputLifetimes().size(), size_t(1));
                QCOMPARE(replay.drawAutoParameters(200).vertexCount, dirty ? 3u : 6u);
                QVERIFY(replay.drawAutoParameters(200).verified);
                QCOMPARE(replay.streamOutputHistory.size(), size_t(2));
                QCOMPARE(replay.streamOutputHistory[0].written, uint64_t(1));
                QCOMPARE(replay.streamOutputHistory[1].written, uint64_t(1));
                auto raw = replay.readBuffer(70);
                Reader reader(Bytes(raw).subspan(16));
                for (unsigned triangle = 0; triangle < (dirty ? 1u : 2u); ++triangle)
                    for (const auto &point :
                         std::array<std::array<float, 4>, 3>{{{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}}})
                        QCOMPARE((reader.array<float, 4>()), point);
            }
    }
};
QTEST_GUILESS_MAIN(SoLifetimeTests)
#include "SoLifetimeTests.moc"
