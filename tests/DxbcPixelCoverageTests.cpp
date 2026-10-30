#include "ClassCapture.h"
#include "application/DxbcPixelCompletion.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> program(uint32_t mask, bool early, uint32_t flow) {
    auto raw = compileClassProgram(
        "AppendStructuredBuffer<uint> u:register(u1);"
        "struct O{float4 c:SV_Target;uint mask:SV_Coverage;};" +
            std::string(early ? "[earlydepthstencil]" : "") +
            "O main(){u.Append(7);O o;o.c=float4(1,0,0,0);o.mask=" + std::to_string(mask) + "u;return o;}",
        "ps_5_0");
    if (!flow)
        return raw;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
    for (const auto &[tag, bytes] : readDxbcParts(raw)) {
        if (tag == 0x54415453 || tag == 0x47424453 || tag == 0x42445053)
            continue;
        auto data = std::vector<uint8_t>(bytes.begin(), bytes.end());
        if (tag == 0x52444853 || tag == 0x58454853) {
            auto code = readDxbcProgram(data);
            std::vector<std::vector<uint32_t>> rows, subroutine;
            for (const auto &row : code.instructions) {
                if (flow == 3 && row.size() > 1 && row[1] == 0x0000f001) {
                    subroutine.push_back(row);
                    rows.push_back({0x03000004, 0x0010a000, 0});
                } else {
                    if (flow != 3 && (row[0] & 2047) == 62) {
                        rows.push_back({0x0304003f, 0x00004001,
                                        flow == 4   ? UINT32_MAX
                                        : flow == 2 ? 1u
                                                    : 0u});
                        rows.push_back({0x04000036, 0x0000f001, 0x00004001, 1});
                    }
                    rows.push_back(row);
                }
            }
            if (flow == 3) {
                if (subroutine.size() != 1)
                    throw std::runtime_error("Coverage fixture expects one output write");
                rows.push_back({0x0300002c, 0x0010a000, 0});
                rows.push_back(subroutine.front());
                rows.push_back({0x0100003e});
            }
            code.instructions = std::move(rows);
            data = writeDxbcProgram(code);
        }
        parts.emplace_back(tag, std::move(data));
    }
    DxbcParts views;
    for (const auto &[tag, data] : parts)
        views.emplace_back(tag, data);
    return makeDxbc(views);
}
struct Observation {
    std::vector<uint8_t> color, buffer;
    uint32_t counter = 0;
    Json diagnostics = Json::array();
    Json adapter;
};
Observation observe(Bytes code, uint32_t vendor, bool alphaCoverage) {
    auto capture = graphicsCounterCapture(false);
    std::erase_if(capture.entries, [](const auto &e) { return e.id == 33 || e.id == 34; });
    auto payload = statePack(uint64_t(code.size()));
    payload.insert(payload.end(), code.begin(), code.end());
    append(payload, Id(0));
    capture.add(33, 9, 0x81, payload);
    auto blend = statePack(Id(0), Id(0), uint32_t(alphaCoverage), 0u);
    for (int i = 0; i < 8; ++i) {
        const auto row = statePack(0u, 2u, 1u, 1u, 2u, 1u, 1u, 15u);
        blend.insert(blend.end(), row.begin(), row.end());
    }
    capture.add(64, 5, 0x8a, blend);
    State state{};
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.viewports = 22;
    state.blend = 64;
    state.stages[0].shader = 30;
    state.stages[4].shader = 32;
    state.rtv[0] = 21;
    state.rtv[1] = 12;
    state.omStart = 1;
    state.rtCount = 2;
    capture.add(34, 3, 3, snapshot(state));
    capture.add(195, 7, 0x32, statePack(Id(0), Id(1), Id(21), uint8_t(1), 0.f, 0.f, 1.f, 1.f));
    QTemporaryDir dir;
    const auto path = dir.filePath("frame.gpa_frame");
    capture.save(path);
    Frame frame(path.toStdWString());
    ReplayOptions options;
    options.debug = true;
    options.warp = !vendor;
    if (vendor)
        options.vendor = vendor;
    options.until = 200;
    Replay replay(frame, options);
    replay.run();
    Observation result{replay.readTexture(20), replay.readBuffer(10), replay.readCounter(12)};
    replay.inspectNativeState([&](auto *context, const auto &) {
        Com<ID3D11Device> device;
        context->GetDevice(&device);
        Com<ID3D11InfoQueue> queue;
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> adapter;
        check(device.As(&dxgi), "Get coverage test DXGI device");
        check(dxgi->GetAdapter(&adapter), "Get coverage test adapter");
        DXGI_ADAPTER_DESC desc{};
        check(adapter->GetDesc(&desc), "Get coverage test adapter description");
        result.adapter = {{"vendor", desc.VendorId},
                          {"device", desc.DeviceId},
                          {"luid", {desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart}}};
        check(device.As(&queue), "Get coverage test validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Get coverage test diagnostic size");
            std::vector<uint8_t> storage(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(queue->GetMessage(i, message, &size), "Get coverage test diagnostic");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
                result.diagnostics.push_back(message->pDescription);
        }
    });
    return result;
}
} // namespace
class DxbcPixelCoverageTests final : public QObject {
    Q_OBJECT
  private slots:
    void execution_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("mask");
        QTest::addColumn<uint32_t>("flow");
        QTest::addColumn<bool>("early");
        QTest::addColumn<bool>("alpha");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto mask : {0u, 1u, 2u, 0x80000000u, UINT32_MAX})
                for (auto flow : {0u, 1u, 2u, 3u, 4u})
                    for (bool early : {false, true})
                        for (bool alpha : {false, true})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(mask)
                                                         .arg(flow)
                                                         .arg(early)
                                                         .arg(alpha)))
                                << vendor << mask << flow << early << alpha;
    }
    void execution() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, mask);
        QFETCH(uint32_t, flow);
        QFETCH(bool, early);
        QFETCH(bool, alpha);
        try {
            const auto original = program(mask, early, flow);
            const auto normalized = normalizeSingleSamplePixelCoverage(original);
            const auto before = observe(original, vendor, alpha);
            const auto after = observe(normalized.bytecode, vendor, alpha);
            const auto root = qEnvironmentVariable("FLORA_PIXEL_COVERAGE_EVIDENCE");
            Json report{{"vendor", vendor},
                        {"mask", mask},
                        {"flow", flow},
                        {"early", early},
                        {"alpha", alpha},
                        {"normalization", normalized.identity},
                        {"original_adapter", before.adapter},
                        {"normalized_adapter", after.adapter},
                        {"original", before.color},
                        {"normalized", after.color},
                        {"original_buffer", before.buffer},
                        {"normalized_buffer", after.buffer},
                        {"original_counter", before.counter},
                        {"normalized_counter", after.counter},
                        {"original_diagnostics", before.diagnostics},
                        {"normalized_diagnostics", after.diagnostics}};
            if (!root.isEmpty()) {
                QDir().mkpath(root);
                const auto base = root + "/" + QString::fromLatin1(QTest::currentDataTag());
                const auto text = report.dump(2);
                for (const auto &[suffix, bytes] : std::vector<std::pair<QString, Bytes>>{
                         {".json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size())},
                         {".original.dxbc", original},
                         {".normalized.dxbc", normalized.bytecode}}) {
                    QFile file(base + suffix);
                    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                    QCOMPARE(file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())),
                             qint64(bytes.size()));
                }
            }
            QVERIFY2(before.diagnostics.empty(), before.diagnostics.dump().c_str());
            QCOMPARE(before.adapter, after.adapter);
            QCOMPARE(before.adapter.at("vendor"), Json(vendor ? vendor : 5140));
            QVERIFY2(after.diagnostics.empty(), after.diagnostics.dump().c_str());
            const bool visible = flow == 1 || (mask & 1);
            QCOMPARE(before.color,
                     (visible ? std::vector<uint8_t>{255, 0, 0, 0} : std::vector<uint8_t>{0, 0, 255, 255}));
            QCOMPARE(after.color, before.color);
            QCOMPARE(before.counter, 1u);
            QCOMPARE(after.counter, before.counter);
            QCOMPARE(before.buffer, (std::vector<uint8_t>{7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
            QCOMPARE(after.buffer, before.buffer);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void guards() {
        const auto plain = compileClassProgram("float4 main():SV_Target{return 1;}", "ps_5_0");
        QCOMPARE(normalizeSingleSamplePixelCoverage(plain).bytecode, plain);
        const auto code = program(0, false, 0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizeSingleSamplePixelCoverage(code, 1));
        QVERIFY_THROWS_EXCEPTION(std::exception,
                                 normalizeSingleSamplePixelCoverage(code, 1024 * 1024, [] { return true; }));
    }
};
QTEST_GUILESS_MAIN(DxbcPixelCoverageTests)
#include "DxbcPixelCoverageTests.moc"
