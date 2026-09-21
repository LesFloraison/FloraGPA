#include "DepthStencilCapture.h"
#include "application/Experiment.h"
#include "application/HlslCompilation.h"
#include "application/QuadFinal.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json snapshot(Replay &replay, const Json &job) {
    Json result{{"textures", Json::object()}, {"buffers", Json::object()}, {"counters", Json::object()}};
    for (auto id : job.value("textures", Json::array()))
        result["textures"][std::to_string(id.get<Id>())] = sha256(replay.readTexture(id.get<Id>()));
    for (auto id : job.value("buffers", Json::array()))
        result["buffers"][std::to_string(id.get<Id>())] = sha256(replay.readBuffer(id.get<Id>()));
    for (auto id : job.value("counters", Json::array()))
        result["counters"][std::to_string(id.get<Id>())] = replay.readCounter(id.get<Id>());
    if (job.contains("msaa")) {
        result["msaa"] = Json::object();
        for (const auto &item : job.at("msaa")) {
            Json samples = Json::array();
            const auto id = item.at("resource").get<Id>();
            for (uint32_t sample = 0; sample < item.at("samples").get<uint32_t>(); ++sample)
                samples.push_back(
                    sha256(replay.readMsaa(id, sample, item.at("format").get<uint32_t>()).bytes));
            result["msaa"][std::to_string(id)] = samples;
        }
    }
    return result;
}
Json run(const Json &job) {
    Frame capture(QString::fromStdString(job.at("capture").get<std::string>()).toStdWString());
    ReplayOptions options;
    options.until = job.value("event", Id(100));
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
        if (id != options.until || after)
            return;
        const auto event = frame.event(id);
        const auto state = effectiveBindings(frame, id, frame.state(event.state), options);
        result["before"] = snapshot(replay, job);
        try {
            QuadFinal helper(replay, event, state);
            result["after_capture"] = snapshot(replay, job);
            result["metadata"] = helper.metadata();
            result["signatures"] = helper.outputSignatures();
            Com<ID3D11Device> device;
            context->GetDevice(&device);
            const auto code = compileHlsl("float4 main(float4 p:SV_Position,uint "
                                          "id:SV_PrimitiveID):SV_Target{return float4(id+1,0,1,1);}",
                                          "ps_5_0")
                                  .bytecode;
            Com<ID3D11PixelShader> ps;
            check(device->CreatePixelShader(code.data(), code.size(), nullptr, &ps), "Create Quad test PS");
            context->PSSetShader(ps.Get(), nullptr, 0);
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
            Com<ID3D11DepthStencilState> ds;
            check(device->CreateDepthStencilState(&depth, &ds), "Create Quad test depth state");
            context->OMSetDepthStencilState(ds.Get(), 0);
            context->OMSetBlendState(nullptr, nullptr, UINT32_MAX);
            result["submissions"] = Json::array();
            result["passes"] = Json::array();
            for (unsigned i = 0; i < 2; ++i) {
                result["submissions"].push_back(helper.submit());
                result["passes"].push_back(snapshot(replay, job));
            }
        } catch (const std::exception &e) {
            result["error"] = e.what();
            result["after_failure"] = snapshot(replay, job);
        }
    });
    result["counts"] = replay.counts;
    return result;
}
} // namespace
class QuadFinalTests final : public QObject {
    Q_OBJECT
  private slots:
    void nativeTriangle() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        for (bool warp : {false, true}) {
            const auto result = run(
                {{"capture", path.toStdString()}, {"event", 1000}, {"warp", warp}, {"textures", {20, 720}}});
            QVERIFY2(!result.contains("error"), result.dump().c_str());
            QCOMPARE(result.at("before"), result.at("after_capture"));
            QCOMPARE(result.at("submissions"), Json::array({1, 1}));
            QCOMPARE(result.at("passes")[0], result.at("passes")[1]);
            QVERIFY(result.at("before").at("textures").at("20") !=
                    result.at("passes")[0].at("textures").at("20"));
            QCOMPARE(result.at("before").at("textures").at("720"),
                     result.at("passes")[0].at("textures").at("720"));
        }
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
    QuadFinalTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "QuadFinalTests.moc"
