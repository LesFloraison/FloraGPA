#include "TextureEditCapture.h"
#include "application/DxbcInputSnapshot.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <cmath>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::testing;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words out{code};
    for (const auto &arg : args)
        out.insert(out.end(), arg.begin(), arg.end());
    out[0] |= uint32_t(out.size()) << 24;
    return out;
}
Program load(uint32_t stage, uint32_t dimension, uint32_t returns, uint32_t mask = 15,
             std::array<int32_t, 3> offset = {}, bool feedback = false) {
    const auto target = (stage == 5 ? 0x100002u : 0x102002u) | (mask << 4);
    uint32_t control = 1;
    for (unsigned c = 0; c < 3; ++c)
        control |= (uint32_t(offset[c]) & 15) << (9 + c * 4);
    std::vector<Words> rows{op(89, {{0x208e46, 0, 2}}),
                            op(88 | (dimension << 11), {{0x1070f2, 0}, {returns * 0x1111}})};
    if (stage == 5) {
        rows.push_back(op(104, {{feedback ? 2u : 1u}}));
        rows.push_back(op(155, {{1, 1, 1}}));
    } else
        rows.push_back(op(101, {{target | 0xf0, 0}}));
    if (feedback && stage != 5)
        rows.push_back(op(104, {{2}}));
    rows.push_back(op(54, {{target | 0xf0, 0}, {0x4002, 7, 8, 9, 10}}));
    auto read =
        op(46 | 0x80000000, {{control}, {target, 0}, {0x208e46, 0, 0}, {0x1071b6, 0}, {0x20800a, 0, 1}});
    if (feedback) {
        read.insert(read.begin() + 4, {0x100012, 1});
        read[0] = (read[0] & ~(2047u | (127u << 24))) | 224u | (uint32_t(read.size()) << 24);
    }
    rows.push_back(read);
    rows.push_back(op(62));
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows})}}));
}
void shader(Capture &capture, Id id, const std::string &source, bool compute = false) {
    const auto bytes = compileHlsl(source, compute ? "cs_5_0" : "ps_5_0").bytecode;
    auto record = std::vector<uint8_t>(48);
    append(record, id + 1);
    capture.add(id, 5, compute ? 0x93 : 0x92, record);
    auto blob = statePack(uint64_t(bytes.size()));
    blob.insert(blob.end(), bytes.begin(), bytes.end());
    append(blob, Id(0));
    capture.add(id + 1, 9, 0x81, blob);
}
Capture fixture(uint32_t format, bool array, uint32_t width = 4, uint32_t height = 4,
                std::optional<float> minLod = {}, uint32_t samples = 4) {
    const bool rawFloat = format == 139;
    const bool depth = format == 121 || format == 122 || format == 146 || format == 147;
    const auto viewFormat = rawFloat ? 41u : depth ? format - 100 : format;
    const auto writeFormat = rawFloat ? 42u : depth ? (format < 130 ? 20u : 45u) : format;
    auto capture = textureEditCapture(viewFormat, 4, false);
    std::erase_if(capture.entries, [](auto &e) {
        return e.id == 20 || e.id == 22 || e.id == 23 || e.id == 24 || e.id == 42 || e.id == 43 ||
               e.id == 44 || e.id == 90 || e.id == 100 || e.id == 200 || e.id == 50;
    });
    const auto layers = array ? 3u : 1u;
    D3D11_TEXTURE2D_DESC texture{
        width,
        height,
        1,
        layers,
        DXGI_FORMAT(rawFloat ? 39u
                    : depth  ? (format < 130 ? 19u : 44u)
                             : format),
        {samples, 0},
        D3D11_USAGE_DEFAULT,
        UINT(D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET)),
        0,
        minLod ? D3D11_RESOURCE_MISC_RESOURCE_CLAMP : 0u};
    capture.add(20, 5, 0x85, statePack(Id(0), Id(0), texture, Id(0)));
    if (minLod)
        capture.add(45, 7, 0x3515, statePack(Id(0), Id(1), Id(20), 0.f));
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT(viewFormat);
    srv.ViewDimension = array ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
    if (array)
        srv.Texture2DMSArray = {1, 2};
    capture.add(22, 5, 0x8c, statePack(Id(0), Id(0), Id(20), srv));
    capture.add(23, 5, 0x8c, statePack(Id(0), Id(0), Id(20), srv));
    const auto readReturns = TextureInputs::loadReturnType(viewFormat);
    const std::string readType = readReturns == 3 ? "int4" : readReturns == 4 ? "uint4" : "float4";
    const std::string shape = array ? "Texture2DMSArray" : "Texture2DMS";
    const std::string point = array ? "int3(0,0,0)" : "int2(0,0)";
    shader(capture, 42,
           shape + "<" + readType + "> a:register(t0),b:register(t1);" + readType +
               " main():SV_Target{return a.Load(" + point + ",0)+b.Load(" + point + ",1);}");
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT(writeFormat);
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
    rtv.Texture2DMSArray = {0, layers};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT(writeFormat);
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
    dsv.Texture2DMSArray = {0, layers};
    if (depth) {
        capture.add(24, 5, 0x8e, statePack(Id(0), Id(0), Id(20), dsv));
        capture.add(50, 7, 0x31, statePack(Id(0), Id(1), Id(24), 3u, 0.f, uint8_t(0)));
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = ds.StencilEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ds.StencilReadMask = ds.StencilWriteMask = 255;
        ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                                      D3D11_COMPARISON_ALWAYS};
        capture.add(80, 5, 0x8b, statePack(Id(0), Id(0), ds));
    } else {
        capture.add(24, 5, 0x8d, statePack(Id(0), Id(0), Id(20), rtv));
        capture.add(50, 7, 0x32, statePack(Id(0), Id(1), Id(24), uint8_t(1), 0.f, 0.f, 0.f, 0.f));
    }
    capture.add(44, 9, 0x87, statePack(1u, 0.f, 0.f, float(width), float(height), 0.f, 1.f));
    const auto returns = depth ? 5u : TextureInputs::loadReturnType(writeFormat);
    const std::string type = returns == 3 ? "int4" : returns == 4 ? "uint4" : "float4";
    State state{};
    state.stages[0].shader = 40;
    state.viewports = 44;
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.rtCount = depth ? 0 : 1;
    state.depthState = depth ? 80 : 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        rtv.Texture2DMSArray = {layer, 1};
        dsv.Texture2DMSArray = {layer, 1};
        if (depth)
            capture.add(70 + layer, 5, 0x8e, statePack(Id(0), Id(0), Id(20), dsv));
        else
            capture.add(70 + layer, 5, 0x8d, statePack(Id(0), Id(0), Id(20), rtv));
        const auto value = "int n=int(p.x)+4*int(p.y)+16*int(s)+64*" + std::to_string(layer) + "+1;";
        const auto result =
            rawFloat       ? "uint4(s==0?uint(n):s==1?0x80000000u:s==2?0x7f800000u:0x7fc00000u|uint(n),0,0,0)"
            : returns == 3 ? "int4(-n,n,-n-1,n+1)"
                           : type + "(n,n+1,n+2,n+3)";
        shader(capture, 60 + layer * 2,
               depth ? "float main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Depth{" + value +
                           "return n/256.0;}"
                     : type + " main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{" + value +
                           "return " + result + (format == 28 ? "/255.0;" : ";") + "}");
        state.stages[4].shader = 60 + layer * 2;
        if (depth) {
            state.dsv = 70 + layer;
            state.stencilRef = 7 + layer;
        } else
            state.rtv[0] = 70 + layer;
        capture.add(90 + layer, 3, 3, snapshot(state));
        capture.add(100 + layer * 10, 7, 0x37, statePack(Id(90 + layer), Id(0), Id(1), 3u, 0u));
    }
    state.rtCount = 0;
    state.rtv[0] = 0;
    state.dsv = state.depthState = 0;
    state.stages[4].shader = 42;
    state.stages[4].srv[0] = 22;
    state.stages[4].srv[1] = 23;
    capture.add(96, 3, 3, snapshot(state));
    if (minLod)
        capture.add(180, 7, 0x3515, statePack(Id(0), Id(1), Id(20), *minLod));
    capture.add(200, 7, 0x37, statePack(Id(96), Id(0), Id(1), 0u, 0u));
    return capture;
}
ReplayOptions options(uint32_t vendor) {
    ReplayOptions config;
    config.warp = !vendor;
    if (vendor)
        config.vendor = vendor;
    config.debug = config.before = true;
    config.until = 200;
    return config;
}
void validate(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Read multisample validation");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read multisample diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read multisample diagnostic");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
std::vector<uintptr_t> bindings(Replay &replay) {
    std::vector<uintptr_t> result;
    replay.inspectNativeState([&](ID3D11DeviceContext *context, const auto &) {
        auto keep = [&](IUnknown *object) {
            result.push_back(reinterpret_cast<uintptr_t>(object));
            if (object)
                object->Release();
        };
        ID3D11ComputeShader *cs{};
        context->CSGetShader(&cs, nullptr, nullptr);
        keep(cs);
        ID3D11PixelShader *ps{};
        context->PSGetShader(&ps, nullptr, nullptr);
        keep(ps);
        ID3D11ShaderResourceView *srv[128]{};
        context->PSGetShaderResources(0, 128, srv);
        for (auto object : srv)
            keep(object);
        context->CSGetShaderResources(0, 128, srv);
        for (auto object : srv)
            keep(object);
        ID3D11UnorderedAccessView *uav[8]{};
        context->CSGetUnorderedAccessViews(0, 8, uav);
        for (auto object : uav)
            keep(object);
        ID3D11RenderTargetView *rtv[8]{};
        ID3D11DepthStencilView *dsv{};
        context->OMGetRenderTargets(8, rtv, &dsv);
        for (auto object : rtv)
            keep(object);
        keep(dsv);
        ID3D11Predicate *predicate{};
        BOOL value{};
        context->GetPredication(&predicate, &value);
        keep(predicate);
        result.push_back(value);
        ID3D11Buffer *so[4]{};
        context->SOGetTargets(4, so);
        for (auto object : so)
            keep(object);
    });
    return result;
}
std::array<uint32_t, 4> expected(uint32_t format, uint32_t x, uint32_t y, uint32_t layer, uint32_t sample) {
    const auto type = TextureInputs::loadReturnType(format);
    const auto n = x + 4 * y + 16 * sample + 64 * layer + 1;
    if (format == 121 || format == 146)
        return {std::bit_cast<uint32_t>(
                    format == 121 ? n / 256.f : float(std::round(double(n) / 256 * 16777215) / 16777215)),
                0, 0, 0x3f800000u};
    if (format == 122 || format == 147)
        return {0, 7 + layer, 0, 1};
    if (format == 139)
        return {sample == 0   ? n
                : sample == 1 ? 0x80000000u
                : sample == 2 ? 0x7f800000u
                              : 0x7fc00000u | n,
                0, 0, 0x3f800000u};
    std::array<uint32_t, 4> result;
    for (unsigned c = 0; c < 4; ++c)
        result[c] = type == 3   ? (c == 0   ? 0u - n
                                   : c == 1 ? n
                                   : c == 2 ? 0u - n - 1
                                            : n + 1)
                    : type == 4 ? n + c
                                : std::bit_cast<uint32_t>(float(n + c) / (format == 28 ? 255.f : 1.f));
    if (format >= 41 && format <= 43)
        result = {result[0], 0, 0, type == 5 ? 0x3f800000u : 1u};
    return result;
}
} // namespace
class DxbcMultisampleTests final : public QObject {
    Q_OBJECT
    std::map<uint32_t, uint32_t> stencilAlpha;
    std::map<uint32_t, uint64_t> probeAdapters;
  private slots:
    void initTestCase() {
        QTemporaryDir directory;
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u}) {
            for (const QString &lod : {QString(), QString("0"), QString("0.25"), QString("1")}) {
                const auto out =
                    directory.filePath(QString::number(vendor) + "-" + (lod.isEmpty() ? "default" : lod));
                QProcess process;
                QStringList arguments{out, QString::number(vendor)};
                if (!lod.isEmpty())
                    arguments.push_back(lod);
                process.start(QCoreApplication::applicationDirPath() + "/FloraMsaaStencilProbe.exe",
                              arguments);
                if (!process.waitForFinished(30000)) {
                    process.kill();
                    process.waitForFinished(5000);
                    QFAIL("Independent multisample probe timed out");
                }
                QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                         process.readAllStandardError().constData());
                QFile report(out + "/results.json");
                QVERIFY(report.open(QIODevice::ReadOnly));
                const auto evidence = nlohmann::json::parse(report.readAll().toStdString());
                QCOMPARE(evidence.at("vendor").get<uint32_t>(), vendor);
                QCOMPARE(evidence.at("validation_errors").get<unsigned>(), 0u);
                QCOMPARE(evidence.at("records").size(), size_t(40));
                QCOMPARE(evidence.at("resource_clamp").get<bool>(), !lod.isEmpty());
                QCOMPARE(evidence.at("requested_min_lod").get<float>(), lod.isEmpty() ? 0.f : lod.toFloat());
                const auto luid = evidence.at("adapter_luid").get<uint64_t>();
                if (probeAdapters.contains(vendor))
                    QCOMPARE(probeAdapters.at(vendor), luid);
                probeAdapters[vendor] = luid;
                if (vendor)
                    QCOMPARE(evidence.at("adapter_vendor").get<uint32_t>(), vendor);
                for (const auto &record : evidence.at("records")) {
                    const auto p = record.at("address").get<std::array<uint32_t, 4>>();
                    const auto bits = record.at("bits").get<std::array<uint32_t, 4>>();
                    const bool outside =
                        p[0] >= 4 || p[1] >= 4 || (record.at("array").get<bool>() && p[2] >= 2);
                    QCOMPARE(bits[0], 0u);
                    QCOMPARE(bits[1], outside || lod.toFloat() > 0 ? 0u : 90u);
                    QCOMPARE(bits[2], 0u);
                    QVERIFY(bits[3] <= 1);
                    if (stencilAlpha.contains(vendor))
                        QCOMPARE(bits[3], stencilAlpha.at(vendor));
                    else
                        stencilAlpha[vendor] = bits[3];
                }
                qInfo().noquote() << QString::fromStdString(evidence.dump());
            }
        }
    }
    void probeNativeComparison_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format : {139u, 147u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(format))) << vendor << format;
    }
    void probeNativeComparison() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        try {
            auto capture = fixture(format, true);
            std::erase_if(capture.entries, [](const auto &e) { return e.id == 96 || e.id == 200; });
            D3D11_BUFFER_DESC buffer{64,
                                     D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_UNORDERED_ACCESS,
                                     0,
                                     D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                     16};
            capture.add(30, 5, 0x83, statePack(Id(0), Id(0), buffer, Id(31)));
            auto data = word(64);
            data.resize(68);
            capture.add(31, 9, 1, data);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = 4;
            capture.add(32, 5, 0x8f, statePack(Id(0), Id(0), Id(30), uav));
            shader(capture, 82,
                   std::string("Texture2DMSArray<") + (format == 139 ? "float4" : "uint4") +
                       "> src:register(t0);RWStructuredBuffer<uint4> dst:register(u0);"
                       "[numthreads(4,1,1)]void main(uint3 t:SV_GroupThreadID){"
                       "dst[t.x]=asuint(src.Load(int3(1,2,1),t.x));}",
                   true);
            State state{};
            state.stages[5].shader = 82;
            state.stages[5].srv[0] = 22;
            state.csUav[0] = 32;
            state.csCount = 1;
            state.sampleMask = UINT32_MAX;
            capture.add(96, 3, 3, snapshot(state));
            capture.add(200, 7, 0x35, statePack(Id(96), Id(0), Id(1), 1u, 1u, 1u));
            QTemporaryDir directory;
            const auto path = directory.filePath("compute-samples.gpa_frame");
            const auto reportPath = directory.filePath("report.json");
            capture.save(path);
            QProcess process;
            process.start(QCoreApplication::applicationDirPath() + "/FloraDxbcInputProbe.exe",
                          {path, "200", "5", QString::number(vendor), reportPath, "--execute-group",
                           "--compare-native"});
            if (!process.waitForFinished(60000)) {
                process.kill();
                process.waitForFinished(5000);
                QFAIL("Native multisample comparison probe timed out");
            }
            QFile file(reportPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto report = nlohmann::json::parse(file.readAll().toStdString());
            QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                     report.dump().c_str());
            QCOMPARE(report.at("schema").get<unsigned>(), 6u);
            QVERIFY(report.at("observed").get<bool>());
            QCOMPARE(report.at("adapter_luid").get<uint64_t>(), probeAdapters.at(vendor));
            QCOMPARE(report.at("multisample_inputs").size(), size_t(1));
            QCOMPARE(report.at("multisample_inputs")[0].at("value_count").get<unsigned>(), 129u);
            const auto &execution = report.at("execution");
            QVERIFY(execution.at("completed").get<bool>());
            QVERIFY(execution.at("native_output_parity_verified").get<bool>());
            const auto &comparison = execution.at("native_comparison");
            QVERIFY(comparison.at("matching_native_inputs").get<bool>());
            QVERIFY(comparison.at("original_observed_resources_unchanged").get<bool>());
            QCOMPARE(comparison.at("multisample_inputs_checked").get<unsigned>(), 1u);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"probe_native_comparison", true},
                                {"vendor", vendor},
                                {"format", format},
                                {"adapter_luid", probeAdapters.at(vendor)},
                                {"multisample_inputs", report.at("multisample_inputs")},
                                {"native_comparison", comparison},
                                {"steps", execution.at("steps")},
                                {"passed", true}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void singleSampleViews_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("array");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool array : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(array))) << vendor << array;
    }
    void singleSampleViews() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, array);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("single-sample-ms-view.gpa_frame");
            fixture(42, array, 4, 4, {}, 1).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            const auto observed = snapshotResourceInputs(replay, 4);
            QCOMPARE(observed.adapterLuid, probeAdapters.at(vendor));
            QVERIFY(observed.inputs.textures->resources.empty());
            const auto &view = observed.inputs.textures->views.at({7, {0}});
            QVERIFY(view.samples);
            QVERIFY(view.samples == observed.inputs.textures->views.at({7, {1}}).samples);
            QCOMPARE(view.samples->values.size(), size_t((array ? 2 : 1) * 16 + 1));
            for (uint32_t layer = 0; layer < (array ? 2u : 1u); ++layer)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x)
                        QCOMPARE(view.samples->values[(layer * 4 + y) * 4 + x],
                                 expected(42, x, y, layer + (array ? 1 : 0), 0));
            QCOMPARE(view.samples->values.back(), (std::array<uint32_t, 4>{0, 0, 0, 1}));
            for (uint32_t stage : {0u, 1u, 5u}) {
                auto inputs = observed.inputs;
                inputs.constantBuffers[0] = {Value::known({1, 2, 0, 0}), Value::known({0, 0, 0, 0})};
                auto program = load(stage, array ? 9 : 4, 4);
                Lane lane(program, inputs);
                lane.step();
                lane.step();
                auto bits = expected(42, 1, 2, array ? 1 : 0, 0);
                std::reverse(bits.begin(), bits.end());
                const auto value = lane.value({stage == 5 ? 0u : 2u, {0}});
                QCOMPARE(value.bits, bits);
                QCOMPARE(value.valid, uint8_t(15));
            }
            const auto bytes = view.samples->values.size() * 16;
            const auto repeated = snapshotResourceInputs(replay, 4, bytes);
            QCOMPARE(repeated.inputs.textures->views.at({7, {0}}).samples->values, view.samples->values);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotResourceInputs(replay, 4, bytes - 1));
            validate(replay);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"single_sample_ms_view", true},
                                {"vendor", vendor},
                                {"array", array},
                                {"adapter_luid", observed.adapterLuid},
                                {"passed", true}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void dispatchRows_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void dispatchRows() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("large-samples.gpa_frame");
            fixture(42, false, 1024, 1024).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            const auto observed = snapshotResourceInputs(replay, 4);
            QCOMPARE(observed.adapterLuid, probeAdapters.at(vendor));
            const auto &values = observed.inputs.textures->views.at({7, {0}}).samples->values;
            QCOMPARE(values.size(), size_t(1024 * 1024 * 4 + 1));
            for (auto point : std::vector<std::array<uint32_t, 3>>{
                     {0, 0, 0}, {1007, 1023, 3}, {1008, 1023, 0}, {1023, 1023, 3}, {31, 1, 2}}) {
                const auto [x, y, sample] = point;
                QCOMPARE(values.at((y * 1024 + x) * 4 + sample), expected(42, x, y, 0, sample));
            }
            QCOMPARE(values.back(), (std::array<uint32_t, 4>{0, 0, 0, 1}));
            validate(replay);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"dispatch_rows", true},
                                {"vendor", vendor},
                                {"adapter_luid", observed.adapterLuid},
                                {"values", values.size()},
                                {"passed", true}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void resourceClamp_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<float>("lod");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (float lod : {0.f, .25f, 1.f})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(lod))) << vendor << lod;
    }
    void resourceClamp() {
        QFETCH(uint32_t, vendor);
        QFETCH(float, lod);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("clamped-samples.gpa_frame");
            fixture(147, true, 4, 4, lod).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            const auto observed = snapshotResourceInputs(replay, 4);
            QCOMPARE(observed.adapterLuid, probeAdapters.at(vendor));
            QCOMPARE(observed.inputs.textures->metadata.at(20).minLod, lod);
            const auto &view = observed.inputs.textures->views.at({7, {0}});
            for (size_t i = 0; i < view.samples->values.size(); ++i) {
                const auto stencil = lod > 0 || i == 128 ? 0u : 8u + uint32_t(i / 64);
                QCOMPARE(view.samples->values[i],
                         (std::array<uint32_t, 4>{0, stencil, 0, stencilAlpha.at(vendor)}));
            }
            QCOMPARE(replay.observeTextureMetadata(20).minLod, lod);
            bool assisted = false;
            QCOMPARE(observed.inputs.textures
                         ->loadMultisample({7, {0}}, 9, 0x4444, Value::known({0, 0, 1, 0}),
                                           Value::known({2, 0, 0, 0}), {0, 1, 2, 3}, 15, {}, assisted)
                         .bits,
                     view.samples->values[66]);
            validate(replay);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"resource_clamp_test", true},
                                {"vendor", vendor},
                                {"lod", lod},
                                {"adapter_luid", observed.adapterLuid},
                                {"passed", true}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void unavailableAndValidation() {
        TextureInputs textures;
        bool assisted = false;
        const Register binding{7, {0}};
        const auto address = Value::known({0, 0, 0, UINT32_MAX});
        const auto sample = Value::known({0, 0, 0, 0});
        const std::array<uint32_t, 4> swizzle{0, 1, 2, 3};
        QCOMPARE(
            textures.loadMultisample(binding, 4, 0x5555, address, sample, swizzle, 15, {}, assisted).valid,
            uint8_t(0));
        textures.views[binding] = {};
        QCOMPARE(
            textures.loadMultisample(binding, 4, 0x5555, address, sample, swizzle, 15, {}, assisted).valid,
            uint8_t(15));
        QVERIFY(!assisted);
        auto &view = textures.views[binding];
        view.resource = 20;
        view.format = 41;
        view.dimension = 6;
        view.nativeDescriptor = {41, 6, 0, 0, 0, 0};
        QCOMPARE(
            textures.loadMultisample(binding, 4, 0x5555, address, sample, swizzle, 15, {}, assisted).valid,
            uint8_t(0));
        auto samples = std::make_shared<TextureInputView::Samples>();
        samples->resource.id = 20;
        samples->resource.type = 0x85;
        samples->resource.desc = {1, 1, 1, 1, 41, 4, 0, 0, 8, 0, 0};
        samples->format = 41;
        samples->returnType = 5;
        samples->dimension = 6;
        samples->layerCount = 1;
        samples->values.resize(5, {1, 0, 0, 0x3f800000});
        view.samples = samples;
        auto partialAddress = address;
        partialAddress.valid = 3;
        auto partialSample = sample;
        partialSample.valid = 1;
        QCOMPARE(
            textures
                .loadMultisample(binding, 4, 0x5555, partialAddress, partialSample, swizzle, 15, {}, assisted)
                .valid,
            uint8_t(15));
        partialSample.valid = 0;
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            textures.loadMultisample(binding, 4, 0x5555, address, partialSample, swizzle, 15, {}, assisted));
        partialAddress.valid = 1;
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            textures.loadMultisample(binding, 4, 0x5555, partialAddress, sample, swizzle, 15, {}, assisted));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            textures.loadMultisample(binding, 4, 0x4444, address, sample, swizzle, 15, {}, assisted));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            textures.loadMultisample(binding, 9, 0x5555, address, sample, swizzle, 15, {}, assisted));
        samples->values.pop_back();
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            textures.loadMultisample(binding, 4, 0x5555, address, sample, swizzle, 15, {}, assisted));
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<bool>("array");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format : {2u, 3u, 4u, 10u, 28u, 41u, 42u, 43u, 139u, 121u, 122u, 146u, 147u})
                for (bool array : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(format).arg(array)))
                        << vendor << format << array;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        QFETCH(bool, array);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("samples.gpa_frame");
            fixture(format, array).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            const auto snapshot = snapshotResourceInputs(replay, 4);
            QCOMPARE(snapshot.adapterLuid, probeAdapters.at(vendor));
            const auto &view = snapshot.inputs.textures->views.at({7, {0}});
            QVERIFY(view.samples);
            QVERIFY(view.samples == snapshot.inputs.textures->views.at({7, {1}}).samples);
            QCOMPARE(view.samples->values.size(), size_t((array ? 2 : 1) * 4 * 4 * 4 + 1));
            const auto returns = TextureInputs::loadReturnType(view.format);
            std::array<uint32_t, 4> outside{0, 0, 0,
                                            view.format == 41 || view.format == 42 || view.format == 43 ||
                                                    view.format == 21 || view.format == 22 ||
                                                    view.format == 46 || view.format == 47
                                                ? (returns == 5 ? 0x3f800000u : 1u)
                                                : 0u};
            if (format == 122 || format == 147)
                outside[3] = stencilAlpha.at(vendor);
            QCOMPARE(view.samples->values.back(), outside);
            // The writer uses SV_SampleIndex and exact integer/half values, independent of the reader.
            for (uint32_t layer = 0; layer < (array ? 2u : 1u); ++layer)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x)
                        for (uint32_t sample = 0; sample < 4; ++sample) {
                            auto oracle = expected(format, x, y, layer + (array ? 1 : 0), sample);
                            if (format == 122 || format == 147)
                                oracle[3] = stencilAlpha.at(vendor);
                            const auto &actual =
                                view.samples->values.at(((layer * 4 + y) * 4 + x) * 4 + sample);
                            for (unsigned c = 0; c < 4; ++c)
                                if (format == 28 || format == 146)
                                    QVERIFY(std::abs(int64_t(actual[c]) - oracle[c]) <= 1);
                                else
                                    QCOMPARE(actual[c], oracle[c]);
                        }
            for (uint32_t stage : {0u, 1u, 5u})
                for (uint32_t sample = 0; sample < 5; ++sample)
                    for (uint32_t mode = 0; mode < 6; ++mode) {
                        auto inputs = snapshot.inputs;
                        auto address = std::array<uint32_t, 4>{1, 2, array ? 1u : 999u, UINT32_MAX};
                        if (mode == 1)
                            address[0] = 4;
                        if (mode == 2)
                            address[1] = UINT32_MAX;
                        if (mode == 3 && array)
                            address[2] = 2;
                        const auto offset = mode == 4   ? std::array<int32_t, 3>{-1, 1, 7}
                                            : mode == 5 ? std::array<int32_t, 3>{-8, 0, 0}
                                                        : std::array<int32_t, 3>{};
                        inputs.constantBuffers[0] = {Value::known(address),
                                                     Value::known({sample, 99, 99, 99})};
                        auto program = load(stage, array ? 9 : 4, returns, mode == 4 ? 5 : 15, offset);
                        Lane lane(program, inputs);
                        lane.step();
                        const auto step = lane.step();
                        const auto value = lane.value({stage == 5 ? 0u : 2u, {0}});
                        QVERIFY(view.nonTiled);
                        const auto feedbackProgram =
                            load(stage, array ? 9u : 4u, returns, mode == 4 ? 5u : 15u, offset, true);
                        Lane feedbackLane(feedbackProgram, inputs);
                        feedbackLane.step();
                        feedbackLane.step();
                        QCOMPARE(feedbackLane.value({stage == 5 ? 0u : 2u, {0}}), value);
                        const auto status = feedbackLane.value({0, {1}});
                        QCOMPARE(status.mapping[0], MappingStatus::Mapped);
                        QCOMPARE(status.valid & 1, 0);
                        const uint32_t mask = mode == 4 ? 5 : 15;
                        QCOMPARE(value.valid, uint8_t(sample == 4 ? 15 & ~mask : 15));
                        if (sample == 4)
                            continue;
                        QVERIFY(step.memoryGpuAssisted);
                        const auto x = address[0] + uint32_t(offset[0]);
                        const auto y = address[1] + uint32_t(offset[1]);
                        const auto layer = array ? address[2] : 0;
                        const auto expectedValue =
                            x >= 4 || y >= 4 || (array && layer >= 2)
                                ? view.samples->values.back()
                                : view.samples->values.at(((layer * 4 + y) * 4 + x) * 4 + sample);
                        for (unsigned c = 0; c < 4; ++c)
                            QCOMPARE(value.bits[c], mask & (1u << c) ? expectedValue[3 - c] : 7 + c);
                    }
            // Execute a compiler-generated cooperative group, not just isolated hand-encoded loads.
            auto groupInputs = snapshot.inputs;
            groupInputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{500, Buffer::known(std::vector<uint8_t>(64))}},
                std::map<Register, BufferView>{{{30, {0}}, {500, 0, 64, 16}}});
            const std::string type = returns == 3 ? "int4" : returns == 4 ? "uint4" : "float4";
            const std::string shape = array ? "Texture2DMSArray" : "Texture2DMS";
            const std::string point = array ? "int3(1,2,1)" : "int2(1,2)";
            const auto code =
                compileHlsl(shape + "<" + type +
                                "> src:register(t0);"
                                "RWStructuredBuffer<uint4> dst:register(u0);[numthreads(4,1,1)]"
                                "void main(uint3 t:SV_GroupThreadID){dst[t.x]=asuint(src.Load(" +
                                point + ",t.x));}",
                            "cs_5_0")
                    .bytecode;
            Program groupProgram(code);
            Group group(groupProgram, groupInputs);
            while (!group.finished())
                group.step();
            const auto &output = groupInputs.memory->buffers().at(500).bytes;
            for (uint32_t sample = 0; sample < 4; ++sample) {
                std::array<uint32_t, 4> bits{};
                std::memcpy(bits.data(), output.data() + sample * 16, 16);
                QCOMPARE(bits, view.samples->values.at((((array ? 1 : 0) * 4 + 2) * 4 + 1) * 4 + sample));
            }
            // Aliases share the budget; a new snapshot observes the unchanged source again.
            const auto bytes = view.samples->values.size() * 16;
            D3D11_SHADER_RESOURCE_VIEW_DESC descriptor{};
            std::memcpy(&descriptor, view.nativeDescriptor.data(), sizeof descriptor);
            const auto before = bindings(replay);
            unsigned checks = 0;
            QVERIFY_THROWS_EXCEPTION(
                OperationCancelled,
                replay.observeTextureSamples(20, descriptor, returns, bytes, [&] { return ++checks == 3; }));
            QCOMPARE(checks, 3u);
            QCOMPARE(bindings(replay), before);
            QCOMPARE(replay.observeTextureSamples(20, descriptor, returns, bytes), view.samples->values);
            QCOMPARE(bindings(replay), before);
            const auto repeated = snapshotResourceInputs(replay, 4, bytes);
            QCOMPARE(repeated.inputs.textures->views.at({7, {0}}).samples->values, view.samples->values);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotResourceInputs(replay, 4, bytes - 1));
            validate(replay);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"vendor", vendor},
                                {"format", format},
                                {"array", array},
                                {"adapter_luid", snapshot.adapterLuid},
                                {"passed", true},
                                {"values", view.samples->values.size()},
                                {"source", InputSnapshot::source}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcMultisampleTests)
#include "DxbcMultisampleTests.moc"
