#include "DepthStencilCapture.h"
#include "MsaaCapture.h"
#include "TextureEditCapture.h"
#include "application/Experiment.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeHistory.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
#include <set>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
void devices() {
    QTest::addColumn<uint32_t>("vendor");
    QTest::newRow("warp") << 0u;
    QTest::newRow("nvidia") << 0x10deu;
    QTest::newRow("intel") << 0x8086u;
}
ReplayOptions options(uint32_t vendor) {
    ReplayOptions result;
    result.debug = true;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    return result;
}
Capture commands() {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    auto data = statePack(320u);
    data.resize(324);
    c.add(30, 9, 1, data);
    c.add(31, 9, 1, statePack(4u, 0xff00ff00u));
    for (Id id : {10u, 11u})
        c.add(id, 5, 0x85, statePack(Id(0), Id(0), 8u, 4u, 2u, 2u, 28u, 1u, 0u, 0u, 40u, 0u, 0u, Id(30)));
    c.add(12, 5, 0x8d, statePack(Id(0), Id(0), Id(10), 28u, 5u, 1u, 1u, 1u));
    c.add(13, 5, 0x8d, statePack(Id(0), Id(0), Id(10), 28u, 5u, 0u, 0u, 1u));
    for (Id event : {100u, 110u, 120u})
        c.add(event, 7, 0x32,
              statePack(Id(0), Id(1), Id(12), uint8_t(1), event == 100 ? 0.f : 1.f, 0.f, 0.f, 1.f));
    c.add(130, 7, 0x32, statePack(Id(0), Id(1), Id(13), uint8_t(1), 0.f, 1.f, 0.f, 1.f));
    c.add(140, 7, 0x3e, statePack(Id(0), Id(1), Id(11), Id(10)));
    for (Id event : {150u, 160u})
        c.add(event, 7, 0x40,
              statePack(Id(0), Id(1), Id(10), 3u, event == 150 ? 1u : 2u, 0u, 0u, Id(11), 3u, uint8_t(1), 0u,
                        0u, 0u, 1u, 1u, 1u));
    for (Id event : {170u, 180u})
        c.add(event, 7, 0x247,
              statePack(Id(0), Id(1), Id(10), 3u, uint8_t(1), 1u, 0u, 0u, 2u, 1u, 1u, Id(31), 4u, 4u));
    c.add(190, 7, 0x3e, statePack(Id(0), Id(1), Id(10), Id(11)));
    for (Id event : {191u, 195u})
        c.add(event, 7, 0x257,
              statePack(Id(0), Id(1), Id(12), uint8_t(1), 0.f, 0.f, 1.f, 1.f, 1u, uint8_t(1),
                        event == 191 ? 0 : 1, 0, event == 191 ? 1 : 2, 1));
    return c;
}
const Json &row(const Json &result, Id event) {
    for (const auto &record : result.at("records"))
        if (record.at("event") == event)
            return record;
    throw std::runtime_error("Missing history event");
}
std::string deviceErrors(const Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Get validation queue");
    std::string errors;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Get validation message size");
        std::vector<uint8_t> storage(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
        check(queue->GetMessage(i, message, &size), "Get validation message");
        if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
            message->Severity == D3D11_MESSAGE_SEVERITY_ERROR)
            errors += std::string(message->pDescription) + "\n";
    }
    return errors;
}
} // namespace
class NativeHistoryTests final : public QObject {
    Q_OBJECT
  private slots:
    void resourceCommands_data() { devices(); }
    void resourceCommands() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        const auto path = directory.filePath("frame.gpa_frame");
        commands().save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        NativeHistorySelection s;
        s.resource = 10;
        s.mip = s.layer = s.x = 1;
        const auto result = collectNativeEventHistory(replay, s);
        QCOMPARE(result.at("records").size(), size_t(8));
        QCOMPARE(result.at("point_readback_count"), result.at("readback_count"));
        QCOMPARE(result.at("readback_bytes"), Json(result.at("readback_count").get<uint64_t>() * 4));
        QCOMPARE(row(result, 100).at("before").at("available"), Json(false));
        QCOMPARE(row(result, 100).at("after").at("bytes"), Json("000000ff"));
        QCOMPARE(row(result, 100).at("after").at("decoded_color").at("rgba"), Json::array({0., 0., 0., 1.}));
        QCOMPARE(row(result, 100).at("after").at("value_format_source"), Json("command_view"));
        QCOMPARE(row(result, 110).at("pixel_storage_changed"), Json(true));
        QCOMPARE(row(result, 120).at("pixel_storage_changed"), Json(false));
        QCOMPARE(row(result, 150).at("pixel_storage_changed"), Json(false));
        QCOMPARE(row(result, 170).at("after").at("bytes"), Json("00ff00ff"));
        QCOMPARE(row(result, 170).at("write_executed"), Json(true));
        QCOMPARE(row(result, 170).at("after").at("value_format_source"), Json("resource_format"));
        QCOMPARE(row(result, 180).at("pixel_storage_changed"), Json(false));
        QCOMPARE(row(result, 180).at("write_executed"), Json(true));
        QCOMPARE(row(result, 195).at("after").at("bytes"), Json("0000ffff"));
        Replay clean(frame, options(vendor));
        clean.run();
        QCOMPARE(replay.readTexture(10), clean.readTexture(10));
        QCOMPARE(replay.readTexture(11), clean.readTexture(11));
        QCOMPARE(replay.counts, clean.counts);
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        for (const auto &record : result.at("records")) {
            QCOMPARE(record.at("record_kind"), Json("event_snapshot"));
            QVERIFY(record.at("fragment").is_null() && record.at("shader_output").is_null());
            QVERIFY(record.at("failure_reasons").is_null());
        }
        auto edited = options(vendor);
        edited.disabled.insert(170);
        Replay disabled(frame, edited);
        const auto changed = collectNativeEventHistory(disabled, s);
        QCOMPARE(row(changed, 170).at("write_executed"), Json(false));
        QCOMPARE(row(changed, 170).at("pixel_storage_changed"), Json(false));
        QCOMPARE(row(changed, 180).at("pixel_storage_changed"), Json(true));
    }
    void drawSnapshots_data() { devices(); }
    void drawSnapshots() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        const auto path = directory.filePath("frame.gpa_frame");
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        NativeHistorySelection s;
        s.resource = 20;
        const auto result = collectNativeEventHistory(replay, s);
        QCOMPARE(result.at("records").size(), size_t(3));
        QCOMPARE(row(result, 1000).at("after").at("bytes"), Json("ff0000ff"));
        QCOMPARE(row(result, 2000).at("pixel_storage_changed"), Json(false));
        QCOMPARE(row(result, 2000).at("footprint").at("coverage"), Json("potential"));
        QVERIFY(row(result, 2000).at("write_executed").is_null());
        Replay clean(frame, options(vendor));
        clean.run();
        QCOMPARE(replay.readTexture(20), clean.readTexture(20));
        QCOMPARE(replay.readTexture(720), clean.readTexture(720));
        QCOMPARE(replay.counts, clean.counts);
        s.resource = 720;
        Replay depth(frame, options(vendor));
        const auto history = collectNativeEventHistory(depth, s);
        QCOMPARE(history.at("records").size(), size_t(3));
        QCOMPARE(history.at("point_readback_count"), Json(0));
        QVERIFY(row(history, 810).at("after").at("available").get<bool>());
        QCOMPARE(row(history, 810).at("after").at("stencil"), Json(0x53));
        QVERIFY(std::abs(row(history, 810).at("after").at("depth").get<double>() - .5) < 1e-7);
    }
    void multisampleAndResolve_data() { devices(); }
    void multisampleAndResolve() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        const auto path = directory.filePath("frame.gpa_frame");
        msaaOutputCapture().save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        NativeHistorySelection s;
        s.resource = 20;
        s.layer = 1;
        s.sample = 2;
        s.x = 3;
        s.y = 2;
        const auto result = collectNativeEventHistory(replay, s);
        QCOMPARE(result.at("records").size(), size_t(1));
        QCOMPARE(result.at("point_readback_count"), Json(0));
        QCOMPARE(row(result, 110).at("after").at("bytes"), Json("20000000030000010000000001000000"));
        QCOMPARE(row(result, 110).at("after").at("decoded_color").at("numeric_type"), Json("uint"));
        QCOMPARE(row(result, 110).at("after").at("decoded_color").at("rgba"),
                 Json::array({32, 16777219, 0, 1}));
        Replay clean(frame, options(vendor));
        clean.run();
        QCOMPARE(replay.readMsaa(20, 2, 3).bytes, clean.readMsaa(20, 2, 3).bytes);
        QCOMPARE(replay.counts, clean.counts);
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        const auto resolvedPath = directory.filePath("resolved.gpa_frame");
        resolvedMsaaOutputCapture(false).save(resolvedPath);
        Frame resolved(resolvedPath.toStdWString());
        Replay resolve(resolved, options(vendor));
        s.resource = 70;
        s.sample = 0;
        const auto history = collectNativeEventHistory(resolve, s);
        QCOMPARE(history.at("records").size(), size_t(1));
        QCOMPARE(row(history, 121).at("footprint").at("operation"), Json("resolve"));
        QCOMPARE(row(history, 121).at("after").at("bytes"), Json("1e0000ff"));
    }
    void cancellationAndLimits() {
        for (const auto &value : {Json{{"resource", 10}, {"history_scope", "fragments"}},
                                  Json{{"resource", 10}, {"history_scope", "events"}, {"max_records", 0}},
                                  Json{{"resource", 10}, {"gpa_event", 100}}})
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", value));
        QTemporaryDir directory;
        const auto path = directory.filePath("frame.gpa_frame");
        commands().save(path);
        Frame frame(path.toStdWString());
        NativeHistorySelection s;
        s.resource = 10;
        s.mip = s.layer = s.x = 1;
        Replay replay(frame, options(0));
        s.x = 4;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, collectNativeEventHistory(replay, s));
        s.x = 1;
        s.maxReadbackBytes = 3;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, collectNativeEventHistory(replay, s));
        s.maxReadbackBytes = 4;
        s.maxRecords = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, collectNativeEventHistory(replay, s));
        s.maxRecords = 100;
        int checks = 0;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 collectNativeEventHistory(replay, s, [&] { return ++checks > 8; }));
        const auto retried = collectNativeEventHistory(replay, s);
        QCOMPARE(retried.at("records").size(), size_t(8));
        Replay clean(frame, options(0));
        clean.run();
        QCOMPARE(replay.readTexture(10), clean.readTexture(10));
    }
    void mappedWrites_data() { devices(); }
    void mappedWrites() {
        QFETCH(uint32_t, vendor);
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        c.add(10, 5, 0x84, statePack(Id(0), Id(0), 4u, 1u, 1u, 28u, 2u, 8u, 0x10000u, 0u, Id(30)));
        c.add(30, 9, 1, statePack(16u, 1u, 2u, 3u, 4u));
        c.add(31, 9, 1, statePack(16u, 5u, 6u, 7u, 8u));
        for (Id event : {100u, 110u}) {
            c.add(event, 7, 0x246, statePack(Id(0), Id(1), int32_t(0), Id(10), 0u, 4u, 0u, Id(31)));
            c.add(event + 1, 7, 0x34ed, statePack(Id(0), Id(1), Id(10), 0u));
        }
        QTemporaryDir directory;
        const auto path = directory.filePath("map.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        NativeHistorySelection s;
        s.resource = 10;
        s.x = 2;
        Replay replay(frame, options(vendor));
        const auto history = collectNativeEventHistory(replay, s);
        QCOMPARE(history.at("records").size(), size_t(2));
        QCOMPARE(row(history, 100).at("after").at("bytes"), Json("07000000"));
        QCOMPARE(row(history, 110).at("pixel_storage_changed"), Json(false));
        QCOMPARE(row(history, 110).at("write_executed"), Json(true));
        Replay clean(frame, options(vendor));
        clean.run();
        QCOMPARE(replay.readTexture(10), clean.readTexture(10));
        QCOMPARE(replay.counts, clean.counts);
    }
    void dispatchSnapshots_data() { devices(); }
    void dispatchSnapshots() {
        try {
            QFETCH(uint32_t, vendor);
            Capture c;
            c.add(1, 5, 0x127, std::vector<uint8_t>(24));
            c.add(10, 5, 0x85, statePack(Id(0), Id(0), 4u, 2u, 1u, 1u, 42u, 1u, 0u, 0u, 128u, 0u, 0u, Id(0)));
            c.add(11, 5, 0x8f, statePack(Id(0), Id(0), Id(10), 42u, 4u, 0u, 0u, 0u));
            auto code =
                compileClassProgram("RWTexture2D<uint> target:register(u0); [numthreads(4,2,1)] "
                                    "void main(uint3 t:SV_DispatchThreadID){target[t.xy]=100+t.x+10*t.y;}",
                                    "cs_5_0");
            c.add(20, 5, 0x93, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(21)));
            auto data = statePack(uint64_t(code.size()));
            data.insert(data.end(), code.begin(), code.end());
            append(data, Id(0));
            c.add(21, 9, 0x81, data);
            c.add(80, 7, 0x33, statePack(Id(0), Id(1), Id(11), uint8_t(1), 0u, 0u, 0u, 0u));
            State state{};
            state.stages[5].shader = 20;
            state.csCount = 1;
            state.csUav[0] = 11;
            c.add(90, 3, 3, snapshot(state));
            for (Id event : {100u, 110u})
                c.add(event, 7, 0x35, statePack(Id(90), Id(0), Id(1), 1u, 1u, 1u));
            QTemporaryDir directory;
            const auto path = directory.filePath("dispatch.gpa_frame");
            c.save(path);
            Frame frame(path.toStdWString());
            NativeHistorySelection s;
            s.resource = 10;
            s.x = 2;
            s.y = 1;
            auto o = options(vendor);
            o.suppressDraws = true; // Graphics suppression must not disable dispatches.
            Replay replay(frame, o);
            const auto history = collectNativeEventHistory(replay, s);
            QCOMPARE(history.at("records").size(), size_t(3));
            QCOMPARE(row(history, 100).at("after").at("bytes"), Json("70000000"));
            QCOMPARE(row(history, 100).at("enabled"), Json(true));
            QCOMPARE(row(history, 100).at("footprint").at("outputs").at(0).at("kind"), Json("uav"));
            QCOMPARE(row(history, 110).at("pixel_storage_changed"), Json(false));
            Replay clean(frame, o);
            clean.run();
            QCOMPARE(replay.readTexture(10), clean.readTexture(10));
            QCOMPARE(replay.counts, clean.counts);
            QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void predicationAndCounters_data() { devices(); }
    void predicationAndCounters() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        for (uint32_t value : {0u, 1u}) {
            const auto path = directory.filePath(QString("predicate-%1.gpa_frame").arg(value));
            predicateCapture(true, value).save(path);
            Frame frame(path.toStdWString());
            NativeHistorySelection s;
            s.resource = 20;
            Replay replay(frame, options(vendor));
            const auto history = collectNativeEventHistory(replay, s);
            QCOMPARE(history.at("records").size(), size_t(4));
            QCOMPARE(row(history, 2000).at("after").at("bytes"), Json(value ? "ff0000ff" : "00ff00ff"));
            QVERIFY(row(history, 2000).at("write_executed").is_null());
            Replay clean(frame, options(vendor));
            clean.run();
            QCOMPARE(replay.readTexture(20), clean.readTexture(20));
            QCOMPARE(replay.readBuffer(7), clean.readBuffer(7));
            QCOMPARE(replay.readCounter(12), clean.readCounter(12));
            const auto a = replay.readPredicateResult(600), b = clean.readPredicateResult(600);
            QCOMPARE(a.value, b.value);
            QCOMPARE(a.bound, b.bound);
            QCOMPARE(a.predicateValue, b.predicateValue);
            QCOMPARE(replay.counts, clean.counts);
            QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        }
    }
    void experimentBoundaries_data() { devices(); }
    void experimentBoundaries() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        const auto path = directory.filePath("experiment.gpa_frame");
        textureEditCapture(28, 1, true).save(path);
        Frame frame(path.toStdWString());
        Experiment experiment(frame);
        TexturePatch patch;
        patch.mip = patch.layer = 1;
        patch.bytes.resize(16);
        for (size_t i = 0; i < patch.bytes.size(); i += 4)
            put(patch.bytes, i, 0x12345678u);
        experiment.setTexturePatch(frame, 100, 20, patch, true);
        auto o = options(vendor);
        experiment.apply(frame, o);
        NativeHistorySelection s;
        s.resource = 20;
        s.mip = s.layer = 1;
        Replay replay(frame, o);
        const auto history = collectNativeEventHistory(replay, s);
        QCOMPARE(history.at("records").size(), size_t(2));
        QCOMPARE(row(history, 100).at("before").at("bytes"), Json("78563412"));
        QCOMPARE(row(history, 200).at("before").at("bytes"), Json("78563412"));
        Replay clean(frame, o);
        clean.run();
        QCOMPARE(replay.readTexture(20), clean.readTexture(20));
        o.suppressDraws = true;
        Replay suppressed(frame, o);
        const auto disabled = collectNativeEventHistory(suppressed, s);
        QCOMPARE(row(disabled, 100).at("enabled"), Json(false));
        QCOMPARE(row(disabled, 100).at("write_executed"), Json(false));
    }
    void discardedStorage_data() { devices(); }
    void discardedStorage() {
        try {
            QFETCH(uint32_t, vendor);
            auto c = commands();
            c.add(200, 7, 0x3554, statePack(Id(0), Id(1), Id(13)));
            c.add(210, 7, 0x3554, statePack(Id(0), Id(1), Id(12)));
            c.add(220, 7, 0x32, statePack(Id(0), Id(1), Id(12), uint8_t(1), 1.f, 0.f, 0.f, 1.f));
            // Define the other discarded subresource before comparing complete storage.
            // Debug devices deliberately fill discarded bytes with nondeterministic patterns.
            c.add(230, 7, 0x32, statePack(Id(0), Id(1), Id(13), uint8_t(1), 0.f, 1.f, 0.f, 1.f));
            QTemporaryDir directory;
            const auto path = directory.filePath("discard.gpa_frame");
            c.save(path);
            Frame frame(path.toStdWString());
            NativeHistorySelection s;
            s.resource = 10;
            s.mip = s.layer = s.x = 1;
            Replay replay(frame, options(vendor));
            const auto history = collectNativeEventHistory(replay, s);
            QCOMPARE(history.at("records").size(), size_t(10));
            QCOMPARE(row(history, 210).at("after").at("available"), Json(false));
            QCOMPARE(row(history, 210).at("after").at("defined"), Json(false));
            QVERIFY(row(history, 210).at("pixel_storage_changed").is_null());
            QCOMPARE(row(history, 220).at("after").at("bytes"), Json("ff0000ff"));
            Replay clean(frame, options(vendor));
            clean.run();
            QCOMPARE(replay.readTexture(10), clean.readTexture(10));
            QCOMPARE(replay.counts, clean.counts);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void mipGenerationAndRectangles_data() { devices(); }
    void mipGenerationAndRectangles() {
        QFETCH(uint32_t, vendor);
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        c.add(10, 5, 0x85, statePack(Id(0), Id(0), 4u, 4u, 3u, 1u, 28u, 1u, 0u, 0u, 40u, 0u, 1u, Id(30)));
        auto initial = statePack(84u);
        initial.resize(88);
        c.add(30, 9, 1, initial);
        c.add(11, 5, 0x8d, statePack(Id(0), Id(0), Id(10), 28u, 4u, 0u, 0u, 0u));
        c.add(12, 5, 0x8c, statePack(Id(0), Id(0), Id(10), 28u, 4u, 0u, 3u, 0u, 0u));
        c.add(100, 7, 0x32, statePack(Id(0), Id(1), Id(11), uint8_t(1), 1.f, 0.f, 0.f, 1.f));
        c.add(110, 7, 0x245, statePack(Id(0), Id(1), Id(12)));
        c.add(120, 7, 0x3563, statePack(Id(0), Id(1), Id(11), 1u, uint8_t(1), 0, 0, 1, 1));
        c.add(130, 7, 0x3563, statePack(Id(0), Id(1), Id(11), 1u, uint8_t(1), 2, 2, 3, 3));
        c.add(140, 7, 0x32, statePack(Id(0), Id(1), Id(11), uint8_t(1), 0.f, 1.f, 0.f, 1.f));
        QTemporaryDir directory;
        const auto path = directory.filePath("mips.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        NativeHistorySelection s;
        s.resource = 10;
        s.mip = 1;
        Replay replay(frame, options(vendor));
        const auto generated = collectNativeEventHistory(replay, s);
        QCOMPARE(generated.at("records").size(), size_t(1));
        QCOMPARE(row(generated, 110).at("footprint").at("operation"), Json("generate_mips"));
        QCOMPARE(row(generated, 110).at("before").at("bytes"), Json("00000000"));
        QCOMPARE(row(generated, 110).at("after").at("bytes"), Json("ff0000ff"));
        s.mip = 0;
        s.x = s.y = 2;
        Replay rectangles(frame, options(vendor));
        const auto history = collectNativeEventHistory(rectangles, s);
        QCOMPARE(history.at("records").size(), size_t(3));
        QCOMPARE(row(history, 130).at("after").at("defined"), Json(false));
        QCOMPARE(row(history, 140).at("after").at("bytes"), Json("00ff00ff"));
        Replay clean(frame, options(vendor));
        clean.run();
        QCOMPARE(replay.readTexture(10), clean.readTexture(10));
        QCOMPARE(rectangles.readTexture(10), clean.readTexture(10));
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        QVERIFY2(deviceErrors(rectangles).empty(), deviceErrors(rectangles).c_str());
    }
    void narrowStorageFormats_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("dimension");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t dimension : {2u, 3u, 4u})
                for (uint32_t format : {3u, 10u, 27u, 41u, 49u, 61u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(dimension).arg(format)))
                        << vendor << format << dimension;
    }
    void narrowStorageFormats() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, dimension);
        Resource resource;
        resource.id = 10;
        resource.type = uint16_t(dimension + 0x82);
        if (dimension == 2)
            resource.desc = {7, 3, 2, format, 0, 8, 0, 0};
        else if (dimension == 3)
            resource.desc = {7, 5, 3, 2, format, 1, 0, 0, 8, 0, 0};
        else
            resource.desc = {7, 5, 4, 3, format, 0, 8, 0, 0};
        const auto subs = textureSubresources(resource);
        std::vector<uint8_t> expected(size_t(subs.back().offset + subs.back().size));
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = uint8_t(i * 53 + 19);
        if (format == 41) {
            for (size_t i = 0; i < expected.size(); i += 4)
                put(expected, i, i % 8 ? 0x80000000u : 0x7fc01234u);
        }
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        auto descriptor = statePack(Id(0), Id(0));
        for (const auto value : resource.desc)
            append(descriptor, value);
        append(descriptor, Id(20));
        c.add(10, 5, resource.type, descriptor);
        auto data = statePack(uint32_t(expected.size()));
        data.insert(data.end(), expected.begin(), expected.end());
        c.add(20, 9, 1, data);
        QTemporaryDir directory;
        const auto path = directory.filePath("point.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        const auto bytes = pitches(1, 1, format).first;
        QCOMPARE(replay.readTexture(10), expected);
        for (const auto &sub : subs) {
            const auto x = sub.width - 1, y = sub.height - 1, z = sub.depth - 1;
            const auto offset = sub.offset + z * sub.slicePitch + y * uint64_t(sub.rowPitch) + x * bytes;
            const auto point = replay.readTexturePoint(10, x, y, z, sub.mip, sub.layer);
            QVERIFY(point);
            QCOMPARE(*point, (std::vector<uint8_t>(expected.begin() + size_t(offset),
                                                   expected.begin() + size_t(offset) + bytes)));
        }
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, replay.readTexturePoint(10, 7, 0, 0, 0, 0));
        QCOMPARE(replay.readTexture(10), expected);
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
    }
    void boundedOrdinarySnapshots_data() { devices(); }
    void boundedOrdinarySnapshots() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        const auto path = directory.filePath("bounded.gpa_frame");
        commands().save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        NativeHistorySelection s;
        s.resource = 10;
        s.mip = s.layer = s.x = 1;
        s.maxReadbackBytes = 4;
        const auto history = collectNativeEventHistory(replay, s);
        QCOMPARE(history.at("records").size(), size_t(8));
        QCOMPARE(history.at("readback_count"), Json(15));
        QCOMPARE(history.at("point_readback_count"), Json(15));
        QCOMPARE(history.at("readback_bytes"), Json(60));
        QCOMPARE(row(history, 100).at("before").at("available"), Json(false));
        QCOMPARE(row(history, 170).at("after").at("bytes"), Json("00ff00ff"));
        QCOMPARE(row(history, 180).at("write_executed"), Json(true));
        QCOMPARE(row(history, 180).at("pixel_storage_changed"), Json(false));
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
    }
    void boundedCompressedSnapshots_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<bool>("cube");
        for (const auto vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format = 70; format <= 99; ++format) {
                if (format > 84 && format < 94)
                    continue;
                for (const auto cube : {false, true}) {
                    if (cube && format != 71)
                        continue;
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(format).arg(cube)))
                        << vendor << format << cube;
                }
            }
    }
    void completeStorageBudget_data() { devices(); }
    void completeStorageBudget() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        for (const bool depth : {false, true}) {
            Capture c;
            c.add(1, 5, 0x127, std::vector<uint8_t>(24));
            const auto format = depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
            auto descriptor = statePack(Id(0), Id(0));
            append(descriptor,
                   D3D11_TEXTURE2D_DESC{7,
                                        5,
                                        1,
                                        2,
                                        format,
                                        {depth ? 1u : 4u, 0},
                                        D3D11_USAGE_DEFAULT,
                                        UINT(depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET),
                                        0,
                                        0});
            append(descriptor, Id(0));
            c.add(10, 5, 0x85, descriptor);
            auto view = statePack(Id(0), Id(0), Id(10));
            if (depth) {
                D3D11_DEPTH_STENCIL_VIEW_DESC d{};
                d.Format = format;
                d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                d.Texture2DArray = {0, 1, 1};
                append(view, d);
                c.add(12, 5, 0x8e, view);
                c.add(100, 7, 0x31, statePack(Id(0), Id(1), Id(12), 3u, .5f, uint8_t(0x53)));
            } else {
                D3D11_RENDER_TARGET_VIEW_DESC d{};
                d.Format = format;
                d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                d.Texture2DMSArray = {1, 1};
                append(view, d);
                c.add(12, 5, 0x8d, view);
                c.add(100, 7, 0x32, statePack(Id(0), Id(1), Id(12), uint8_t(1), 1.f, 0.f, 0.f, 1.f));
            }
            const auto path = directory.filePath(QString("complete-%1.gpa_frame").arg(depth));
            c.save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            NativeHistorySelection s;
            s.resource = 10;
            s.layer = 1;
            s.x = 3;
            s.y = 2;
            s.maxReadbackBytes = 4;
            try {
                collectNativeEventHistory(replay, s);
                QFAIL("A complete-storage readback must not exceed its budget");
            } catch (const std::runtime_error &error) {
                const auto message = QByteArray(error.what());
                QVERIFY(message.startsWith("Event 100 ("));
                QVERIFY(message.endsWith("Native history complete readback exceeds its storage budget"));
            }
            s.maxReadbackBytes = 280;
            const auto history = collectNativeEventHistory(replay, s);
            QCOMPARE(history.at("readback_count"), Json(1));
            QCOMPARE(history.at("readback_bytes"), Json(280));
            QCOMPARE(history.at("point_readback_count"), Json(0));
            QCOMPARE(history.at("block_readback_count"), Json(0));
            QCOMPARE(row(history, 100).at("after").at("readback_scope"), Json("complete_storage"));
            Replay clean(frame, options(vendor));
            clean.run();
            if (depth)
                QCOMPARE(replay.readTexture(10), clean.readTexture(10));
            else
                QCOMPARE(replay.readMsaa(10, 0, format).bytes, clean.readMsaa(10, 0, format).bytes);
            QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        }
    }
    void compressedPredicationAndCounters_data() { devices(); }
    void compressedPredicationAndCounters() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir directory;
        for (const uint32_t value : {0u, 1u}) {
            auto c = predicateCapture(true, value);
            const auto dispatch = c.entries.back();
            const auto dispatchBytes = std::vector<uint8_t>(
                c.bytes.begin() + dispatch.offset, c.bytes.begin() + dispatch.offset + dispatch.size);
            QCOMPARE(dispatch.id, Id(3000));
            c.entries.pop_back();
            for (const Id id : {9000u, 9001u}) {
                c.add(id, 5, 0x85,
                      statePack(Id(0), Id(0), 8u, 8u, 1u, 1u, 71u, 1u, 0u, 0u, 8u, 0u, 0u, id + 10));
                auto data = statePack(32u);
                data.resize(36, id == 9000 ? 0 : 0x5a);
                c.add(id + 10, 9, 1, data);
            }
            for (const Id event : {4000u, 4100u})
                c.add(event, 7, 0x3e, statePack(Id(0), Id(1), Id(9000), Id(9001)));
            c.add(5000, 7, 0x35, dispatchBytes);
            const auto path = directory.filePath(QString("predicated-block-%1.gpa_frame").arg(value));
            c.save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            NativeHistorySelection s;
            s.resource = 9000;
            s.x = s.y = 7;
            s.maxReadbackBytes = 8;
            s.maxRecords = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, collectNativeEventHistory(replay, s));
            s.maxRecords = 2;
            const auto history = collectNativeEventHistory(replay, s);
            QCOMPARE(history.at("block_readback_count"), Json(3));
            QCOMPARE(history.at("readback_bytes"), Json(24));
            const auto wanted = value ? "0000000000000000" : "5a5a5a5a5a5a5a5a";
            for (const Id event : {4000u, 4100u}) {
                QCOMPARE(row(history, event).at("after").at("bytes"), Json(wanted));
                QVERIFY(row(history, event).at("write_executed").is_null());
            }
            QCOMPARE(row(history, 4100).at("storage_changed"), Json(false));
            Replay clean(frame, options(vendor));
            clean.run();
            QCOMPARE(replay.readTexture(9000), clean.readTexture(9000));
            QCOMPARE(replay.readTexture(20), clean.readTexture(20));
            QCOMPARE(replay.readBuffer(7), clean.readBuffer(7));
            QCOMPARE(replay.readCounter(12), clean.readCounter(12));
            const auto a = replay.readPredicateResult(600), b = clean.readPredicateResult(600);
            QCOMPARE(a.value, b.value);
            QCOMPARE(a.bound, b.bound);
            QCOMPARE(a.predicateValue, b.predicateValue);
            QCOMPARE(replay.counts, clean.counts);
            QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        }
    }
    void boundedCompressedSnapshots() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        QFETCH(bool, cube);
        Resource resource;
        resource.id = 10;
        resource.type = 0x85;
        resource.desc = {12, cube ? 12u : 20u, cube ? 4u : 5u, cube ? 6u : 2u, format, 1, 0, 0, 8,
                         0,  cube ? 4u : 0u};
        const auto subs = textureSubresources(resource);
        const auto count = size_t(subs.back().offset + subs.back().size);
        std::vector<uint8_t> expected(count);
        for (size_t i = 0; i < count; ++i)
            expected[i] = uint8_t(i * 53 + i / 256 * 31 + 19);
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        for (const Id id : {10u, 11u, 12u}) {
            auto descriptor = statePack(Id(0), Id(0));
            for (const auto value : resource.desc)
                append(descriptor, value);
            append(descriptor, id + 20);
            c.add(id, 5, 0x85, descriptor);
            auto data = statePack(uint32_t(count));
            if (id != 11)
                data.resize(4 + count);
            else
                data.insert(data.end(), expected.begin(), expected.end());
            c.add(id + 20, 9, 1, data);
        }
        // A copy away from the selection materializes its initial contents before
        // the first target write, without introducing a target history record.
        c.add(90, 7, 0x3e, statePack(Id(0), Id(1), Id(12), Id(10)));
        for (const Id event : {100u, 110u})
            c.add(event, 7, 0x3e, statePack(Id(0), Id(1), Id(10), Id(11)));
        QTemporaryDir directory;
        const auto path = directory.filePath("blocks.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        const auto blockBytes = pitches(1, 1, format).first;
        Json observations = Json::array();
        for (const auto &sub : subs) {
            const std::set<std::pair<uint32_t, uint32_t>> points{
                {0, 0}, {sub.width / 2, sub.height / 2}, {sub.width - 1, sub.height - 1}};
            for (const auto &[x, y] : points) {
                NativeHistorySelection s;
                s.resource = 10;
                s.x = x;
                s.y = y;
                s.mip = sub.mip;
                s.layer = sub.layer;
                s.maxReadbackBytes = blockBytes;
                const auto history = collectNativeEventHistory(replay, s);
                const auto offset =
                    sub.offset + uint64_t(y / 4) * sub.rowPitch + uint64_t(x / 4) * blockBytes;
                const auto wanted =
                    QByteArray(reinterpret_cast<const char *>(expected.data() + offset), blockBytes)
                        .toHex()
                        .toStdString();
                QCOMPARE(history.at("records").size(), size_t(2));
                QCOMPARE(history.at("readback_count"), Json(4));
                QCOMPARE(history.at("point_readback_count"), Json(0));
                QCOMPARE(history.at("block_readback_count"), Json(4));
                QCOMPARE(history.at("readback_bytes"), Json(4 * blockBytes));
                QCOMPARE(row(history, 100).at("before").at("bytes"), Json(std::string(blockBytes * 2, '0')));
                for (const Id event : {100u, 110u}) {
                    const auto &record = row(history, event);
                    QVERIFY2(record.at("after").at("bytes") == wanted,
                             (history.at("selection").dump() + " expected=" + wanted + " actual=" +
                              record.at("after").at("bytes").dump() + " errors=" + deviceErrors(replay))
                                 .c_str());
                    QCOMPARE(record.at("after").at("readback_scope"), Json("single_compressed_block"));
                    QCOMPARE(record.at("after").at("exact_pixel_storage"), Json(false));
                    QVERIFY(record.at("pixel_storage_changed").is_null());
                    QVERIFY(record.at("write_executed").is_null());
                    QVERIFY(record.at("fragment").is_null());
                }
                QCOMPARE(row(history, 100).at("storage_changed"), Json(true));
                QCOMPARE(row(history, 110).at("storage_changed"), Json(false));
                QCOMPARE(replay.readTexture(10), expected);
                QCOMPARE(replay.readTexture(11), expected);
                observations.push_back(history);
            }
        }
        NativeHistorySelection invalid;
        invalid.resource = 10;
        invalid.maxReadbackBytes = blockBytes - 1;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, collectNativeEventHistory(replay, invalid));
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        const auto evidence = qEnvironmentVariable("FLORA_NATIVE_HISTORY_BLOCK_EVIDENCE");
        if (!evidence.isEmpty()) {
            const auto folder = evidence + "/" + QTest::currentDataTag();
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(QFile::copy(path, folder + "/source.gpa_frame"));
            QFile output(folder + "/observations.json");
            QVERIFY(output.open(QIODevice::WriteOnly));
            const auto text =
                Json{{"vendor", vendor}, {"format", format}, {"cube", cube}, {"observations", observations}}
                    .dump(2) +
                "\n";
            QCOMPARE(output.write(text.data(), qint64(text.size())), qint64(text.size()));
        }
    }
    void groupedStorageRetainsBlockScope_data() { devices(); }
    void groupedStorageRetainsBlockScope() {
        QFETCH(uint32_t, vendor);
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        c.add(10, 5, 0x85, statePack(Id(0), Id(0), 8u, 8u, 1u, 1u, 71u, 1u, 0u, 0u, 8u, 0u, 0u, Id(30)));
        c.add(11, 5, 0x85, statePack(Id(0), Id(0), 8u, 8u, 1u, 1u, 71u, 1u, 0u, 0u, 8u, 0u, 0u, Id(31)));
        auto blank = statePack(32u);
        blank.resize(36);
        c.add(30, 9, 1, blank);
        for (size_t i = 4; i < blank.size(); ++i)
            blank[i] = uint8_t(i - 4);
        c.add(31, 9, 1, blank);
        for (Id event : {100u, 110u})
            c.add(event, 7, 0x3e, statePack(Id(0), Id(1), Id(10), Id(11)));
        QTemporaryDir directory;
        const auto path = directory.filePath("compressed.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor));
        NativeHistorySelection s;
        s.resource = 10;
        s.x = s.y = 7;
        const auto history = collectNativeEventHistory(replay, s);
        QCOMPARE(history.at("records").size(), size_t(2));
        QCOMPARE(history.at("point_readback_count"), Json(0));
        QCOMPARE(row(history, 100).at("after").at("bytes"), Json("18191a1b1c1d1e1f"));
        QCOMPARE(row(history, 100).at("after").at("encoding"), Json("compressed_4x4_block"));
        QVERIFY(row(history, 110).at("pixel_storage_changed").is_null());
        QCOMPARE(row(history, 110).at("storage_changed"), Json(false));
        QCOMPARE(row(history, 100).at("after").at("decoding_source"), Json("native_gpu_load"));
        QCOMPARE(replay.readTexture(10), (std::vector<uint8_t>(blank.begin() + 4, blank.end())));
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
        const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
        Json selection{{"resource", 10},       {"x", 7},        {"y", 7}, {"history_scope", "events"},
                       {"debug_device", true}, {"max_bytes", 8}};
        if (vendor)
            selection["gpu_vendor"] = vendor;
        else
            selection["warp"] = true;
        const auto job =
            prepareNativeAnalysisJob(path, "history", directory.filePath("worker"), worker, selection);
        QFile file(runNativeAnalysisJob(job, worker, 30));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = Json::parse(file.readAll().toStdString());
        validateNativeAnalysisResult(job, result);
        QCOMPARE(result.at("data"), history);
        const auto &adapter = row(result.at("data"), 100).at("after").at("decoding_adapter");
        QCOMPARE(adapter.at("luid_low"), result.at("context").at("adapter").at("luid_low"));
        QCOMPARE(adapter.at("luid_high"), result.at("context").at("adapter").at("luid_high"));
        for (const auto &module : result.at("execution").at("loaded_modules")) {
            const auto modulePath = QString::fromStdString(module.get<std::string>()).toLower();
            QVERIFY(!modulePath.contains("renderdoc") && !modulePath.contains("intelswtools\\gpa\\"));
        }
    }
    void typelessInterpretations_data() { devices(); }
    void typelessInterpretations() {
        QFETCH(uint32_t, vendor);
        auto c = commands();
        for (const auto &entry : c.entries)
            if (entry.id == 10 || entry.id == 11)
                put(c.bytes, size_t(entry.offset) + 32, 27u);
        c.add(14, 5, 0x8d, statePack(Id(0), Id(0), Id(10), 29u, 5u, 1u, 1u, 1u));
        c.add(196, 7, 0x32, statePack(Id(0), Id(1), Id(14), uint8_t(1), .5f, .5f, .5f, .5f));
        QTemporaryDir directory;
        const auto path = directory.filePath("typeless.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        NativeHistorySelection s;
        s.resource = 10;
        s.mip = s.layer = s.x = 1;
        Replay replay(frame, options(vendor));
        const auto history = collectNativeEventHistory(replay, s);
        QCOMPARE(history.at("schema"), Json(2));
        QCOMPARE(row(history, 170).at("after").at("bytes"), Json("00ff00ff"));
        QCOMPARE(row(history, 170).at("after").at("decoding_status"), Json("unknown"));
        QVERIFY(row(history, 170).at("after").at("decoded_color").is_null());
        QCOMPARE(row(history, 195).at("after").at("value_format"), Json(28));
        const auto &srgb = row(history, 196).at("after");
        QCOMPARE(srgb.at("value_format"), Json(29));
        QCOMPARE(srgb.at("value_format_source"), Json("command_view"));
        QCOMPARE(srgb.at("decoded_color").at("srgb_to_linear"), Json(true));
        QVERIFY(std::abs(srgb.at("decoded_color").at("rgba")[0].get<double>() - .5) < .005);
        s.typedFormat = 30;
        Replay explicitView(frame, options(vendor));
        const auto explicitHistory = collectNativeEventHistory(explicitView, s);
        const auto &update = row(explicitHistory, 170).at("after");
        QCOMPARE(update.at("value_format_source"), Json("explicit_selection"));
        QCOMPARE(update.at("decoded_color").at("rgba"), Json::array({0, 255, 0, 255}));
        QCOMPARE(explicitView.readTexture(10), replay.readTexture(10));
        s.typedFormat = 41;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, collectNativeEventHistory(explicitView, s));
        QVERIFY2(deviceErrors(replay).empty(), deviceErrors(replay).c_str());
    }
    void workerRequest() {
        QTemporaryDir directory;
        const auto path = directory.filePath("frame.gpa_frame");
        commands().save(path);
        const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
        const auto job = prepareNativeAnalysisJob(path, "history", directory.filePath("out"), worker,
                                                  {{"warp", true},
                                                   {"resource", 10},
                                                   {"mip", 1},
                                                   {"layer", 1},
                                                   {"x", 1},
                                                   {"history_scope", "events"}});
        QFile file(runNativeAnalysisJob(job, worker, 30));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = Json::parse(file.readAll().toStdString());
        validateNativeAnalysisResult(job, result);
        QCOMPARE(result.at("data").at("records").size(), size_t(8));
        QCOMPARE(result.at("data").at("fragment_history_available"), Json(false));
        for (const auto &module : result.at("execution").at("loaded_modules")) {
            const auto modulePath = QString::fromStdString(module.get<std::string>()).toLower();
            QVERIFY(!modulePath.contains("renderdoc") && !modulePath.contains("intelswtools\\gpa\\"));
        }
    }
};
QTEST_GUILESS_MAIN(NativeHistoryTests)
#include "NativeHistoryTests.moc"
