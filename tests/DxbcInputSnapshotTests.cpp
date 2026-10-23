#include "ConstantBufferCapture.h"
#include "application/DxbcInputSnapshot.h"
#include "application/HlslCompilation.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
ReplayOptions options(uint32_t vendor, Id event = 100) {
    ReplayOptions result;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    result.until = event;
    result.before = true;
    result.debug = true;
    return result;
}
void replaceShader(testing::Capture &capture, Bytes shader) {
    std::erase_if(capture.entries, [](auto &e) { return e.id == 16; });
    std::vector<uint8_t> bytes;
    testing::append(bytes, uint64_t(shader.size()));
    bytes.insert(bytes.end(), shader.begin(), shader.end());
    testing::append(bytes, Id(0));
    capture.add(16, 9, 0x81, bytes);
}
void buffer(testing::Capture &capture, Id id, Id data, UINT size, UINT bind, UINT misc, UINT stride,
            uint32_t base) {
    std::erase_if(capture.entries, [&](auto &e) { return e.id == id || e.id == data; });
    std::vector<uint8_t> payload(16);
    testing::append(payload, D3D11_BUFFER_DESC{size, D3D11_USAGE_DEFAULT, bind, 0, misc, stride});
    testing::append(payload, data);
    capture.add(id, 5, 0x83, payload);
    auto bytes = testing::word(size);
    for (uint32_t i = 0; i < size / 4; ++i)
        testing::append(bytes, base + i);
    capture.add(data, 9, 1, bytes);
}
void execute(const InputSnapshot &snapshot) {
    Program program(snapshot.bytecode);
    Group group(program, snapshot.inputs);
    while (!group.finished())
        group.step();
}
void validate(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Read snapshot validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read snapshot message size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read snapshot message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
std::string hash(Bytes bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
void evidence(const InputSnapshot &snapshot, Replay &replay, const char *fixture, uint32_t vendor) {
    const auto gpu = replay.readBuffer(7), cpu = snapshot.inputs.memory->buffers().at(7).bytes;
    qInfo().noquote() << QString::fromStdString(
        nlohmann::json({{"probe", "dxbc-buffer-inputs"},
                        {"fixture", fixture},
                        {"vendor", vendor},
                        {"adapter_vendor", snapshot.adapterVendor},
                        {"adapter_device", snapshot.adapterDevice},
                        {"adapter_luid", snapshot.adapterLuid},
                        {"event", snapshot.event},
                        {"shader", snapshot.shader},
                        {"frame_sha256", snapshot.frameSha256},
                        {"shader_sha256", hash(snapshot.bytecode)},
                        {"cpu_output_sha256", hash(cpu)},
                        {"gpu_output_sha256", hash(gpu)},
                        {"source", InputSnapshot::source},
                        {"comparison", "exact observed buffer bytes and counter state"},
                        {"passed", true}})
            .dump());
}
} // namespace
class DxbcInputSnapshotTests final : public QObject {
    Q_OBJECT
  private slots:
    void compute_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("ordered");
        QTest::addColumn<bool>("edited");
        QTest::addColumn<uint64_t>("event");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool ordered : {false, true})
                for (bool edited : {false, true})
                    for (uint64_t event : {100u, 110u})
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(ordered).arg(edited).arg(event)))
                            << vendor << ordered << edited << event;
    }
    void compute() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, ordered);
        QFETCH(bool, edited);
        QFETCH(uint64_t, event);
        try {
            QTemporaryDir directory;
            const auto path = directory.path() + "/inputs.gpa_frame";
            testing::computeCapture(ordered).save(path);
            Frame frame(path.toStdWString());
            auto configuration = options(vendor, event);
            if (edited) {
                configuration.buffers[event][2] = {{0, testing::word(3)}};
                configuration.buffers[event][4] = {{0, testing::word(11)}};
                configuration.buffers[event][7] = {{0, testing::word(50)}};
                configuration.uavCounters[event][12] = 2;
            }
            Replay replay(frame, configuration);
            auto snapshot = snapshotBufferInputs(replay, 5);
            QCOMPARE(snapshot.event, event);
            QCOMPARE(snapshot.shader, Id(15));
            QCOMPARE(snapshot.stage, 5u);
            QCOMPARE(snapshot.frameSha256, frame.sha256());
            QCOMPARE(snapshot.replayGeneration, replay.generation());
            if (vendor)
                QCOMPARE(snapshot.adapterVendor, vendor);
            QCOMPARE(snapshot.inputs.constantBuffers.at(0).at(0).bits[0], edited ? 3u : 2u);
            QVERIFY(snapshot.inputs.constantBuffers.at(13).empty());
            QCOMPARE(snapshot.bindings.at({8, {0}}).resource, Id(2));
            QCOMPARE(snapshot.bindings.at({7, {0}}).view, Id(6));
            QCOMPARE(snapshot.bindings.at({7, {0}}).resource, Id(4));
            QCOMPARE(snapshot.bindings.at({30, {1}}).view, Id(12));
            const auto baseCount = event == 100 ? 0u : 1u;
            QCOMPARE(snapshot.inputs.memory->counters().at(12).value,
                     std::optional<uint32_t>(edited ? 2u : baseCount));
            QCOMPARE(replay.readCounter(12), baseCount);
            QCOMPARE(testing::firstWord(replay, 2), 2u);
            QCOMPARE(testing::firstWord(replay, 4), 7u);
            QCOMPARE(testing::firstWord(replay, 7), event == 100 ? 10u : 19u);
            execute(snapshot);
            configuration.before = false;
            Replay reference(frame, configuration);
            reference.run();
            const auto &output = snapshot.inputs.memory->buffers().at(7);
            QCOMPARE(output.bytes, reference.readBuffer(7));
            QCOMPARE(Reader(output.bytes).read<uint32_t>(), edited ? 64u : event == 100 ? 19u : 28u);
            QCOMPARE(snapshot.inputs.memory->counters().at(12).value,
                     std::optional<uint32_t>(reference.readCounter(12)));
            const auto &appended = snapshot.inputs.memory->buffers().at(10);
            const auto gpu = reference.readBuffer(10);
            for (size_t i = 0; i < gpu.size(); ++i)
                if (appended.valid[i])
                    QCOMPARE(appended.bytes[i], gpu[i]);
            QCOMPARE(replay.readCounter(12), baseCount);
            QCOMPARE(testing::firstWord(replay, 7), event == 100 ? 10u : 19u);
            validate(replay);
            validate(reference);
            evidence(snapshot, reference, edited ? "scoped-buffer-and-counter-edits" : "frame-inputs",
                     vendor);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void ranges_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("raw");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool raw : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(raw ? "raw" : "structured")))
                    << vendor << raw;
    }
    void ranges() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, raw);
        try {
            auto capture = testing::computeCapture(true);
            buffer(capture, 4, 5, 64, D3D11_BIND_SHADER_RESOURCE,
                   raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                   raw ? 0 : 4, 100);
            buffer(capture, 7, 8, 64, D3D11_BIND_UNORDERED_ACCESS,
                   raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                   raw ? 0 : 4, 200);
            std::erase_if(capture.entries, [](auto &e) { return e.id == 6 || e.id == 9; });
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = raw ? D3D11_SRV_DIMENSION_BUFFEREX : D3D11_SRV_DIMENSION_BUFFER;
            srv.BufferEx = {4, 8, raw ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0u};
            auto data = testing::statePack(Id(0), Id(0), Id(4), srv);
            capture.add(6, 5, 0x8c, data);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = srv.Format;
            uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uav.Buffer = {8, 4, raw ? D3D11_BUFFER_UAV_FLAG_RAW : 0u};
            capture.add(9, 5, 0x8f, testing::statePack(Id(0), Id(0), Id(7), uav));
            const auto shader =
                compileHlsl(
                    raw ? "ByteAddressBuffer src:register(t0);RWByteAddressBuffer "
                          "dst:register(u0);[numthreads(1,1,1)]void main(){dst.Store4(0,src.Load4(4));}"
                        : "StructuredBuffer<uint> src:register(t0);RWStructuredBuffer<uint> "
                          "dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=src[1];}",
                    "cs_5_0")
                    .bytecode;
            replaceShader(capture, shader);
            QTemporaryDir directory;
            const auto path = directory.path() + "/ranges.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            auto configuration = options(vendor);
            Replay replay(frame, configuration);
            const auto snapshot = snapshotBufferInputs(replay, 5);
            QCOMPARE(snapshot.bindings.at({7, {0}}).offset, uint64_t(16));
            QCOMPARE(snapshot.bindings.at({7, {0}}).size, uint64_t(32));
            QCOMPARE(snapshot.bindings.at({30, {0}}).offset, uint64_t(32));
            QCOMPARE(snapshot.bindings.at({30, {0}}).size, uint64_t(16));
            QCOMPARE(snapshot.bindings.at({7, {0}}).stride, raw ? 0u : 4u);
            execute(snapshot);
            configuration.before = false;
            Replay reference(frame, configuration);
            reference.run();
            QCOMPARE(snapshot.inputs.memory->buffers().at(7).bytes, reference.readBuffer(7));
            validate(replay);
            validate(reference);
            evidence(snapshot, reference, raw ? "raw-view-range" : "structured-view-range", vendor);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void constantWindows_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("first");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t first : {16u, 32u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(first))) << vendor << first;
    }
    void constantWindows() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, first);
        try {
            auto capture = testing::computeCapture(true);
            buffer(capture, 2, 3, 512, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 10);
            capture.add(
                95, 7, 0x254,
                testing::cbSetter(0x254, 0, {2}, std::vector<uint32_t>{first}, std::vector<uint32_t>{16}));
            QTemporaryDir directory;
            const auto path = directory.path() + "/window.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            auto configuration = options(vendor);
            Replay replay(frame, configuration);
            const auto snapshot = snapshotBufferInputs(replay, 5);
            QCOMPARE(snapshot.bindings.at({8, {0}}).offset, uint64_t(first) * 16);
            QCOMPARE(snapshot.bindings.at({8, {0}}).size, uint64_t(256));
            if (first == 16)
                QCOMPARE(snapshot.inputs.constantBuffers.at(0).at(0).bits[0], 74u);
            else
                QVERIFY(snapshot.inputs.constantBuffers.at(0).empty());
            execute(snapshot);
            configuration.before = false;
            Replay reference(frame, configuration);
            reference.run();
            QCOMPARE(snapshot.inputs.memory->buffers().at(7).bytes, reference.readBuffer(7));
            QCOMPARE(testing::firstWord(reference, 7), first == 16 ? 91u : 17u);
            validate(replay);
            validate(reference);
            evidence(snapshot, reference, "constant-window", vendor);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void descriptorAndShaderEdits() {
        auto capture = testing::computeCapture(true);
        QTemporaryDir directory;
        const auto path = directory.path() + "/descriptor.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        auto configuration = options(0);
        const auto shader = compileHlsl("StructuredBuffer<uint> src:register(t0);RWStructuredBuffer<uint> "
                                        "dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=src[0]+5;}",
                                        "cs_5_0")
                                .bytecode;
        configuration.shaders[15] = shader;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        desc.Buffer = {1, 2};
        configuration.srvEdits[100][{5, 0}] = desc;
        configuration.buffers[100][4] = {{4, testing::word(23)}};
        Replay replay(frame, configuration);
        const auto snapshot = snapshotBufferInputs(replay, 5);
        QCOMPARE(snapshot.bytecode, shader);
        QVERIFY(snapshot.bindings.at({7, {0}}).experimentalView);
        QCOMPARE(snapshot.bindings.at({7, {0}}).view, Id(6));
        QCOMPARE(snapshot.bindings.at({7, {0}}).offset, uint64_t(4));
        execute(snapshot);
        configuration.before = false;
        Replay reference(frame, configuration);
        reference.run();
        QCOMPARE(snapshot.inputs.memory->buffers().at(7).bytes, reference.readBuffer(7));
        QCOMPARE(testing::firstWord(reference, 7), 28u);
        validate(replay);
        validate(reference);
    }
    void cancellationBudgetAndRetry() {
        QTemporaryDir directory;
        const auto path = directory.path() + "/cancel.gpa_frame";
        testing::computeCapture(true).save(path);
        Frame frame(path.toStdWString());
        auto configuration = options(0);
        configuration.buffers[100][7] = {{0, testing::word(50)}};
        configuration.uavCounters[100][12] = 2;
        Replay replay(frame, configuration);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotBufferInputs(replay, 5, 63));
        QCOMPARE(replay.readCounter(12), 0u);
        QCOMPARE(testing::firstWord(replay, 7), 10u);
        unsigned checks = 0;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 snapshotBufferInputs(replay, 5, 64, [&] { return ++checks == 100; }));
        QCOMPARE(checks, 100u);
        QCOMPARE(replay.readCounter(12), 0u);
        QCOMPARE(testing::firstWord(replay, 7), 10u);
        const auto snapshot = snapshotBufferInputs(replay, 5, 64);
        execute(snapshot);
        QCOMPARE(Reader(snapshot.inputs.memory->buffers().at(7).bytes).read<uint32_t>(), 59u);
        QCOMPARE(replay.readCounter(12), 0u);
        QCOMPARE(testing::firstWord(replay, 7), 10u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotBufferInputs(replay, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotBufferInputs(replay, 6));
        configuration.before = false;
        Replay after(frame, configuration);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotBufferInputs(after, 5));
    }
    void graphicsStages() {
        QTemporaryDir directory;
        const auto path = directory.path() + "/graphics.gpa_frame";
        testing::graphicsCounterCapture(true).save(path);
        Frame frame(path.toStdWString());
        for (unsigned stage : {0u, 4u}) {
            auto configuration = options(0, 200);
            configuration.initialUavCounters[12] = 0;
            Replay replay(frame, configuration);
            const auto snapshot = snapshotBufferInputs(replay, stage);
            QCOMPARE(snapshot.stage, stage);
            QCOMPARE(snapshot.shader, stage == 0 ? Id(30) : Id(32));
            QVERIFY(
                std::ranges::equal(snapshot.bytecode, frame.shader(frame.resource(snapshot.shader).data)));
            QCOMPARE(snapshot.bindings.at({30, {1}}).view, Id(12));
            QCOMPARE(snapshot.inputs.memory->counters().at(12).value, std::optional<uint32_t>(0));
            validate(replay);
        }
    }
    void typedResourcesAreNotUnbound() {
        for (bool texture : {false, true}) {
            auto capture = testing::computeCapture(true);
            std::erase_if(capture.entries, [](auto &e) { return e.id == 6; });
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_UINT;
            if (texture) {
                capture.add(40, 5, 0x85,
                            testing::statePack(Id(0), Id(0),
                                               D3D11_TEXTURE2D_DESC{1,
                                                                    1,
                                                                    1,
                                                                    1,
                                                                    DXGI_FORMAT_R32_UINT,
                                                                    {1, 0},
                                                                    D3D11_USAGE_DEFAULT,
                                                                    D3D11_BIND_SHADER_RESOURCE,
                                                                    0,
                                                                    0},
                                               Id(41)));
                capture.add(41, 9, 1, testing::statePack(4u, 0x81234567u));
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipLevels = 1;
            } else {
                buffer(capture, 4, 5, 16, D3D11_BIND_SHADER_RESOURCE, 0, 0, 7);
                desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                desc.Buffer.NumElements = 4;
            }
            capture.add(6, 5, 0x8c, testing::statePack(Id(0), Id(0), Id(texture ? 40 : 4), desc));
            replaceShader(
                capture,
                compileHlsl(
                    texture ? "Texture2D<uint> src:register(t0);RWStructuredBuffer<uint> "
                              "dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=src.Load(int3(0,0,0));}"
                            : "Buffer<uint> src:register(t0);RWStructuredBuffer<uint> "
                              "dst:register(u0);[numthreads(1,1,1)]void main(){dst[0]=src[0];}",
                    "cs_5_0")
                    .bytecode);
            QTemporaryDir directory;
            const auto path = directory.path() + "/unsupported.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            const auto snapshot = snapshotBufferInputs(replay, 5);
            QCOMPARE(snapshot.bindings.at({7, {0}}).resource, Id(texture ? 40 : 4));
            QCOMPARE(snapshot.bindings.at({7, {0}}).view, Id(6));
            if (texture) {
                QVERIFY(!snapshot.unavailable.contains({7, {0}}));
                QVERIFY(snapshot.inputs.textures);
                QCOMPARE(snapshot.inputs.textures->views.at({7, {0}}).resource, std::optional<uint64_t>(40));
                QCOMPARE(snapshot.inputs.textures->resources.at(40).storage.bytes.size(), size_t(4));
                execute(snapshot);
                QCOMPARE(Reader(snapshot.inputs.memory->buffers().at(7).bytes).read<uint32_t>(), 0x81234567u);
                QVERIFY(!snapshot.inputs.textures->views.at({7, {1}}).resource);
            } else {
                QVERIFY(!snapshot.unavailable.contains({7, {0}}));
                QCOMPARE(snapshot.bindings.at({7, {0}}).format, uint32_t(DXGI_FORMAT_R32_UINT));
                execute(snapshot);
                const auto &bytes = snapshot.inputs.memory->buffers().at(7).bytes;
                QCOMPARE(Reader(bytes).read<uint32_t>(), 7u);
                auto configuration = options(0);
                configuration.before = false;
                Replay reference(frame, configuration);
                reference.run();
                QCOMPARE(bytes, reference.readBuffer(7));
                validate(reference);
            }
            QCOMPARE(snapshot.bindings.at({7, {1}}).resource, Id(0));
            QVERIFY(!snapshot.unavailable.contains({7, {1}}));
            validate(replay);
        }
    }
};
QTEST_GUILESS_MAIN(DxbcInputSnapshotTests)
#include "DxbcInputSnapshotTests.moc"
