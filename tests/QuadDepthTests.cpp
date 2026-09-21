#include "DepthStencilCapture.h"
#include "application/DrawParameters.h"
#include "application/Experiment.h"
#include "application/QuadDepth.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
std::vector<uintptr_t> bindings(ID3D11DeviceContext *context) {
    std::vector<uintptr_t> result;
    auto append = [&](auto pointer) {
        result.push_back(reinterpret_cast<uintptr_t>(pointer));
        if (pointer)
            pointer->Release();
    };
    ID3D11RenderTargetView *rt[8]{};
    ID3D11DepthStencilView *ds{};
    context->OMGetRenderTargets(8, rt, &ds);
    for (auto value : rt)
        append(value);
    append(ds);
    Com<ID3D11Device> device;
    context->GetDevice(&device);
    ID3D11UnorderedAccessView *uav[64]{};
    const auto count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, uav);
    for (auto value : uav)
        append(value);
    ID3D11PixelShader *ps{};
    ID3D11ClassInstance *classes[256]{};
    UINT size = 256;
    context->PSGetShader(&ps, classes, &size);
    append(ps);
    for (UINT i = 0; i < size; ++i)
        append(classes[i]);
    ID3D11DepthStencilState *depth{};
    UINT reference{};
    context->OMGetDepthStencilState(&depth, &reference);
    append(depth);
    result.push_back(reference);
    ID3D11Buffer *so[4]{};
    context->SOGetTargets(4, so);
    for (auto value : so)
        append(value);
    return result;
}
uint64_t draw(ID3D11DeviceContext *context, const Event &event) {
    const auto &a = event.args;
    switch (event.type) {
    case 0x37:
        context->Draw(a[0], a[1]);
        break;
    case 0x39:
        context->DrawIndexed(a[0], a[1], int32_t(a[2]));
        break;
    case 0x3a:
        context->DrawIndexedInstanced(a[0], a[1], a[2], int32_t(a[3]), a[4]);
        break;
    case 0x3c:
        context->DrawInstanced(a[0], a[1], a[2], a[3]);
        break;
    default:
        throw std::runtime_error("Unexpected resolved draw");
    }
    return 1;
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
        QuadResources resources(replay);
        result["before"] = resources.fingerprint(state);
        const auto original = bindings(context);
        const auto counts = replay.counts;
        try {
            auto target = resources.select(state, job.value("target", "auto"));
            if (job.value("mismatch", false))
                ++target.width;
            const auto direct = resolveDrawParameters(replay, event).direct;
            QuadDepth depth(replay);
            auto prepared = depth.prepare(
                event, state, target,
                [&] {
                    if (job.value("throw", false))
                        throw std::runtime_error("Injected Quad submit failure");
                    if (options.disabled.contains(id))
                        return uint64_t(0);
                    if (job.value("suspend_so", false))
                        depth.suspendStreamOutput();
                    return draw(context, direct);
                },
                job.value("copy_so", true));
            result["metadata"] = prepared.metadata;
            result["digest"] = prepared.digest(replay);
            result["digest_again"] = prepared.digest(replay);
        } catch (const std::exception &error) {
            result["error"] = error.what();
        }
        result["bindings_restored"] = bindings(context) == original;
        result["counts_unchanged"] = replay.counts == counts;
        result["after"] = resources.fingerprint(state);
    });
    return result;
}
} // namespace
class QuadDepthTests final : public QObject {
    Q_OBJECT
  private slots:
    void isolatedDepth() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        for (bool warp : {false, true})
            for (const auto &mode : {std::string("normal"), std::string("mismatch"), std::string("throw")}) {
                const auto result =
                    run({{"capture", path.toStdString()}, {"event", 1000}, {"warp", warp}, {mode, true}});
                QCOMPARE(result.at("before"), result.at("after"));
                QCOMPARE(result.at("bindings_restored"), Json(true));
                QCOMPARE(result.at("counts_unchanged"), Json(true));
                QCOMPARE(result.contains("error"), mode != "normal");
                if (mode == "normal") {
                    QCOMPARE(result.at("digest"), result.at("digest_again"));
                    QVERIFY(result.at("metadata").at("cleared_sha256") != result.at("digest"));
                }
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
        Json result = Json::array();
        for (const auto &job : Json::parse(input.readAll().toStdString()))
            try {
                result.push_back(run(job));
            } catch (const std::exception &error) {
                result.push_back({{"error", error.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    QuadDepthTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "QuadDepthTests.moc"
