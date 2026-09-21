#include "DepthStencilCapture.h"
#include "application/DrawParameters.h"
#include "application/Experiment.h"
#include "application/HlslCompilation.h"
#include "application/QuadDepth.h"
#include "application/QuadUavs.h"
#include "replay/Unpredicated.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
std::vector<uintptr_t> outputBindings(ID3D11DeviceContext *context) {
    std::vector<uintptr_t> result;
    auto append = [&](auto pointer) {
        result.push_back(reinterpret_cast<uintptr_t>(pointer));
        if (pointer)
            pointer->Release();
    };
    ID3D11RenderTargetView *rt[8]{};
    ID3D11DepthStencilView *depth{};
    context->OMGetRenderTargets(8, rt, &depth);
    for (auto value : rt)
        append(value);
    append(depth);
    Com<ID3D11Device> device;
    context->GetDevice(&device);
    ID3D11UnorderedAccessView *uav[64]{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, nullptr, 0, device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8, uav);
    for (auto value : uav)
        append(value);
    ID3D11Buffer *so[4]{};
    context->SOGetTargets(4, so);
    for (auto value : so)
        append(value);
    return result;
}
std::vector<std::vector<uintptr_t>> classes(ID3D11DeviceContext *context) {
    std::vector<std::vector<uintptr_t>> result;
    auto read = [&](auto method, auto shader) {
        ID3D11ClassInstance *instances[256]{};
        UINT count = 256;
        (context->*method)(&shader, instances, &count);
        if (shader)
            shader->Release();
        std::vector<uintptr_t> row;
        for (UINT i = 0; i < count; ++i) {
            row.push_back(reinterpret_cast<uintptr_t>(instances[i]));
            instances[i]->Release();
        }
        result.push_back(row);
    };
    read(&ID3D11DeviceContext::VSGetShader, static_cast<ID3D11VertexShader *>(nullptr));
    read(&ID3D11DeviceContext::HSGetShader, static_cast<ID3D11HullShader *>(nullptr));
    read(&ID3D11DeviceContext::DSGetShader, static_cast<ID3D11DomainShader *>(nullptr));
    read(&ID3D11DeviceContext::GSGetShader, static_cast<ID3D11GeometryShader *>(nullptr));
    read(&ID3D11DeviceContext::PSGetShader, static_cast<ID3D11PixelShader *>(nullptr));
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
        throw std::runtime_error("Unexpected resolved Quad draw");
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
        Experiment project(capture);
        project.load(QString::fromStdString(job.at("experiment").get<std::string>()), capture);
        project.apply(capture, options);
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
        const auto previousBindings = outputBindings(context);
        const auto previousClasses = classes(context);
        const auto previousCounts = replay.counts;
        try {
            result["writers"] = QuadUavs::writers(replay, state);
            QuadUavs helper(replay, state);
            result["metadata"] = helper.report();
            const auto direct = resolveDrawParameters(replay, event).direct;
            const auto target = resources.select(state);
            Com<ID3D11Device> device;
            context->GetDevice(&device);
            const auto code =
                compileHlsl("float4 main():SV_Target{return float4(.25,.5,.75,1);}", "ps_5_0").bytecode;
            Com<ID3D11PixelShader> marker;
            check(device->CreatePixelShader(code.data(), code.size(), nullptr, &marker),
                  "Create Quad test marker");
            std::array<Com<ID3D11Buffer>, 4> buffers;
            std::array<Com<ID3D11UnorderedAccessView>, 4> views;
            std::array<ID3D11UnorderedAccessView *, 4> pointers;
            Resource bufferDesc;
            bufferDesc.type = 0x83;
            bufferDesc.desc = {16, 0, 128, 0, 0, 0};
            for (UINT i = 0; i < 4; ++i) {
                D3D11_BUFFER_DESC desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
                const std::array<UINT, 4> seed{i + 11, i + 11, i + 11, i + 11};
                D3D11_SUBRESOURCE_DATA initial{seed.data(), 0, 0};
                check(device->CreateBuffer(&desc, &initial, &buffers[i]), "Create Quad test counter storage");
                D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R32_UINT;
                view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                view.Buffer.NumElements = 4;
                check(device->CreateUnorderedAccessView(buffers[i].Get(), &view, &views[i]),
                      "Create Quad test counter view");
                pointers[i] = views[i].Get();
            }
            result["passes"] = Json::array();
            for (bool preserve : {false, true, false, true}) {
                Json pass;
                helper.withPrivatePass(event, [&] {
                    pass["before"] = resources.fingerprint(state);
                    auto dummy = resources.dummy(target);
                    const float clear[4]{};
                    {
                        Unpredicated guard(context);
                        context->ClearRenderTargetView(dummy.view.Get(), clear);
                    }
                    Com<ID3D11DepthStencilView> depth;
                    context->OMGetRenderTargets(0, nullptr, &depth);
                    auto rt = dummy.view.Get();
                    context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rt, depth.Get(), 1, 4,
                                                                       pointers.data(), nullptr);
                    if (!preserve)
                        context->PSSetShader(marker.Get(), nullptr, 0);
                    helper.bind(preserve);
                    std::array<ID3D11UnorderedAccessView *, 4> actual{};
                    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 4,
                                                                       actual.data());
                    pass["reserved_bound"] = actual == pointers;
                    for (auto value : actual)
                        if (value)
                            value->Release();
                    auto now = classes(context), wanted = previousClasses;
                    if (!preserve) {
                        now.resize(4);
                        wanted.resize(4);
                    }
                    pass["classes_preserved"] = now == wanted;
                    pass["submissions"] = options.disabled.contains(id) ? 0 : draw(context, direct);
                    pass["after"] = resources.fingerprint(state);
                    pass["dummy_sha256"] = sha256(resources.storage(dummy.object.Get(), dummy.resource));
                    pass["reserved_sha256"] = Json::array();
                    for (const auto &buffer : buffers)
                        pass["reserved_sha256"].push_back(
                            sha256(resources.storage(buffer.Get(), bufferDesc)));
                    if (job.value("throw", false))
                        throw std::runtime_error("Injected private Quad pass failure");
                });
                result["passes"].push_back(pass);
            }
            QuadDepth depth(replay);
            auto prepared = depth.prepare(event, state, target, [&] {
                helper.bind(true);
                return options.disabled.contains(id) ? 0 : draw(context, direct);
            });
            result["prepared"] = prepared.metadata;
            result["prepared"]["color_and_uav_outputs_removed"] = false;
            result["prepared"]["private_uavs_preserved_for_pre_raster_stages"] = true;
        } catch (const std::exception &error) {
            result["error"] = error.what();
        }
        result["after"] = resources.fingerprint(state);
        result["bindings_restored"] = outputBindings(context) == previousBindings;
        result["classes_restored"] = classes(context) == previousClasses;
        result["counts_unchanged"] = replay.counts == previousCounts;
    });
    return result;
}
} // namespace
class QuadUavTests final : public QObject {
    Q_OBJECT
  private slots:
    void privatePasses() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        for (bool warp : {false, true})
            for (bool failure : {false, true}) {
                const auto result = run(
                    {{"capture", path.toStdString()}, {"event", 1000}, {"warp", warp}, {"throw", failure}});
                QCOMPARE(result.at("before"), result.at("after"));
                QCOMPARE(result.at("bindings_restored"), Json(true));
                QCOMPARE(result.at("classes_restored"), Json(true));
                QCOMPARE(result.at("counts_unchanged"), Json(true));
                QCOMPARE(result.contains("error"), failure);
                if (!failure) {
                    QCOMPARE(result.at("passes").size(), size_t(4));
                    QCOMPARE(result.at("passes")[0], result.at("passes")[2]);
                    QCOMPARE(result.at("passes")[1], result.at("passes")[3]);
                    for (const auto &pass : result.at("passes")) {
                        QCOMPARE(pass.at("reserved_bound"), Json(true));
                        QCOMPARE(pass.at("classes_preserved"), Json(true));
                    }
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
    QuadUavTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "QuadUavTests.moc"
