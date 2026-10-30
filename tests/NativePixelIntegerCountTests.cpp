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
void save(const Json &report, const QString &frame) {
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
    for (uint32_t phase : {0u, 1u})
        if (!QFile::copy(QFileInfo(frame).dir().filePath(QString("reference-%1.gpa_frame").arg(phase)),
                         base + QString(".reference-%1.gpa_frame").arg(phase)))
            throw std::runtime_error("Cannot retain original integer reference frame");
}
} // namespace
class NativePixelIntegerCountTests final : public QObject {
    Q_OBJECT
  private slots:
    void count_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("targets");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<uint32_t>("alpha");
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
                                QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5-%6")
                                                             .arg(vendor)
                                                             .arg(kind)
                                                             .arg(samples)
                                                             .arg(targets)
                                                             .arg(mode)
                                                             .arg(alpha)))
                                    << vendor << QString(kind) << samples << targets << mode << alpha;
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
            const auto format = kind.endsWith("uint") ? 3u : 4u;
            const auto resource = Id(1000 + 4 * (targets - 1));
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
            const bool unsupportedCoverage = targets == 1 && mode == 2;
            result["unsupported_integer_rt0_coverage"] = unsupportedCoverage;
            result["rejected_selections"] = 0;
            try {
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const auto initial = bytes(replay, resource, samples, sample, format);
                    if (unsupportedCoverage) {
                        bool rejected = false;
                        try {
                            replayNativePixelReturnCount(replay, 100, {0, 1, 1, 0, sample});
                        } catch (const std::runtime_error &error) {
                            QCOMPARE(std::string(error.what()),
                                     std::string("Integer RT0 alpha-to-coverage is not preserved by the "
                                                 "float count target"));
                            rejected = true;
                        }
                        QVERIFY(rejected);
                        QCOMPARE(bytes(replay, resource, samples, sample, format), initial);
                        result["rejected_selections"] = sample + 1;
                        continue;
                    }
                    auto report = replayNativePixelReturnCount(replay, 100, {targets - 1, 1, 1, 0, sample});
                    result["reports"].push_back(report);
                    QCOMPARE(bytes(replay, resource, samples, sample, format), initial);
                }
            } catch (const std::exception &error) {
                result["error"] = error.what();
                save(result, path);
                throw;
            }
            save(result, path);
            if (unsupportedCoverage)
                return;
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
