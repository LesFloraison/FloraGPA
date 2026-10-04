#include "ConstantBufferCapture.h"
#include "application/FrameValidation.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing CB lifetime oracle");
    const auto data = file.readAll();
    return {reinterpret_cast<const uint8_t *>(data.data()),
            reinterpret_cast<const uint8_t *>(data.data()) + data.size()};
}
Id firstMissingSetter(const Frame &frame) {
    for (const auto &[id, e] : frame.entries())
        if (e.category == 7 && constantBufferStage(e.type))
            for (auto buffer : readConstantBufferSetter(e.type, frame.payload(id)).buffers)
                if (buffer && !frame.entries().contains(buffer))
                    return id;
    throw std::runtime_error("Missing original unused-CB evidence");
}
void clone(const Frame &frame, const QString &path, Id edited, const Raw &replacement, uint16_t type = 0,
           Id removed = 0) {
    Capture capture;
    for (const auto &[id, e] : frame.entries()) {
        if (id == removed)
            continue;
        const auto raw = frame.payload(id);
        capture.add(id, e.category, id == edited && type ? type : e.type,
                    id == edited ? replacement : Raw(raw.begin(), raw.end()));
    }
    capture.save(path);
}
} // namespace
class ConstantBufferLifetimeTests final : public QObject {
    Q_OBJECT
  private slots:
    void slotLifetimes_data() {
        QTest::addColumn<unsigned>("type");
        QTest::addColumn<int>("variant");
        for (unsigned stage = 0; stage < 6; ++stage)
            for (unsigned type : {0x249 + stage, 0x24f + stage, unsigned(constantBufferShimTypes[stage])})
                for (int variant = 0; variant < 8; ++variant)
                    QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(variant))) << type << variant;
    }
    void slotLifetimes() {
        QFETCH(unsigned, type);
        QFETCH(int, variant);
        Capture c;
        c.add(1, 5, 0x127, statePack(Id(0), Id(0), 0u, 0u));
        c.buffer(2, 3, D3D11_BIND_CONSTANT_BUFFER, 0, {1, 2, 3, 4});
        // Missing slots 2 and 4, with a saved buffer in slot 3. Partial closes
        // must preserve the unrelated saved slot and close both missing slots.
        c.add(10, 7, uint16_t(type), cbSetter(uint16_t(type), 2, {900, 2, 901}));
        if (variant == 0)
            c.add(20, 7, 0x242, statePack(Id(0), Id(1)));
        else {
            c.add(15, 7, uint16_t(type), cbSetter(uint16_t(type), 2, {0}));
            c.add(20, 7, uint16_t(type), cbSetter(uint16_t(type), variant == 2 ? 5 : 4, {2}));
        }
        if (variant == 3)
            c.add(12, 7, 0x35, statePack(Id(0), Id(0), Id(1), 1u, 1u, 1u));
        if (variant == 4)
            c.add(12, 7, 0x41, statePack(Id(0), Id(1), Id(1000), 0u));
        if (variant == 5) {
            auto raw = cbSetter(uint16_t(type), 2, {900});
            put(raw, 0, Id(99));
            c.add(12, 7, uint16_t(type), raw);
        }
        if (variant == 6)
            c.add(12, 7, uint16_t(type), cbSetter(uint16_t(type), 2, {902}));
        if (variant == 7) {
            auto raw = cbSetter(uint16_t(type), 0, {0});
            raw.pop_back();
            c.add(12, 7, uint16_t(type), raw);
        }
        QTemporaryDir directory;
        const auto path = directory.filePath("slots.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        const bool good = variant == 0 || variant == 1 || variant == 6;
        if (!good) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, proveUnusedConstantBufferLifetime(frame, 10));
            QVERIFY(validateFrame(frame.path())["errors"].get<unsigned>() > 0);
            // A disabled Draw still prepares bindings and must not hide a gap.
            if (variant == 3)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         proveUnusedConstantBufferLifetime(frame, 10, {12}));
            return;
        }
        const auto proof = proveUnusedConstantBufferLifetime(frame, 10);
        QCOMPARE(proof.closingEvent, Id(20));
        QCOMPARE(proof.stage, *constantBufferStage(uint16_t(type)));
        QCOMPARE(proof.resources.size(), size_t(variant == 6 ? 3 : 2));
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        replay.inspectNativeState([&](auto *context, const auto &objects) {
            QVERIFY(!objects.contains(900) && !objects.contains(901) && !objects.contains(902));
            const std::array methods{
                &ID3D11DeviceContext::VSGetConstantBuffers, &ID3D11DeviceContext::HSGetConstantBuffers,
                &ID3D11DeviceContext::DSGetConstantBuffers, &ID3D11DeviceContext::GSGetConstantBuffers,
                &ID3D11DeviceContext::PSGetConstantBuffers, &ID3D11DeviceContext::CSGetConstantBuffers};
            ID3D11Buffer *buffers[3]{};
            (context->*methods[proof.stage])(2, 3, buffers);
            QVERIFY(!buffers[0]);
            QCOMPARE(bool(buffers[1]), variant != 0);
            QCOMPARE(bool(buffers[2]), variant != 0);
            if (variant != 0)
                QCOMPARE(buffers[1], buffers[2]);
            for (auto buffer : buffers)
                if (buffer)
                    buffer->Release();
        });
    }
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
        const auto root = qEnvironmentVariable("FLORA_CB_LIFETIMES");
        if (root.isEmpty())
            QSKIP("Set FLORA_CB_LIFETIMES to the original omitted-sentinel corpus");
        const auto folder = root + QString("/%1/").arg(mode);
        Frame frame((folder + "capture.gpa_frame").toStdWString());
        const auto first = firstMissingSetter(frame);
        const auto proof = proveUnusedConstantBufferLifetime(frame, first);
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(replay.output().rgba, bytes(folder + "native/expected.rgba"));
            QCOMPARE(replay.unusedConstantBufferLifetimes().size(), size_t(3));
            replay.inspectNativeState([&](auto *, const auto &objects) {
                for (auto id : proof.resources)
                    QVERIFY(!objects.contains(id));
            });
        }
        options.until = first;
        Replay stopped(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, stopped.run());
        options.until = proof.closingEvent;
        options.before = true;
        Replay beforeClose(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, beforeClose.run());
        options.before = false;
        Replay afterClose(frame, options);
        afterClose.run();
        options.until = 0;
        Replay observer(frame, options);
        bool exposed = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 observer.run({}, {}, [&](Id id, bool after, auto *, const auto &) {
                                     exposed |= id == first && after;
                                 }));
        QVERIFY(!exposed);
        Replay scoped(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 scoped.run({}, {}, {}, [](Id, const auto &body) { body(); }));
        Replay drawObserved(frame, options);
        drawObserved.run({}, [](Id, bool, auto *, const auto &) {});
        QCOMPARE(drawObserved.output().rgba, bytes(folder + "native/expected.rgba"));
    }
    void malformedAndEditedIntervals() {
        const auto root = qEnvironmentVariable("FLORA_CB_LIFETIMES");
        if (root.isEmpty())
            QSKIP("Set FLORA_CB_LIFETIMES to the original omitted-sentinel corpus");
        Frame frame((root + "/0/capture.gpa_frame").toStdWString());
        const auto first = firstMissingSetter(frame);
        const auto proof = proveUnusedConstantBufferLifetime(frame, first);
        QTemporaryDir directory;
        const auto path = directory.filePath("negative.gpa_frame");
        for (Id event : {first, Id(25), Id(27), proof.closingEvent}) {
            const auto raw = frame.payload(event);
            for (size_t length = 0; length < raw.size(); ++length) {
                clone(frame, path, event, Raw(raw.begin(), raw.begin() + length));
                Frame invalid(path.toStdWString());
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         proveUnusedConstantBufferLifetime(invalid, first));
                QVERIFY(validateFrame(invalid.path())["errors"].get<unsigned>() > 0);
            }
        }
        for (unsigned variant = 0; variant < 5; ++variant) {
            ReplayOptions options;
            options.warp = true;
            if (variant == 0)
                options.disabled.insert(proof.closingEvent);
            if (variant == 1) {
                auto raw = frame.payload(proof.closingEvent);
                options.commandPayloads[proof.closingEvent] = Raw(raw.begin(), raw.end());
                put(options.commandPayloads[proof.closingEvent], 0, Id(77));
            }
            if (variant == 2 || variant == 3) {
                const auto id = variant == 2 ? first : proof.closingEvent;
                options.constantBufferSetters[id] =
                    readConstantBufferSetter(frame.entry(id).type, frame.payload(id));
            }
            if (variant == 4)
                options.commandPayloads[25] = Raw(1);
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
        clone(frame, path, 0, {}, 0, proof.closingEvent);
        {
            Frame noClose(path.toStdWString());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, proveUnusedConstantBufferLifetime(noClose, first));
        }
        for (unsigned variant = 0; variant < 8; ++variant) {
            const Id target = variant == 4 ? proof.closingEvent : variant >= 6 ? 25 : first;
            const auto bytes = frame.payload(target);
            Raw raw(bytes.begin(), bytes.end());
            if (variant == 0)
                put(raw, 0, Id(1)); // Unresolved link.
            if (variant == 1 || variant == 4)
                put(raw, 8, Id(3)); // Device is not an immediate context.
            if (variant == 2)
                put(raw, 25, Id(3)); // Wrong resource family, not an absent-only placeholder.
            if (variant == 3)
                put(raw, 20, UINT32_MAX);
            if (variant == 5)
                raw.push_back(0);
            if (variant == 6)
                put(raw, 20, Id(999999)); // Upload requires actual saved resource storage.
            if (variant == 7)
                put(raw, 32, 1u); // Captured write cannot be reinterpreted as a READ observation.
            clone(frame, path, target, raw);
            Frame invalid(path.toStdWString());
            QVERIFY(validateFrame(invalid.path())["errors"].get<unsigned>() > 0);
            ReplayOptions options;
            options.warp = true;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, ([&] {
                                         Replay replay(invalid, options);
                                         replay.run();
                                     }()));
        }
    }
};
QTEST_GUILESS_MAIN(ConstantBufferLifetimeTests)
#include "ConstantBufferLifetimeTests.moc"
