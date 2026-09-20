#include "SyntheticCapture.h"
#include "application/PlanarWrites.h"
#include "application/TextureInspector.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
Capture mapCapture(uint32_t format, uint32_t kind, uint32_t index, const std::vector<uint8_t> &raw,
                   bool diff = false) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    std::vector<uint8_t> resource(16);
    const bool discard = kind == 4;
    append(resource, D3D11_TEXTURE2D_DESC{10,
                                          6,
                                          1,
                                          discard ? 1u : 2u,
                                          DXGI_FORMAT(format),
                                          {1, 0},
                                          discard ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_STAGING,
                                          discard ? 8u : 0u,
                                          discard ? 0x10000u : 0x30000u,
                                          0});
    append(resource, Id(0));
    c.add(20, 5, 0x85, resource);
    auto data = diff ? std::vector<uint8_t>(8) : word(uint32_t(raw.size()));
    if (!diff)
        data.insert(data.end(), raw.begin(), raw.end());
    c.add(21, 9, diff ? 0x100 : 1, data);
    std::vector<uint8_t> command(16);
    put(command, 8, Id(1));
    append(command, int32_t(0));
    append(command, Id(20));
    append(command, index);
    append(command, kind);
    append(command, 0u);
    append(command, Id(21));
    c.add(80, 7, 0x246, command);
    return c;
}
} // namespace
class PlanarWriteTests : public QObject {
    Q_OBJECT
  private slots:
    void map_data() {
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("kind");
        QTest::addColumn<bool>("warp");
        for (auto format : {103u, 104u, 105u})
            for (auto kind : {2u, 3u, 4u})
                for (bool warp : {false, true}) {
                    if (!warp && format == 105)
                        continue; // Tested as a controlled device rejection by the CLI comparison.
                    const auto name = QString("%1-%2-%3").arg(format).arg(kind).arg(warp);
                    QTest::newRow(qPrintable(name)) << format << kind << warp;
                }
    }
    void map() {
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, kind);
        QFETCH(bool, warp);
        QTemporaryDir dir;
        const uint32_t unit = format == 103 ? 1 : 2, index = kind == 4 ? 0 : 1;
        std::vector<uint8_t> raw(90 * unit);
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = uint8_t(i * 17 + 13);
        auto capture = mapCapture(format, kind, index, raw);
        auto path = dir.path() + "/map.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = warp;
        options.until = 80;
        std::vector<uint8_t> initial(90 * unit * (kind == 4 ? 1 : 2), 0x7b);
        options.textures[20] = initial;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            const auto bytes = replay.readTexture(20);
            auto expected = initial;
            for (size_t y = 0; y < 6; ++y)
                std::copy_n(raw.begin() + y * (format == 103 ? 10 : 30), 10 * unit,
                            expected.begin() + index * 90 * unit + y * 10 * unit);
            if (kind == 4)
                QCOMPARE(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 60 * unit),
                         std::vector<uint8_t>(expected.begin(), expected.begin() + 60 * unit));
            else
                QCOMPARE(bytes, expected);
            QCOMPARE(replay.planarWrites().size(), size_t(1));
            auto report = planarWriteReport(replay);
            QCOMPARE(report[0]["written_planes"], nlohmann::json::array({"y"}));
            QCOMPARE(report[0]["uv_effect"],
                     nlohmann::json(kind == 4 ? "discarded_unspecified" : "retained"));
            QCOMPARE(report[0]["source_row_pitch"].get<uint32_t>(), format == 103 ? 10u : 30u);
            QCOMPARE(report[0]["written_row_bytes"].get<uint32_t>(), 10u * unit);
            QVERIFY(report[0]["native_row_pitch"].get<uint32_t>() >= 10 * unit);
            if (warp && format != 103)
                QVERIFY(report[0]["native_row_pitch"].get<uint32_t>() < 30);
            TextureInspectionOptions view;
            view.preview = false;
            view.layer = index;
            QCOMPARE(inspectTexture(replay, 20, view).metadata["planar_writes"], report);
        }
        options.before = true;
        Replay before(frame, options);
        before.run();
        QCOMPARE(before.readTexture(20), initial);
        QVERIFY(before.planarWrites().empty());
        options.before = false;
        options.disabled.insert(80);
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(disabled.readTexture(20), initial);
        QVERIFY(disabled.planarWrites().empty());
    }
    void invalidMap() {
        QTemporaryDir dir;
        for (auto format : {103u, 104u, 105u})
            for (int invalid = 0; invalid < 5; ++invalid) {
                const auto size = format == 103 ? 90 : 180;
                auto capture = mapCapture(format, invalid == 3 ? 5 : 2, invalid == 2 ? 2 : 0,
                                          std::vector<uint8_t>(size + (invalid == 0   ? -1
                                                                       : invalid == 1 ? 1
                                                                                      : 0)),
                                          invalid == 4);
                auto path = dir.path() + "/invalid.gpa_frame";
                capture.save(path);
                Frame frame(path.toStdWString());
                ReplayOptions options;
                options.warp = true;
                options.until = 80;
                Replay replay(frame, options);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
                QVERIFY(replay.planarWrites().empty());
                QVERIFY(!replay.counts.contains("Map"));
            }
    }
    void updateRegion() {
        try {
            QTemporaryDir dir;
            for (uint32_t format : {103u, 104u, 105u}) {
                const uint32_t unit = format == 103 ? 1 : 2;
                Capture capture;
                capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
                std::vector<uint8_t> resource(16);
                append(resource, D3D11_TEXTURE2D_DESC{
                                     10, 6, 1, 2, DXGI_FORMAT(format), {1, 0}, D3D11_USAGE_DEFAULT, 8, 0, 0});
                append(resource, Id(0));
                capture.add(20, 5, 0x85, resource);
                std::vector<uint8_t> raw(36 * unit);
                for (size_t i = 0; i < raw.size(); ++i)
                    raw[i] = uint8_t(i * 3 + 5);
                auto data = word(uint32_t(raw.size()));
                data.insert(data.end(), raw.begin(), raw.end());
                capture.add(21, 9, 1, data);
                std::vector<uint8_t> command(16);
                put(command, 8, Id(1));
                append(command, Id(20));
                append(command, 1u);
                append(command, uint8_t(1));
                append(command, D3D11_BOX{2, 2, 0, 8, 6, 1});
                append(command, Id(21));
                append(command, 4096u);
                append(command, 8192u);
                capture.add(80, 7, 0x247, command);
                auto path = dir.path() + "/update.gpa_frame";
                capture.save(path);
                Frame frame(path.toStdWString());
                ReplayOptions options;
                options.warp = true;
                options.until = 80;
                std::vector<uint8_t> initial(180 * unit, 0x7b), expected = initial;
                options.textures[20] = initial;
                for (uint32_t row = 0; row < 4; ++row)
                    std::copy_n(raw.begin() + row * 6 * unit, 6 * unit,
                                expected.begin() + (90 + (2 + row) * 10 + 2) * unit);
                for (uint32_t row = 0; row < 2; ++row)
                    std::copy_n(raw.begin() + (4 + row) * 6 * unit, 6 * unit,
                                expected.begin() + (90 + 60 + (1 + row) * 10 + 2) * unit);
                Replay captured(frame, options);
                if (format == 103) {
                    captured.run();
                    QCOMPARE(captured.readTexture(20), expected);
                    QCOMPARE(planarWriteReport(captured)[0]["uv_effect"],
                             nlohmann::json("captured_region_bytes"));
                } else {
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, captured.run());
                    QVERIFY(captured.planarWrites().empty());
                }
                options.updateSources[80] = raw;
                Replay edited(frame, options);
                edited.run();
                QCOMPARE(edited.readTexture(20), expected);
                const auto report = planarWriteReport(edited)[0];
                QCOMPARE(report["source_origin"], nlohmann::json("explicit_experiment"));
                QCOMPARE(report["written_planes"], nlohmann::json::array({"y", "uv"}));
                QCOMPARE(report["box"], nlohmann::json::array({2, 2, 0, 8, 6, 1}));
                QCOMPARE(report["row_pitch"].get<uint32_t>(), 6u * unit);
                QCOMPARE(report["slice_pitch"].get<uint32_t>(), 36u * unit);
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    PlanarWriteTests test;
    return QTest::qExec(&test, argc, argv);
}
#include "PlanarWriteTests.moc"
