#include "DepthStencilCapture.h"
#include "application/Experiment.h"
#include "application/QuadResources.h"
#include "replay/Unpredicated.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Resource descriptor(ID3D11Resource *source) {
    D3D11_RESOURCE_DIMENSION dimension{};
    source->GetType(&dimension);
    Resource result;
    result.type = uint16_t(0x82 + dimension);
    auto read = [&]<class T, class D>() {
        Com<T> object;
        check(source->QueryInterface(IID_PPV_ARGS(&object)), "Read dummy resource interface");
        D desc{};
        object->GetDesc(&desc);
        result.desc.resize(sizeof desc / 4);
        std::memcpy(result.desc.data(), &desc, sizeof desc);
    };
    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER:
        read.operator()<ID3D11Buffer, D3D11_BUFFER_DESC>();
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        read.operator()<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>();
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        read.operator()<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>();
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        read.operator()<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>();
        break;
    default:
        throw std::runtime_error("Unexpected dummy resource dimension");
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
        QuadResources resources(replay);
        result["before"] = resources.fingerprint(state);
        result["producer"] = resources.producer(state);
        if (job.value("unbind", false))
            context->OMSetRenderTargets(0, nullptr, nullptr);
        result["selections"] = Json::array();
        for (const auto &request : job.at("requests")) {
            Json row;
            try {
                std::optional<uint32_t> layer;
                if (!request.value("layer", Json()).is_null())
                    layer = request.at("layer").get<uint32_t>();
                const auto selected = resources.select(state, request.value("target", "auto"), layer);
                row = {{"selected", selected.selected},
                       {"metadata", selected.metadata},
                       {"size", {selected.width, selected.height}},
                       {"dummy", Json::array()}};
                for (bool prepared : {false, true}) {
                    auto dummy = resources.dummy(selected, prepared);
                    const auto native = descriptor(dummy.object.Get());
                    if (native.type != dummy.resource.type || native.desc != dummy.resource.desc)
                        throw std::runtime_error(
                            "Native dummy descriptor differs from its storage descriptor");
                    D3D11_RENDER_TARGET_VIEW_DESC view{};
                    dummy.view->GetDesc(&view);
                    std::array<uint32_t, 5> descriptor{};
                    std::memcpy(descriptor.data(), &view, sizeof view);
                    {
                        Unpredicated guard(context);
                        const float color[]{.25f, .5f, .75f, 1.f};
                        context->ClearRenderTargetView(dummy.view.Get(), color);
                    }
                    const auto data = resources.storage(dummy.object.Get(), dummy.resource);
                    row["dummy"].push_back({{"type", dummy.resource.type},
                                            {"desc", dummy.resource.desc},
                                            {"view", descriptor},
                                            {"bytes", data.size()},
                                            {"sha256", sha256(data)}});
                }
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            result["selections"].push_back(row);
        }
        result["after"] = resources.fingerprint(state);
    });
    return result;
}
} // namespace
class QuadResourceTests final : public QObject {
    Q_OBJECT
  private slots:
    void nativeTargets() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        for (bool warp : {false, true}) {
            const auto result =
                run({{"capture", path.toStdString()},
                     {"event", 1000},
                     {"warp", warp},
                     {"requests", {{{"target", "auto"}}, {{"target", "depth"}}, {{"target", "rt7"}}}}});
            QCOMPARE(result.at("before"), result.at("after"));
            QVERIFY(!result.at("selections")[0].contains("error"));
            QVERIFY(!result.at("selections")[1].contains("error"));
            QVERIFY(result.at("selections")[2].contains("error"));
            QCOMPARE(result.at("selections")[0].at("metadata").at("target_kind"), Json("color"));
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
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    QuadResourceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "QuadResourceTests.moc"
