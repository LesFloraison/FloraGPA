#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/Commands.h"
#include "core/CopyCommands.h"
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
        throw std::runtime_error("Missing transfer producer oracle");
    auto a = f.readAll();
    return {reinterpret_cast<const uint8_t *>(a.data()),
            reinterpret_cast<const uint8_t *>(a.data()) + a.size()};
}
Id transfer(const Frame &frame) {
    Id found{};
    for (const auto &[id, e] : frame.entries())
        if (e.category == 7 && (e.type == 0x255 || e.type == 0x256)) {
            if (found)
                throw std::runtime_error("Expected exactly one Context1 transfer");
            found = id;
        }
    if (!found)
        throw std::runtime_error("Missing Context1 transfer");
    return found;
}
void copyFrame(const Frame &source, const QString &path, const std::function<void(Id, Raw &)> &edit) {
    Capture copy;
    for (const auto &[id, e] : source.entries()) {
        auto payload = source.payload(id);
        Raw raw(payload.begin(), payload.end());
        edit(id, raw);
        copy.add(id, e.category, e.type, raw);
    }
    copy.save(path);
}
} // namespace
class Transfer1Tests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 26; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_TRANSFER1_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original Context1 transfer corpus");
        auto base = root + QString("/%1/").arg(mode);
        Frame frame((base + "capture.gpa_frame").toStdWString());
        const Id event = transfer(frame);
        const auto type = frame.entry(event).type;
        const Id resource = type == 0x255 ? updateSourceLayout(frame, event).destination
                                          : readCopyCommand(type, frame.payload(event)).destination;
        const bool buffer = frame.resource(resource).type == 0x83;
        auto expected = bytes(base + "native/expected.bin"), rgba = bytes(base + "native/expected.rgba");
        QCOMPARE(inspectCommand(frame, event)["status"], nlohmann::json("decoded"));
        const auto preflight = validateFrame(frame.path());
        QVERIFY2(preflight["errors"] == 0, preflight.dump(2).c_str());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        auto storage = [&] { return buffer ? replay.readBuffer(resource) : replay.readTexture(resource); };
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(storage(), expected);
            QCOMPARE(replay.output().rgba, rgba);
            unsigned checks = 0;
            replay.run({}, {}, [&](Id id, bool after, auto *, const auto &) {
                if (id == event && after) {
                    ++checks;
                    QCOMPARE(storage(), expected);
                }
            });
            QCOMPARE(checks, 1u);
            QCOMPARE(storage(), expected);
            QCOMPARE(replay.output().rgba, rgba);
            QCOMPARE(replay.counts.at(type == 0x255 ? "UpdateSubresource1" : "CopySubresourceRegion1"),
                     uint64_t(1));
        }
        options.disabled.insert(event);
        Replay disabled(frame, options);
        disabled.run();
        const auto control = buffer ? disabled.readBuffer(resource) : disabled.readTexture(resource);
        if (mode == 6 || mode == 20)
            QCOMPARE(control, expected);
        else
            QVERIFY(control != expected);
    }
    void malformed_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("copy") << 1;
        QTest::newRow("update") << 11;
        QTest::newRow("constant-buffer") << 15;
    }
    void malformed() {
        QFETCH(int, mode);
        auto root = qEnvironmentVariable("FLORA_TRANSFER1_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original Context1 transfer corpus");
        Frame source((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
        const Id event = transfer(source);
        const auto type = source.entry(event).type;
        const auto payload = source.payload(event);
        QTemporaryDir dir;
        // All strict prefixes, plus surplus bytes, must fail before any native call.
        for (size_t n = 0; n <= payload.size(); ++n) {
            auto path = dir.filePath(QString("prefix-%1.gpa_frame").arg(n));
            copyFrame(source, path, [&](Id id, Raw &raw) {
                if (id == event) {
                    if (n == payload.size())
                        raw.push_back(0);
                    else
                        raw.resize(n);
                }
            });
            Frame f(path.toStdWString());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateWritableCommand(f, event));
        }
        for (int bad = 0; bad < 10; ++bad) {
            auto path = dir.filePath(QString("bad-%1.gpa_frame").arg(bad));
            const Id asset = type == 0x255 ? updateSourceLayout(source, event).data : 0;
            copyFrame(source, path, [&](Id id, Raw &raw) {
                if (id == event) {
                    if (bad == 0)
                        put(raw, 0, Id(7));
                    if (bad == 1)
                        put(raw, 8, Id(99999));
                    if (bad == 2)
                        put(raw, 16, Id(99999));
                    if (bad == 3)
                        put(raw, 24, UINT32_MAX);
                    if (bad == 4)
                        raw[type == 0x255 ? 28 : 52] = 2;
                    if (bad == 5)
                        put(raw, raw.size() - 4, 3u);
                    if (bad == 6)
                        put(raw, type == 0x255 ? 41 : 65, UINT32_MAX);
                    if (bad == 7)
                        put(raw, type == 0x255 ? 53 : 40, Id(99999));
                    if (bad == 8 && type == 0x256)
                        put(raw, 48, UINT32_MAX);
                    if (bad == 9) {
                        if (mode == 15)
                            put(raw, 29, 17u); // Misaligned partial constant-buffer update.
                        else
                            put(raw, raw.size() - 4, 4u); // Unknown flag bit.
                    }
                }
                if (bad == 8 && id == asset) {
                    raw.pop_back();
                    put(raw, 0, uint32_t(raw.size() - 4));
                }
            });
            Frame f(path.toStdWString());
            QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
            ReplayOptions options;
            options.warp = true;
            Replay replay(f, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
    }
};
QTEST_GUILESS_MAIN(Transfer1Tests)
#include "Transfer1Tests.moc"
