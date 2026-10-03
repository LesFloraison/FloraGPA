#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/ResourceLod.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
QString fixture(int mode) {
    const auto root = qEnvironmentVariable(mode < 6 ? "FLORA_MINLOD_CAPTURES" : "FLORA_MINLOD_BOUNDARIES");
    return root.isEmpty() ? QString{} : root + QString("/%1/").arg(mode);
}
std::vector<uint8_t> bytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing LOD oracle");
    auto a = f.readAll();
    return {reinterpret_cast<const uint8_t *>(a.data()),
            reinterpret_cast<const uint8_t *>(a.data()) + a.size()};
}
void copy(const Frame &frame, const QString &path, Id edited, const std::vector<uint8_t> &replacement) {
    Capture out;
    for (const auto &[id, e] : frame.entries()) {
        auto raw = frame.payload(id);
        out.add(id, e.category, e.type,
                id == edited ? replacement : std::vector<uint8_t>(raw.begin(), raw.end()));
    }
    out.save(path);
}
} // namespace
class ResourceLodTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 12; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = fixture(mode);
        if (root.isEmpty())
            QSKIP("Set original resource LOD corpora");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        const auto validation = validateFrame(frame.path());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        if (mode == 6) {
            QVERIFY(!audit.issues.empty());
            QVERIFY(validation["errors"] != 0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            return;
        }
        QVERIFY2(audit.issues.empty(), audit.issues.empty() ? "" : audit.issues.front().reason.c_str());
        QVERIFY2(validation["errors"] == 0, validation.dump(2).c_str());
        if (mode == 5 || mode == 7 || mode == 8) {
            QCOMPARE(audit.initial.size(), size_t(1));
            QCOMPARE(audit.initial.begin()->second.value, mode == 8 ? 0.0f : 1.0f);
        } else
            QVERIFY(audit.initial.empty());
        for (const auto &[id, record] : audit.records)
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
        const auto expected = bytes(root + "native/expected.rgba");
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(replay.output().rgba, expected);
            unsigned checks = 0;
            replay.run({}, {}, [&](Id id, bool after, auto *context, const auto &objects) {
                auto it = audit.records.find(id);
                if (!after || it == audit.records.end() || !it->second.setter)
                    return;
                Com<ID3D11Resource> resource;
                check(objects.at(it->second.resource).As(&resource), "LOD observation resource");
                QCOMPARE(context->GetResourceMinLOD(resource.Get()), it->second.value);
                ++checks;
            });
            QCOMPARE(checks, unsigned(std::count_if(audit.records.begin(), audit.records.end(),
                                                    [](const auto &v) { return v.second.setter; })));
            QCOMPARE(replay.output().rgba, expected);
        }
        if (mode == 8 || mode == 10) {
            for (const auto &[id, record] : audit.records)
                if (record.setter && record.value == 1)
                    options.disabled.insert(id);
            QVERIFY(!options.disabled.empty());
            Replay disabled(frame, options);
            disabled.run();
            const auto image = disabled.output().rgba;
            QVERIFY(image != expected);
            for (size_t p = 0; p < image.size(); p += 4) {
                QCOMPARE(image[p], uint8_t(255));
                QCOMPARE(image[p + 1], uint8_t(0));
                QCOMPARE(image[p + 2], uint8_t(0));
                QCOMPARE(image[p + 3], uint8_t(255));
            }
        }
        if (mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 7 || mode == 10 || mode == 11)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.readTexture(*audit.clamped.begin()));
        else {
            std::vector<uint8_t> expectedStorage;
            const std::array<std::array<uint8_t, 4>, 4> colors{
                {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}}};
            for (unsigned mip = 0; mip < 4; ++mip)
                for (unsigned pixel = 0; pixel < (8u >> mip) * (8u >> mip); ++pixel)
                    expectedStorage.insert(expectedStorage.end(), colors[mip].begin(), colors[mip].end());
            QCOMPARE(replay.readTexture(*audit.clamped.begin()), expectedStorage);
        }
        if (mode == 1) {
            for (const auto &[id, record] : audit.records)
                if (record.setter)
                    options.disabled.insert(id);
            Replay unknown(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.run());
        }
    }
    void opaquePrefix() {
        const auto root = fixture(7);
        if (root.isEmpty())
            QSKIP("Set original resource LOD boundaries");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        QVERIFY(!auditResourceLod(frame).initial.empty());
        Capture changed;
        bool replaced = false;
        for (const auto &[id, e] : frame.entries()) {
            const auto raw = frame.payload(id);
            auto type = e.type;
            if (!replaced && e.category == 7) {
                type = 0x7ffe;
                replaced = true;
            }
            changed.add(id, e.category, type, {raw.begin(), raw.end()});
        }
        QVERIFY(replaced);
        QTemporaryDir directory;
        const auto path = directory.path() + "/opaque.gpa_frame";
        changed.save(path);
        Frame opaque(path.toStdWString());
        const auto audit = auditResourceLod(opaque);
        QVERIFY(audit.initial.empty());
        QVERIFY(!audit.issues.empty());
    }
    void malformed() {
        const auto root = fixture(1);
        if (root.isEmpty())
            QSKIP("Set original resource LOD corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        QTemporaryDir temp;
        for (const auto &[id, record] : audit.records) {
            const auto payload = frame.payload(id);
            const auto type = frame.entry(id).type;
            for (size_t length = 0; length < payload.size(); ++length)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResourceLod(type, payload.first(length)));
            auto extra = std::vector<uint8_t>(payload.begin(), payload.end());
            extra.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResourceLod(type, extra));
            for (int bad = 0; bad < 9; ++bad) {
                auto edited = std::vector<uint8_t>(payload.begin(), payload.end());
                const auto value = record.setter ? 24u : 16u;
                const auto resource = record.setter ? 16u : 20u;
                if (bad == 0)
                    put(edited, 0, Id(1));
                if (bad == 1)
                    put(edited, 8, Id(0));
                if (bad == 2)
                    put(edited, resource, Id(0));
                if (bad == 3)
                    put(edited, resource, record.context);
                if (bad == 4)
                    put(edited, value, 0x7fc00000u);
                if (bad == 5)
                    put(edited, value, 0x7f800000u);
                if (bad == 6)
                    put(edited, value, -1.0f);
                if (bad == 7)
                    put(edited, value, 1000.0f);
                if (bad == 8)
                    edited.pop_back();
                const auto path = temp.path() + QString("/%1-%2.gpa_frame").arg(id).arg(bad);
                copy(frame, path, id, edited);
                Frame changed(path.toStdWString());
                QVERIFY(validateFrame(changed.path())["errors"] != 0);
                ReplayOptions options;
                options.warp = true;
                Replay replay(changed, options);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            }
        }
    }
    void malformedResourceKeepsDiagnostics() {
        const auto root = fixture(1);
        if (root.isEmpty())
            QSKIP("Set original resource LOD corpus");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto audit = auditResourceLod(frame);
        const auto resource = *audit.clamped.begin();
        const auto raw = frame.payload(resource);
        QTemporaryDir directory;
        const auto path = directory.path() + "/bad-resource.gpa_frame";
        copy(frame, path, resource, {raw.begin(), raw.begin() + 17});
        const auto validation = validateFrame(path.toStdWString());
        QVERIFY(validation["errors"] != 0);
        QVERIFY2(validation["completed"] == true, validation.dump(2).c_str());
        QCOMPARE(validation["scanned_records"].get<size_t>(), frame.entries().size());
    }
};
QTEST_GUILESS_MAIN(ResourceLodTests)
#include "ResourceLodTests.moc"
