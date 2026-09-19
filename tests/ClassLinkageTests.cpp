#include "ClassCapture.h"
#include "application/ClassInspector.h"
#include "application/ReplayPipeline.h"
#include "application/ShaderInspector.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class ClassLinkageTests final : public QObject {
    Q_OBJECT
    static Json field(const Json &report, const char *name) {
        for (const auto &row : report.at("fields"))
            if (row.at("field") == name)
                return row;
        throw std::runtime_error("Missing pipeline field");
    }
  private slots:
    void linkage_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("created");
        QTest::newRow("named-hardware") << false << false;
        QTest::newRow("created-hardware") << false << true;
        QTest::newRow("named-warp") << true << false;
        QTest::newRow("created-warp") << true << true;
    }
    void linkage() {
        QFETCH(bool, warp);
        QFETCH(bool, created);
        auto capture = classCapture(created);
        QTemporaryDir dir;
        capture.save(dir.path() + "/linked.gpa_frame");
        Frame frame((dir.path() + "/linked.gpa_frame").toStdWString());
        QCOMPARE(inspectClass(frame, 60), Json({{"resource_kind", "class_linkage"}}));
        auto metadata = inspectClass(frame, 62);
        QCOMPARE(metadata["class_linkage_id"], Json(60));
        QCOMPARE(metadata["creation_method"], Json(created ? "CreateClassInstance" : "GetClassInstance"));
        QCOMPARE(inspectShader(frame.shader(16))["interface_slots"], Json(1));
        ReplayOptions options;
        options.warp = warp;
        options.until = 100;
        Replay replay(frame, options);
        auto report = inspectReplayPipeline(frame, replay);
        QCOMPARE(field(report, "cs.classes")["value"], Json::array({62}));
        QCOMPARE(field(report, "cs.classes")["object"][0]["captured_ids"], Json::array({62}));
        auto output = replay.readBuffer(7);
        Reader reader(output);
        for (uint32_t i = 0; i < 4; ++i)
            QCOMPARE(reader.read<uint32_t>(), (created ? 40u : 20u) + i);
        QCOMPARE(inspectReplayPipeline(frame, replay), report);
        QCOMPARE(replay.readBuffer(7), output);
        options.shaders[15] = classProgram(false);
        Replay replacement(frame, options);
        auto replaced = inspectReplayPipeline(frame, replacement, true);
        QCOMPARE(field(replaced, "cs.classes")["value"], Json::array());
        auto bytes = replacement.readBuffer(7);
        Reader replacedBytes(bytes);
        for (uint32_t i = 0; i < 4; ++i)
            QCOMPARE(replacedBytes.read<uint32_t>(), 77u + i);
        auto artifact = qEnvironmentVariable("FLORA_CLASS_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            capture.save(artifact + (created ? "/created.gpa_frame" : "/named.gpa_frame"));
        }
    }
    void invalid() {
        for (auto capture : {classCapture(false, 2), classCapture(false, 257), classCapture(false, 1, 0),
                             classCapture(false, 1, 60), classCapture(false, 1, 62, 2),
                             classCapture(false, 1, 62, UINT32_MAX, true)}) {
            QTemporaryDir dir;
            capture.save(dir.path() + "/invalid.gpa_frame");
            Frame frame((dir.path() + "/invalid.gpa_frame").toStdWString());
            ReplayOptions options;
            options.warp = true;
            options.until = 100;
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
    }
    void recordBounds() {
        auto reject = [&](Capture capture, Id id) {
            QTemporaryDir dir;
            capture.save(dir.path() + "/malformed.gpa_frame");
            Frame frame((dir.path() + "/malformed.gpa_frame").toStdWString());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readClassRecord(frame, id));
        };
        for (auto [id, length] : {std::pair<Id, uint32_t>{60, 16}, {62, 56}, {64, 14}})
            for (uint32_t truncated = 0; truncated < length; ++truncated) {
                auto capture = classCapture();
                for (auto &entry : capture.entries)
                    if (entry.id == id)
                        entry.size = truncated;
                reject(std::move(capture), id == 60 ? 60 : 62);
            }
        for (int kind = 0; kind < 5; ++kind) {
            auto capture = classCapture();
            for (const auto &entry : capture.entries)
                if (entry.id == 64) {
                    if (kind == 0)
                        put(capture.bytes, entry.offset, 257u);
                    if (kind == 1)
                        capture.bytes[entry.offset + 5] = 0;
                    if (kind == 2)
                        capture.bytes[entry.offset + 5] = 0xff;
                } else if (entry.id == 62) {
                    if (kind == 3)
                        put(capture.bytes, entry.offset + 8, Id(2));
                    if (kind == 4)
                        put(capture.bytes, entry.offset + 48, Id(3));
                }
            reject(std::move(capture), 62);
        }
    }
    void capturedShaderBoundaries() {
        auto capture = classCapture();
        capture.add(90, 7, 0x3523, statePack(Id(0), Id(1), Id(15), 1u, uint8_t(1), Id(62)));
        capture.add(95, 7, 0x3523, statePack(Id(0), Id(1), Id(0), 0u, uint8_t(0)));
        QTemporaryDir dir;
        capture.save(dir.path() + "/setter.gpa_frame");
        Frame frame((dir.path() + "/setter.gpa_frame").toStdWString());
        for (Id event : {90, 95, 100}) {
            ReplayOptions options;
            options.warp = true;
            options.until = event;
            Replay replay(frame, options);
            auto report = inspectReplayPipeline(frame, replay);
            QCOMPARE(field(report, "cs.shader")["value"], Json(event == 100 ? 15 : 0));
            QCOMPARE(field(report, "cs.classes")["value"], event == 100 ? Json::array({62}) : Json::array());
        }
    }
    void emptyFunctionTable() {
        auto capture = graphicsClassCapture(true);
        QTemporaryDir dir;
        capture.save(dir.path() + "/empty-table.gpa_frame");
        Frame frame((dir.path() + "/empty-table.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 200;
        Replay warp(frame, options);
        warp.run();
        QCOMPARE(warp.output(20).rgba, (std::vector<uint8_t>{255, 0, 0, 255}));
        Com<ID3D11Device> device;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &device, nullptr, nullptr),
              "Query test adapter");
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        check(device.As(&dxgi), "Query test DXGI device");
        check(dxgi->GetAdapter(&adapter), "Get test adapter");
        check(adapter->GetDesc(&desc), "Get test adapter description");
        options.warp = false;
        Replay hardware(frame, options);
        if (desc.VendorId == 0x10de && desc.DeviceId == 0x249d) {
            bool rejected = false;
            try {
                hardware.run();
            } catch (const std::runtime_error &error) {
                rejected = std::string(error.what()).find("--warp") != std::string::npos;
            }
            QVERIFY(rejected);
        } else {
            hardware.run();
            QCOMPARE(hardware.output(20).rgba, (std::vector<uint8_t>{255, 0, 0, 255}));
        }
        auto artifact = qEnvironmentVariable("FLORA_CLASS_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            capture.save(artifact + "/empty-table.gpa_frame");
        }
    }
};
QTEST_GUILESS_MAIN(ClassLinkageTests)
#include "ClassLinkageTests.moc"
