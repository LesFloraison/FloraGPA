#include "MsaaCapture.h"
#include "application/TextureInspector.h"
#include "core/TextureStorage.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class TextureInspectorTests : public QObject {
    Q_OBJECT
  private slots:
    void ddsAndPlanes() {
        Resource r;
        r.type = 0x85;
        r.desc = {8, 8, 2, 12, 28, 1, 0, 0, 8, 0, 4};
        const auto subs = textureSubresources(r);
        std::vector<uint8_t> data(size_t(subs.back().offset + subs.back().size), 0x5a);
        const auto dds = textureDds(r, data);
        QCOMPARE(dds.size(), data.size() + 148);
        Reader header(dds);
        QCOMPARE(header.read<uint32_t>(), 0x20534444u);
        QCOMPARE(header.read<uint32_t>(), 124u);
        header.skip(17 * 4);
        QCOMPARE(header.read<uint32_t>(), 32u);
        QCOMPARE(header.read<uint32_t>(), 4u);
        QCOMPARE(header.read<uint32_t>(), 0x30315844u);
        Reader dx10(Bytes(dds).subspan(128));
        QCOMPARE(dx10.read<uint32_t>(), 28u);
        QCOMPARE(dx10.read<uint32_t>(), 3u);
        QCOMPARE(dx10.read<uint32_t>(), 4u);
        QCOMPARE(dx10.read<uint32_t>(), 2u);
        r.desc[3] = 7;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureDds(r, data));
        r.desc = {10, 6, 1, 2, 104, 1, 0, 0, 8, 0, 0};
        auto plane = texturePlane(r, 0, 1, "auto", 36);
        QVERIFY(plane);
        QCOMPARE(plane->name, std::string("uv"));
        QCOMPARE(plane->offset, uint64_t(300));
        QCOMPARE(plane->size, uint64_t(60));
        QCOMPARE(plane->width, 5u);
        QCOMPARE(plane->height, 3u);
        QCOMPARE(plane->rowPitch, 20u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, texturePlane(r, 0, 1, "y", 36));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, texturePlane(r, 0, 2, "auto"));
        r.desc[4] = 28;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, texturePlane(r, 0, 0, "uv"));
    }
    void legacyLuma() {
        Resource r;
        r.type = 0x85;
        r.desc = {10, 6, 1, 2, 105, 1, 0, 0, 8, 0, 0};
        std::vector<uint8_t> raw(360);
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = uint8_t(i);
        const auto result = capturedLuma(r, raw, 0, 1, 0, "y", 57);
        QCOMPARE(result.bytes.size(), size_t(120));
        QCOMPARE(result.sourceOffset, uint64_t(180));
        for (size_t y = 0; y < 6; ++y)
            for (size_t x = 0; x < 20; ++x)
                QCOMPARE(result.bytes[y * 20 + x], raw[180 + y * 30 + x]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturedLuma(r, raw, 0, 1, 0, "uv"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturedLuma(r, raw, 0, 1, 0, "auto", 35));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturedLuma(r, raw, 0, 1, 1, "y"));
        raw.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturedLuma(r, raw, 0, 1, 0, "y"));
    }
    void msaaInspectionPreservesState() {
        QTemporaryDir dir;
        auto c = msaaOutputCapture();
        auto path = dir.path() + "/msaa.gpa_frame";
        c.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 110;
        Replay replay(frame, options);
        replay.run();
        const auto counts = replay.counts;
        auto binding = [&] {
            std::array<uintptr_t, 4> result{};
            replay.inspectNativeState([&](auto *context, const auto &) {
                Com<ID3D11VertexShader> vs;
                Com<ID3D11PixelShader> ps;
                Com<ID3D11RenderTargetView> rtv;
                context->VSGetShader(&vs, nullptr, nullptr);
                context->PSGetShader(&ps, nullptr, nullptr);
                context->OMGetRenderTargets(1, &rtv, nullptr);
                D3D11_PRIMITIVE_TOPOLOGY topology{};
                context->IAGetPrimitiveTopology(&topology);
                result = {reinterpret_cast<uintptr_t>(vs.Get()), reinterpret_cast<uintptr_t>(ps.Get()),
                          reinterpret_cast<uintptr_t>(rtv.Get()), uintptr_t(topology)};
            });
            return result;
        };
        const auto before = binding();
        TextureInspectionOptions select;
        select.sample = 2;
        select.layer = 1;
        select.channel = "r";
        select.high = 40;
        auto result = inspectTexture(replay, 20, select);
        QCOMPARE(result.metadata["msaa"]["storage_path"], nlohmann::json("integer_bits"));
        QCOMPARE(result.metadata["msaa"]["initial_samples_reconstructed"], nlohmann::json(false));
        QVERIFY(result.image);
        QCOMPARE(result.image->width, 7u);
        QCOMPARE(result.image->height, 5u);
        QCOMPARE(result.subresource.size(), size_t(7 * 5 * 16));
        QCOMPARE(Reader(result.subresource).read<uint32_t>(), 32u);
        QCOMPARE(binding(), before);
        QCOMPARE(replay.counts, counts);
        exportTextureInspection(result, std::filesystem::path(dir.path().toStdWString()) / L"export");
        QVERIFY(QFile::exists(dir.path() + "/export/texture.dds"));
        QVERIFY(QFile::exists(dir.path() + "/export/preview.png"));
        // The selected UINT sample contains R=32. A [-32, 96] display range
        // maps it to 0.5, independently of the Python shader-string oracle.
        select.low = -32;
        select.high = 96;
        auto negativeRange = inspectTexture(replay, 20, select);
        QVERIFY(negativeRange.image);
        for (size_t i = 0; i < negativeRange.image->rgba.size(); i += 4) {
            QCOMPARE(negativeRange.image->rgba[i], uint8_t(128));
            QCOMPARE(negativeRange.image->rgba[i + 1], uint8_t(128));
            QCOMPARE(negativeRange.image->rgba[i + 2], uint8_t(128));
            QCOMPARE(negativeRange.image->rgba[i + 3], uint8_t(255));
        }
        QCOMPARE(binding(), before);
        QCOMPARE(replay.counts, counts);
        select.sample.reset();
        select.preview = false;
        auto resolved = inspectTexture(replay, 20, select);
        QVERIFY(!resolved.image);
        QVERIFY(!resolved.metadata["output_edit_supported"].get<bool>());
        QCOMPARE(binding(), before);
    }
    void captureEndAndLegacyExport() {
        QTemporaryDir dir;
        Capture c;
        c.add(20, 5, 0x87, statePack(Id(0), Id(0), 2u, 2u, 1u, 1u, 28u, 1u, 0u, 0u, 8u, 0u, 0u, Id(21)));
        auto raw = word(16);
        raw.resize(20, 0x7f);
        c.add(21, 9, 1, raw);
        c.add(30, 5, 0x85, statePack(Id(0), Id(0), 2u, 2u, 1u, 1u, 104u, 1u, 0u, 0u, 8u, 0u, 0u, Id(31)));
        auto legacy = word(12);
        for (uint8_t i = 0; i < 12; ++i)
            legacy.push_back(i);
        c.add(31, 9, 1, legacy);
        auto path = dir.path() + "/initial.gpa_frame";
        c.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        TextureInspectionOptions selection;
        selection.preview = false;
        auto result = inspectTexture(replay, 20, selection);
        QCOMPARE(result.storage.size(), size_t(16));
        selection.typedFormat = 57;
        auto luma = inspectTexture(replay, 30, selection);
        QCOMPARE(luma.storage, std::vector<uint8_t>({0, 1, 2, 3, 6, 7, 8, 9}));
        QVERIFY(luma.metadata["recovered_luma_only"].get<bool>());
        QVERIFY(!luma.metadata["uv_available"].get<bool>());
        QCOMPARE(luma.captured.size(), size_t(12));
        exportTextureInspection(luma, std::filesystem::path(dir.path().toStdWString()) / L"luma");
        QVERIFY(QFile::exists(dir.path() + "/luma/luma.dds"));
        QVERIFY(QFile::exists(dir.path() + "/luma/capture_data.bin"));
        QVERIFY(!QFile::exists(dir.path() + "/luma/texture.dds"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    TextureInspectorTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "TextureInspectorTests.moc"
