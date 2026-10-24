#include "TextureEditCapture.h"
#include "application/DxbcInputSnapshot.h"
#include "application/HlslCompilation.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::testing;
using namespace flora::dxbc_exec;
namespace {
Resource description(unsigned shape) {
    Resource r{};
    r.type = shape == 0 ? 0x84 : shape == 2 ? 0x86 : 0x85;
    const uint32_t format = shape == 4 ? 71 : shape == 5 ? 27 : 28;
    const uint32_t bind = shape == 4 || shape == 3 ? 8 : 136;
    if (shape == 0)
        r.desc = {8, 3, 3, format, 0, bind, 0, 0};
    else if (shape == 2)
        r.desc = {8, 8, 4, 3, format, 0, bind, 0, 0};
    else
        r.desc = {8,
                  8,
                  3,
                  shape == 3 ? 12u : 3u,
                  format,
                  1,
                  0,
                  0,
                  bind,
                  0,
                  shape == 3 ? 4u : shape == 6 ? 0x80u : 0u};
    return r;
}
std::vector<uint8_t> initial(const Resource &r) {
    const auto subs = textureSubresources(r);
    std::vector<uint8_t> bytes(subs.back().offset + subs.back().size);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t(i * 37 + i / 17);
    return bytes;
}
Capture fixture(unsigned shape, bool writable) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    const auto r = description(shape);
    auto payload = statePack(Id(0), Id(0));
    for (auto word : r.desc)
        append(payload, word);
    append(payload, Id(21));
    c.add(20, 5, r.type, payload);
    auto bytes = initial(r);
    auto data = word(uint32_t(bytes.size()));
    data.insert(data.end(), bytes.begin(), bytes.end());
    c.add(21, 9, 1, data);
    const auto format = shape == 4 ? DXGI_FORMAT_BC1_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (writable) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = format;
        d.ViewDimension = shape == 0   ? D3D11_UAV_DIMENSION_TEXTURE1DARRAY
                          : shape == 2 ? D3D11_UAV_DIMENSION_TEXTURE3D
                                       : D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {1, 1, 1};
        c.add(22, 5, 0x8f, statePack(Id(0), Id(0), Id(20), d));
    } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = format;
        if (shape == 2) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            d.Texture3D = {1, 2};
        } else if (shape == 3) {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            d.TextureCubeArray = {1, 2, 6, 1};
        } else {
            d.ViewDimension =
                shape == 0 ? D3D11_SRV_DIMENSION_TEXTURE1DARRAY : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {1, 2, 1, 2};
        }
        c.add(22, 5, 0x8c, statePack(Id(0), Id(0), Id(20), d));
        c.add(23, 5, 0x8c, statePack(Id(0), Id(0), Id(20), d));
    }
    const std::string dimension = shape == 0   ? "Texture1DArray"
                                  : shape == 2 ? "Texture3D"
                                  : shape == 3 ? "TextureCubeArray"
                                               : "Texture2DArray";
    std::string source;
    if (writable)
        source = "RW" + dimension + "<float4> dst:register(u0);[numthreads(1,1,1)]void main(){dst[" +
                 (shape == 0 ? "uint2(0,0)" : "uint3(0,0,0)") + "]=float4(1,2,3,4);}";
    else {
        const std::string load = shape == 0   ? ".Load(int3(0,0,0))"
                                 : shape == 3 ? ".SampleLevel(s,float4(1,0,0,0),0)"
                                              : ".Load(int4(0,0,0,0))";
        source = dimension +
                 "<float4> a:register(t0),b:register(t1);SamplerState s:register(s0);"
                 "RWByteAddressBuffer dst:register(u0);[numthreads(1,1,1)]void main(){dst.Store4(0,asuint(a" +
                 load + "+b" + load + "));}";
        c.add(50, 5, 0x83,
              statePack(Id(0), Id(0),
                        D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                          D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0},
                        Id(51)));
        auto sink = word(16);
        sink.resize(20);
        c.add(51, 9, 1, sink);
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer = {0, 4, D3D11_BUFFER_UAV_FLAG_RAW};
        c.add(52, 5, 0x8f, statePack(Id(0), Id(0), Id(50), d));
    }
    const auto shader = compileHlsl(source, "cs_5_0").bytecode;
    auto shaderObject = std::vector<uint8_t>(48);
    append(shaderObject, Id(16));
    c.add(15, 5, 0x93, shaderObject);
    auto shaderData = statePack(uint64_t(shader.size()));
    shaderData.insert(shaderData.end(), shader.begin(), shader.end());
    append(shaderData, Id(0));
    c.add(16, 9, 0x81, shaderData);
    State state{};
    state.stages[5].shader = 15;
    if (writable)
        state.csUav[0] = 22;
    else {
        state.stages[5].srv[0] = 22;
        state.stages[5].srv[1] = 23;
        state.csUav[0] = 52;
    }
    if (shape == 6)
        c.add(80, 7, 0x3515, statePack(Id(0), Id(1), Id(20), 1.25f));
    c.add(90, 3, 3, snapshot(state));
    c.add(100, 7, 0x35, statePack(Id(90), Id(0), Id(1), 1u, 1u, 1u));
    return c;
}
void validate(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Read texture snapshot diagnostics");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read diagnostic");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
ReplayOptions options(uint32_t vendor) {
    ReplayOptions o;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.until = 100;
    o.before = true;
    o.debug = true;
    return o;
}
} // namespace
class DxbcTextureInputTests final : public QObject {
    Q_OBJECT
  private slots:
    void storage_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("shape");
        QTest::addColumn<bool>("writable");
        QTest::addColumn<bool>("edited");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (unsigned shape = 0; shape < 7; ++shape)
                for (bool writable : {false, true}) {
                    if (writable && (shape == 3 || shape == 4))
                        continue;
                    for (bool edited : {false, true})
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(shape).arg(writable).arg(edited)))
                            << vendor << shape << writable << edited;
                }
    }
    void storage() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, shape);
        QFETCH(bool, writable);
        QFETCH(bool, edited);
        QTemporaryDir directory;
        const auto path = directory.filePath("texture.gpa_frame");
        fixture(shape, writable).save(path);
        Frame frame(path.toStdWString());
        auto o = options(vendor);
        auto expected = initial(description(shape));
        if (edited) {
            auto layout = textureSubresources(description(shape));
            const auto &sub = layout.at(writable && shape != 2 ? 4 : 1);
            TexturePatch patch;
            patch.mip = sub.mip;
            patch.layer = sub.layer;
            patch.bytes.assign(sub.size, 0x6d);
            (writable ? o.textureOutputs : o.textureInputs)[100][20] = {patch};
            std::fill(expected.begin() + sub.offset, expected.begin() + sub.offset + sub.size, 0x6d);
        }
        Replay replay(frame, o);
        const auto budget = expected.size() + (writable ? 0 : 16);
        auto observed = snapshotResourceInputs(replay, 5, budget);
        if (vendor)
            QCOMPARE(observed.adapterVendor, vendor);
        QCOMPARE(observed.inputs.textures->resources.size(), size_t(1));
        const auto &texture = observed.inputs.textures->resources.at(20);
        QCOMPARE(texture.storage.bytes, expected);
        QCOMPARE(texture.storage.valid, std::vector<uint8_t>(expected.size(), 1));
        auto expectedDescription = description(shape).desc;
        if (edited && !writable)
            expectedDescription[expectedDescription.size() - 3] = D3D11_BIND_SHADER_RESOURCE;
        QCOMPARE(texture.resource.desc, expectedDescription);
        QCOMPARE(texture.minLod, shape == 6 ? 1.25f : 0.f);
        const Register reg{writable ? 30u : 7u, {0}};
        const auto &view = observed.inputs.textures->views.at(reg);
        QCOMPARE(view.resource, std::optional<uint64_t>(20));
        QCOMPARE(view.firstMip, 1u);
        QCOMPARE(view.mipCount, writable ? 1u : 2u);
        QCOMPARE(view.firstLayer, shape == 2 ? 0u : shape == 3 ? 6u : 1u);
        QCOMPARE(view.layerCount, shape == 2 ? 1u : shape == 3 ? 6u : writable ? 1u : 2u);
        if (writable && shape == 2)
            QCOMPARE(view.depthSlices, (std::optional<std::array<uint32_t, 2>>{{1, 1}}));
        if (!writable)
            QCOMPARE(observed.inputs.textures->views.at({7, {1}}).resource, view.resource);
        QVERIFY(!observed.inputs.textures->views.at({7, {127}}).resource);
        QVERIFY(!observed.unavailable.contains(reg));
        QCOMPARE(replay.readTexture(20), initial(description(shape)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotResourceInputs(replay, 5, budget - 1));
        QCOMPARE(replay.readTexture(20), initial(description(shape)));
        if (shape == 1 && edited) {
            unsigned checks = 0;
            QVERIFY_THROWS_EXCEPTION(
                OperationCancelled, replay.run({}, [&](Id, bool, ID3D11DeviceContext *, const auto &) {
                    replay.observeTextureStorage(20, expected.size(), [&] { return ++checks == 4; });
                }));
            QCOMPARE(checks, 4u);
            QCOMPARE(replay.readTexture(20), initial(description(shape)));
        }
        QCOMPARE(snapshotResourceInputs(replay, 5, budget).inputs.textures->resources.at(20).storage.bytes,
                 expected);
        validate(replay);
        qInfo().noquote() << QString::fromStdString(
            nlohmann::json({{"probe", "dxbc-texture-inputs"},
                            {"vendor", vendor},
                            {"adapter_vendor", observed.adapterVendor},
                            {"adapter_device", observed.adapterDevice},
                            {"adapter_luid", observed.adapterLuid},
                            {"shape", shape},
                            {"writable", writable},
                            {"edited", edited},
                            {"bytes", expected.size()},
                            {"frame_sha256", observed.frameSha256},
                            {"source", InputSnapshot::source},
                            {"passed", true}})
                .dump());
    }
    void multisampleObservation() {
        QTemporaryDir directory;
        const auto path = directory.filePath("msaa.gpa_frame");
        auto capture = textureEditCapture(28, 4, false);
        std::erase_if(capture.entries, [](auto &e) { return e.id == 43; });
        const auto shader = compileHlsl("Texture2DMSArray<float4> src:register(t0);"
                                        "float4 main():SV_Target{return src.Load(int3(0,0,0),0);}",
                                        "ps_5_0")
                                .bytecode;
        auto data = statePack(uint64_t(shader.size()));
        data.insert(data.end(), shader.begin(), shader.end());
        append(data, Id(0));
        capture.add(43, 9, 0x81, data);
        capture.save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(0));
        const auto observed = snapshotResourceInputs(replay, 4);
        QVERIFY(observed.inputs.textures->resources.empty());
        QCOMPARE(observed.inputs.textures->views.at({7, {0}}).resource, std::optional<uint64_t>(20));
        QVERIFY(!observed.unavailable.contains({7, {0}}));
        const auto &samples = observed.inputs.textures->views.at({7, {0}}).samples;
        QVERIFY(samples);
        QCOMPARE(samples->values.size(), size_t(4 * 4 * 2 * 4 + 1));
        for (const auto &value : samples->values)
            QCOMPARE(value, (std::array<uint32_t, 4>{}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotResourceInputs(replay, 4, 2048));
        Program program(observed.bytecode);
        Lane lane(program, observed.inputs);
        while (!lane.finished())
            lane.step();
        QCOMPARE(lane.value({2, {0}}).bits, (std::array<uint32_t, 4>{}));
        QCOMPARE(lane.value({2, {0}}).valid, uint8_t(15));
        QVERIFY(!observed.inputs.textures->views.at({7, {1}}).resource);
        validate(replay);
    }
    void nativeDescriptorAndRejectedImplicitMips() {
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("implicit-mips.gpa_frame");
            auto capture = fixture(1, false);
            std::erase_if(capture.entries, [](auto &e) { return e.id == 20; });
            auto resource = description(1);
            capture.add(2, 5, 0x38, std::vector<uint8_t>(24));
            auto data = statePack(Id(0), Id(2));
            for (auto value : resource.desc)
                append(data, value);
            append(data, Id(21));
            capture.add(20, 5, 0x85, data);
            capture.save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            const auto observed = snapshotResourceInputs(replay, 5, 1024);
            const auto &texture = observed.inputs.textures->resources.at(20);
            QCOMPARE(frame.resource(20).desc.at(4), 28u);
            QCOMPARE(texture.resource.desc.at(4), 27u);
            QCOMPARE(texture.storage.bytes, initial(resource));
            QCOMPARE(texture.subresources.size(), size_t(9));
            QCOMPARE(texture.subresources.back().mip, 2u);
            QCOMPARE(texture.subresources.back().layer, 2u);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotResourceInputs(replay, 5, 1023));
            validate(replay);
            // Lazy replay rejects implicit mip metadata before native observation.
            // Preserve that existing boundary instead of inventing missing layout.
            auto rejected = fixture(1, false);
            std::erase_if(rejected.entries, [](auto &e) { return e.id == 20; });
            resource.desc[2] = 0;
            data = statePack(Id(0), Id(0));
            for (auto value : resource.desc)
                append(data, value);
            append(data, Id(0));
            rejected.add(20, 5, 0x85, data);
            const auto invalidPath = directory.filePath("implicit-mips-rejected.gpa_frame");
            rejected.save(invalidPath);
            Frame invalidFrame(invalidPath.toStdWString());
            Replay invalidReplay(invalidFrame, options(0));
            bool rejectedMips = false;
            try {
                snapshotResourceInputs(invalidReplay, 5);
            } catch (const std::runtime_error &error) {
                rejectedMips = std::string(error.what()).find("Invalid mip/layer count") != std::string::npos;
            }
            QVERIFY(rejectedMips);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcTextureInputTests)
#include "DxbcTextureInputTests.moc"
