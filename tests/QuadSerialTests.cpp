#include "DepthStencilCapture.h"
#include "application/Experiment.h"
#include "application/QuadSerial.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json bindings(ID3D11DeviceContext *context) {
    std::array<ID3D11Buffer *, 32> buffers{};
    std::array<UINT, 32> strides{}, offsets{};
    context->IAGetVertexBuffers(0, 32, buffers.data(), strides.data(), offsets.data());
    Com<ID3D11Buffer> index;
    DXGI_FORMAT format;
    UINT offset;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    context->IAGetIndexBuffer(&index, &format, &offset);
    context->IAGetPrimitiveTopology(&topology);
    Json pointers = Json::array();
    for (auto buffer : buffers) {
        pointers.push_back(uintptr_t(buffer));
        if (buffer)
            buffer->Release();
    }
    return {uintptr_t(index.Get()), uint32_t(format), offset, uint32_t(topology), pointers, strides, offsets};
}
Json run(const Json &job) {
    if (job.value("action", "") == "expand") {
        const auto values = job.at("values").get<std::vector<uint32_t>>();
        std::optional<uint32_t> cut;
        if (!job.value("cut", Json()).is_null())
            cut = job.at("cut").get<uint32_t>();
        const auto strip = expandQuadStrip(values, cut, job.value("factor", 3u));
        return {{"indices", strip.indices}, {"metadata", strip.metadata}};
    }
    Frame capture(QString::fromStdString(job.at("capture").get<std::string>()).toStdWString());
    ReplayOptions options;
    const Id eventId = job.value("event", Id(100));
    options.until = eventId;
    options.before = true;
    options.warp = job.value("warp", false);
    if (job.contains("experiment")) {
        Experiment experiment(capture);
        experiment.load(QString::fromStdString(job.at("experiment").get<std::string>()), capture);
        experiment.apply(capture, options);
    }
    const auto &frame = effectiveFrame(capture, options);
    Replay replay(frame, options);
    Json result;
    replay.run({}, [&](Id id, bool after, ID3D11DeviceContext *context, const auto &) {
        if (id != eventId || after)
            return;
        const auto event = frame.event(eventId);
        auto state = effectiveBindings(frame, id, frame.state(event.state), options);
        if (job.contains("topology_override"))
            state.topology = job.at("topology_override").get<uint32_t>();
        const auto before = bindings(context);
        try {
            QuadSerial serial(replay, event, state);
            result["submissions"] = serial.submit();
            result["metadata"] = serial.metadata();
        } catch (const std::exception &e) {
            result["error"] = e.what();
        }
        result["bindings_restored"] = before == bindings(context);
        result["textures"] = Json::object();
        for (auto texture : job.value("textures", Json::array()))
            result["textures"][std::to_string(texture.get<Id>())] =
                sha256(replay.readTexture(texture.get<Id>()));
        result["buffers"] = Json::object();
        for (auto buffer : job.value("buffers", Json::array()))
            result["buffers"][std::to_string(buffer.get<Id>())] = sha256(replay.readBuffer(buffer.get<Id>()));
    });
    result["counts"] = replay.counts;
    return result;
}
} // namespace
class QuadSerialTests final : public QObject {
    Q_OBJECT
  private slots:
    void preservesNativeTriangle() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        for (bool warp : {false, true}) {
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.until = 1000;
            options.warp = warp;
            Replay original(frame, options);
            original.run();
            const auto result = run(
                {{"capture", path.toStdString()}, {"event", 1000}, {"warp", warp}, {"textures", {20, 720}}});
            QVERIFY2(!result.contains("error"), result.dump().c_str());
            QVERIFY(result.at("bindings_restored").get<bool>());
            QCOMPARE(result.at("submissions"), Json(1));
            QCOMPARE(result.at("textures").at("20"), Json(sha256(original.readTexture(20))));
            QCOMPARE(result.at("textures").at("720"), Json(sha256(original.readTexture(720))));
            const auto rejected = run({{"capture", path.toStdString()},
                                       {"event", 1000},
                                       {"warp", warp},
                                       {"topology_override", 12}});
            QVERIFY(rejected.contains("error"));
            QVERIFY(rejected.at("bindings_restored").get<bool>());
        }
    }
    void stripCornerOrder() {
        const std::vector<uint32_t> values{9, 0, 1, 1, 2, 9, 3, 4, 5, 6};
        const auto strip = expandQuadStrip(values, 9, 3);
        QCOMPARE(strip.indices, (std::vector<uint32_t>{0, 1, 1, 1, 2, 1, 3, 4, 5, 4, 6, 5}));
        QCOMPARE(strip.metadata.at("restart_markers"), Json(2));
        QCOMPARE(strip.metadata.at("degenerate_triangles"), Json(2));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        Json results = Json::array();
        for (const auto &job : Json::parse(input.readAll().toStdString()))
            try {
                results.push_back(run(job));
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(results.dump(2)));
        return 0;
    }
    QuadSerialTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "QuadSerialTests.moc"
