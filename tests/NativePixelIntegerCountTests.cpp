#include "PixelOrdinalCapture.h"
#include "application/NativePixelGeometryReplay.h"
#include "replay/BlendState.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Capture fixture(const QString &kind, uint32_t samples, uint32_t targets, uint32_t mode, uint32_t alpha,
                uint32_t phase, uint32_t count) {
    auto c = blendCapture(targets, false, true, samples);
    const uint32_t format = kind.endsWith("uint") ? 3 : 4;
    for (uint32_t target = 0; target < targets; ++target) {
        const Id id = 1000 + 4 * target;
        std::erase_if(c.entries, [&](const auto &e) {
            return e.id == id || e.id == id + 1 || e.id == id + 2 || e.id == 70 + target;
        });
        c.add(id, 5, 0x85,
              statePack(Id(0), Id(0), 4u, 4u, 1u, 1u, format, samples, 0u, 0u, 32u, 0u, 0u, Id(0)));
        c.add(id + 2, 5, 0x8d, statePack(Id(0), Id(0), id, format, samples == 1 ? 4u : 6u, 0u, 0u, 0u));
        c.add(70 + target, 7, 0x32, statePack(Id(0), Id(1), id + 2, uint8_t(1), 0.f, 0.f, 0.f, 0.f));
    }
    std::erase_if(c.entries,
                  [](const auto &e) { return e.id == 50 || e.id == 90 || e.id == 190 || e.id == 100; });
    c.add(50, 9, 0x87, statePack(1u, 0.f, 0.f, 4.f, 4.f, 0.f, 1.f));
    c.add(300, 5, 0x83, statePack(Id(0), Id(0), 16u, 0u, 4u, 0u, 0u, 0u, Id(301)));
    c.add(301, 9, 1, statePack(16u, alpha, phase, 0u, 0u));
    addOrdinalCode(
        c, 11,
        "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
        "vs_5_0");
    const auto type = kind.toStdString() + "4";
    std::string source = "cbuffer C:register(b0){uint4 params;}struct O{";
    for (uint32_t target = 0; target < targets; ++target)
        source += type + " c" + std::to_string(target) + ":SV_Target" + std::to_string(target) + ";";
    if (mode == 3)
        source += "uint mask:SV_Coverage;";
    source += "};O main(uint id:SV_PrimitiveID){O o;uint tag=id+params.y;";
    if (mode == 1)
        source += "if(tag&1)discard;";
    if (mode == 3)
        source += "o.mask=(tag&1)?10:5;";
    for (uint32_t target = 0; target < targets; ++target)
        source += "o.c" + std::to_string(target) + "=" + type + "(7,11,13,params.x);";
    source += "return o;}";
    addOrdinalCode(c, 13, source, "ps_5_0");
    State state{};
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.rtCount = targets;
    state.blend = 64;
    state.rasterizer = 52;
    state.viewports = 50;
    state.stages[0].shader = 10;
    state.stages[4].shader = 12;
    state.stages[4].cb[0] = 300;
    for (uint32_t target = 0; target < targets; ++target)
        state.rtv[target] = 1002 + 4 * target;
    c.add(90, 3, 3, snapshot(state));
    c.add(190, 3, 3, snapshot(state));
    c.add(100, 7, 0x37, statePack(Id(90), Id(0), Id(1), 3 * count, 0u));
    return c;
}
ReplayOptions options(uint32_t vendor, uint32_t mode, bool before) {
    ReplayOptions result;
    result.debug = true;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    result.until = 100;
    result.before = result.prepareBeforeDraw = before;
    auto blend = defaultBlend();
    blend.AlphaToCoverageEnable = mode >= 2;
    result.blendEdits[100].descriptor = blend;
    return result;
}
std::vector<uint8_t> bytes(Replay &r, Id resource, uint32_t samples, uint32_t sample, uint32_t format) {
    return samples == 1 ? r.readTexture(resource) : r.readMsaa(resource, sample, format).bytes;
}
std::array<uint32_t, 4> pixel(Replay &r, Id resource, uint32_t samples, uint32_t sample, uint32_t format) {
    const auto data = bytes(r, resource, samples, sample, format);
    if (data.size() != 256)
        throw std::runtime_error("Integer count reference size differs");
    std::array<uint32_t, 4> value{};
    std::memcpy(value.data(), data.data() + 16 * 5, 16);
    return value;
}
void save(const Json &report, const QString &frame, bool references = true) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_INTEGER_COUNT_EVIDENCE");
    if (root.isEmpty())
        return;
    QDir().mkpath(root);
    const auto base = root + "/" + QString::fromLatin1(QTest::currentDataTag());
    QFile file(base + ".json");
    const auto text = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()) ||
        !QFile::copy(frame, base + ".gpa_frame"))
        throw std::runtime_error("Cannot retain integer count evidence");
    if (references)
        for (uint32_t phase : {0u, 1u})
            if (!QFile::copy(QFileInfo(frame).dir().filePath(QString("reference-%1.gpa_frame").arg(phase)),
                             base + QString(".reference-%1.gpa_frame").arg(phase)))
                throw std::runtime_error("Cannot retain original integer reference frame");
}
} // namespace
class NativePixelIntegerCountTests final : public QObject {
    Q_OBJECT
  private slots:
    void uav_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("slot");
        QTest::addColumn<bool>("counter");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t slot : {1u, 2u})
                for (bool counter : {false, true})
                    QTest::newRow(qPrintable(QString("uav-%1-%2-%3").arg(vendor).arg(slot).arg(counter)))
                        << vendor << slot << counter;
    }
    void uav() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, slot);
            QFETCH(bool, counter);
            auto capture = graphicsCounterCapture(counter);
            for (const auto &entry : capture.entries) {
                if (entry.id == 20)
                    put(capture.bytes, size_t(entry.offset) + 32, 3u);
                if (entry.id == 21)
                    put(capture.bytes, size_t(entry.offset) + 24, 3u);
            }
            addOrdinalCode(
                capture, 33,
                std::string(counter ? "RWStructuredBuffer<uint>" : "AppendStructuredBuffer<uint>") +
                    " target:register(u" + std::to_string(slot) + ");uint4 main():SV_Target{" +
                    (counter ? "uint i=target.IncrementCounter();target[i]=7;" : "target.Append(7);") +
                    "return uint4(7,11,13,1);}",
                "ps_5_0");
            std::erase_if(capture.entries,
                          [](const auto &entry) { return entry.id == 34 || entry.id == 190; });
            State state{};
            state.topology = 4;
            state.sampleMask = UINT32_MAX;
            state.viewports = 22;
            state.stages[0].shader = 30;
            state.stages[4].shader = 32;
            state.rtv[0] = 21;
            state.rtv[slot] = 12;
            state.omStart = slot;
            state.rtCount = slot + 1;
            capture.add(34, 3, 3, snapshot(state));
            auto setter = statePack(Id(0), Id(1), 1u, uint8_t(1), Id(21), Id(0), slot, 1u, uint8_t(1), Id(12),
                                    uint8_t(1), 0u);
            capture.add(190, 7, 0x3500, setter);
            QTemporaryDir directory;
            const auto path = directory.filePath("uav.gpa_frame");
            capture.save(path);
            Frame frame(path.toStdWString());
            auto config = options(vendor, 2, true);
            config.until = 200;
            config.blendEdits[200] = config.blendEdits[100];
            config.blendEdits.erase(100);
            auto ordinary = config;
            ordinary.before = ordinary.prepareBeforeDraw = false;
            Replay reference(frame, ordinary);
            reference.run();
            const auto expected = reference.readBuffer(10);
            const auto expectedCounter = reference.readCounter(12);
            Replay replay(frame, config);
            replay.run();
            const auto initial = replay.readBuffer(10), color = replay.readTexture(20);
            const auto initialCounter = replay.readCounter(12);
            Json report;
            if (slot == 1) {
                bool rejected = false;
                try {
                    replayNativePixelReturnCount(replay, 200, {});
                } catch (const std::runtime_error &error) {
                    QCOMPARE(std::string(error.what()),
                             std::string("Auxiliary pixel count target overlaps UAV slot 1"));
                    rejected = true;
                }
                QVERIFY(rejected);
            } else {
                report = replayNativePixelReturnCount(replay, 200, {}, [&](Replay &r, bool after) {
                    if (after) {
                        QCOMPARE(r.readBuffer(10), expected);
                        QCOMPARE(r.readCounter(12), expectedCounter);
                    }
                });
                QCOMPARE(report["return_count"]["count"],
                         Json(Reader(reference.readTexture(20)).read<uint32_t>() == 7 ? 1 : 0));
                QCOMPARE(report["count_transport"]["slot"], Json(1));
            }
            QCOMPARE(replay.readBuffer(10), initial);
            QCOMPARE(replay.readCounter(12), initialCounter);
            QCOMPARE(replay.readTexture(20), color);
            replayNativeOriginalDraw(replay, 200, [&](Replay &r, bool after) {
                if (after) {
                    QCOMPARE(r.readBuffer(10), expected);
                    QCOMPARE(r.readCounter(12), expectedCounter);
                }
            });
            QCOMPARE(replay.readBuffer(10), initial);
            QCOMPARE(replay.readCounter(12), initialCounter);
            save({{"vendor", vendor},
                  {"slot", slot},
                  {"counter", counter},
                  {"rejected", slot == 1},
                  {"initial_counter", initialCounter},
                  {"expected_counter", expectedCounter},
                  {"restored_counter", replay.readCounter(12)},
                  {"restored", true},
                  {"retry_matched", true},
                  {"report", report}},
                 path, false);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void count_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("targets");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<uint32_t>("alpha");
        QTest::addColumn<uint32_t>("selected");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (auto kind : {"uint", "int", "min16uint", "min16int"})
                for (uint32_t samples : {1u, 4u})
                    for (uint32_t targets : {1u, 8u})
                        for (uint32_t mode : {0u, 1u, 2u, 3u}) {
                            const std::vector<uint32_t> alphas =
                                mode == 2   ? std::vector<uint32_t>{0,          1,          0x3f000000,
                                                                    0x3f800000, 0xbf800000, 0x7fc00000}
                                : mode == 0 ? std::vector<uint32_t>{1, 0x3f800000}
                                            : std::vector<uint32_t>{0x3f800000};
                            for (uint32_t alpha : alphas)
                                for (uint32_t selected :
                                     targets == 1 ? std::vector<uint32_t>{0} : std::vector<uint32_t>{7, 0}) {
                                    const auto tag = QString("%1-%2-%3-%4-%5-%6")
                                                         .arg(vendor)
                                                         .arg(kind)
                                                         .arg(samples)
                                                         .arg(targets)
                                                         .arg(mode)
                                                         .arg(alpha) +
                                                     (targets == 8 && selected == 0 ? "-rt0" : "");
                                    QTest::newRow(qPrintable(tag)) << vendor << QString(kind) << samples
                                                                   << targets << mode << alpha << selected;
                                }
                        }
    }
    void count() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, kind);
            QFETCH(uint32_t, samples);
            QFETCH(uint32_t, targets);
            QFETCH(uint32_t, mode);
            QFETCH(uint32_t, alpha);
            QFETCH(uint32_t, selected);
            const auto format = kind.endsWith("uint") ? 3u : 4u;
            const auto resource = Id(1000 + 4 * selected);
            QTemporaryDir directory;
            Json reference = Json::array();
            Json adapters = Json::array();
            for (uint32_t phase : {0u, 1u}) {
                const auto path = directory.filePath(QString("reference-%1.gpa_frame").arg(phase));
                fixture(kind, samples, targets, mode, alpha, phase, 1).save(path);
                Frame frame(path.toStdWString());
                Replay original(frame, options(vendor, mode, false));
                original.run();
                const auto adapter = original.adapterDescription();
                adapters.push_back({{"vendor", adapter.VendorId},
                                    {"device", adapter.DeviceId},
                                    {"luid_low", adapter.AdapterLuid.LowPart},
                                    {"luid_high", adapter.AdapterLuid.HighPart}});
                Json output = Json::array();
                for (uint32_t sample = 0; sample < samples; ++sample)
                    output.push_back(pixel(original, resource, samples, sample, format));
                reference.push_back(output);
            }
            const auto path = directory.filePath("count.gpa_frame");
            fixture(kind, samples, targets, mode, alpha, 0, 260).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor, mode, true));
            replay.run();
            Json result{{"vendor", vendor},       {"kind", kind.toStdString()},
                        {"samples", samples},     {"targets", targets},
                        {"mode", mode},           {"alpha_bits", alpha},
                        {"reference", reference}, {"reports", Json::array()}};
            result["reference_adapters"] = adapters;
            result["selected"] = selected;
            result["private_rt0"] = Json::array();
            result["private_rt0_formats"] = Json::array();
            try {
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const auto initial = bytes(replay, resource, samples, sample, format);
                    const bool auxiliary = selected == 0 && mode == 2;
                    std::vector<std::vector<uint8_t>> originalTargets;
                    for (uint32_t t = 0; t < targets; ++t)
                        originalTargets.push_back(bytes(replay, 1000 + 4 * t, samples, sample, format));
                    auto report = replayNativePixelReturnCount(
                        replay, 100, {selected, 1, 1, 0, sample}, [&](Replay &r, bool after) {
                            if (auxiliary) {
                                r.inspectNativeState([&](auto *context, const auto &) {
                                    Com<ID3D11RenderTargetView> view;
                                    context->OMGetRenderTargets(1, &view, nullptr);
                                    QVERIFY(view != nullptr);
                                    D3D11_RENDER_TARGET_VIEW_DESC desc{};
                                    view->GetDesc(&desc);
                                    QCOMPARE(uint32_t(desc.Format), format);
                                    result["private_rt0_formats"].push_back(uint32_t(desc.Format));
                                });
                                if (after) {
                                    const auto observed = pixel(r, 1000, samples, sample, format);
                                    result["private_rt0"].push_back(observed);
                                    QCOMPARE(Json(observed), reference[0][sample]);
                                }
                            }
                        });
                    QCOMPARE(report["count_transport"]["source_slot"], Json(selected));
                    QCOMPARE(report["count_transport"]["slot"], Json(auxiliary ? 1u : selected));
                    for (uint32_t t = 0; t < targets; ++t)
                        QCOMPARE(bytes(replay, 1000 + 4 * t, samples, sample, format), originalTargets[t]);
                    result["reports"].push_back(report);
                    QCOMPARE(bytes(replay, resource, samples, sample, format), initial);
                }
            } catch (const std::exception &error) {
                result["error"] = error.what();
                save(result, path);
                throw;
            }
            save(result, path);
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const uint32_t expected =
                    130u * (uint32_t(reference[0][sample][0] == 7) + uint32_t(reference[1][sample][0] == 7));
                for (uint32_t phase : {0u, 1u}) {
                    const auto &value = reference[phase][sample];
                    QVERIFY(value == Json(std::array<uint32_t, 4>{0, 0, 0, 0}) ||
                            (value[0] == 7 && value[1] == 11 && value[2] == 13));
                }
                QCOMPARE(result["reports"][sample]["return_count"]["count"], Json(expected));
                QVERIFY(result["reports"][sample]["original_fragment_eligibility"].is_null());
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativePixelIntegerCountTests)
#include "NativePixelIntegerCountTests.moc"
