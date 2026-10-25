#include "TextureEditCapture.h"
#include "application/DxbcInputSnapshot.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::testing;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
using Words = std::vector<uint32_t>;
Words row(uint32_t opcode, std::initializer_list<Words> args) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Program program(uint32_t stage, uint32_t dimension, uint32_t opcode, bool integer, uint32_t mask = 15) {
    const uint32_t kind = dimension ? 7 : 14;
    const auto destination = 0x100002u | ((stage == 5 ? 0u : 2u) << 12) | (mask << 4);
    std::vector<Words> rows;
    if (dimension)
        rows.push_back(row(88 | (dimension << 11), {{0x1070f2, 0}, {0x5555}}));
    rows.push_back(row(89, {{0x208e46, 0, 1}}));
    if (stage == 5) {
        rows.push_back(row(104, {{1}}));
        rows.push_back(row(155, {{1, 1, 1}}));
    } else
        rows.push_back(row(101, {{destination | 0xf0, 0}}));
    rows.push_back(row(54, {{destination | 0xf0, 0}, {0x4002, 7, 8, 9, 10}}));
    Words resource{(dimension ? 0x100000u : 0u) | (kind << 12) | 0x1b6};
    if (dimension)
        resource.push_back(0);
    rows.push_back(opcode == 111 ? row(opcode | (integer ? 0x800 : 0), {{destination, 0}, resource})
                                 : row(opcode, {{destination, 0}, resource, {0x20800a, 0, 0}}));
    rows.push_back(row(62, {}));
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows})}}));
}
Capture fixture(uint32_t count, uint32_t quality, unsigned mode, bool multisample) {
    auto c = textureEditCapture(41, 4, false);
    std::erase_if(c.entries, [](auto &e) {
        return e.id == 20 || e.id == 22 || e.id == 23 || e.id == 24 || e.id == 42 || e.id == 43 ||
               e.id == 50 || e.id == 90 || e.id == 100 || e.id == 200;
    });
    const bool array = mode == 1;
    D3D11_TEXTURE2D_DESC td{4,
                            4,
                            1,
                            array ? 2u : 1u,
                            DXGI_FORMAT_R32_FLOAT,
                            {count, quality},
                            D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                            0,
                            0};
    c.add(20, 5, 0x85, statePack(Id(0), Id(0), td, Id(0)));
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = td.Format;
    srv.ViewDimension = array ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
    if (array)
        srv.Texture2DMSArray = {1, 1};
    c.add(22, 5, 0x8c, statePack(Id(0), Id(0), Id(20), srv));
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = td.Format;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
    rtv.Texture2DMSArray = {0, td.ArraySize};
    c.add(24, 5, 0x8d, statePack(Id(0), Id(0), Id(20), rtv));
    c.add(50, 7, 0x32, statePack(Id(0), Id(1), Id(24), uint8_t(1), .25f, 0.f, 0.f, 0.f));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = multisample;
    c.add(80, 5, 0x89, statePack(Id(0), Id(0), rd));
    const std::string resource = mode >= 2 && mode != 4 ? ""
                                 : array                ? "Texture2DMSArray<float> t:register(t0);"
                                                        : "Texture2DMS<float> t:register(t0);";
    const std::string query =
        mode >= 2 && mode != 4 ? "n=GetRenderTargetSampleCount();float2 p=GetRenderTargetSamplePosition(0);"
        : array                ? "t.GetDimensions(w,h,l,n);float2 p=t.GetSamplePosition(0);"
                               : "t.GetDimensions(w,h,n);float2 p=t.GetSamplePosition(0);";
    const auto shader = compileHlsl(resource + "uint4 main():SV_Target{uint w,h,l,n;" + query +
                                        "return uint4(n,asuint(p),0);}",
                                    "ps_5_0")
                            .bytecode;
    auto object = std::vector<uint8_t>(48);
    append(object, Id(43));
    c.add(42, 5, 0x92, object);
    auto bytes = statePack(uint64_t(shader.size()));
    bytes.insert(bytes.end(), shader.begin(), shader.end());
    append(bytes, Id(0));
    c.add(43, 9, 0x81, bytes);
    State state{};
    state.stages[0].shader = 40;
    state.stages[4].shader = 42;
    if (mode < 2)
        state.stages[4].srv[0] = 22;
    state.rasterizer = 80;
    state.viewports = 44;
    state.topology = 4;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    if (mode == 2) {
        state.rtCount = 1;
        state.rtv[0] = 24;
    }
    c.add(90, 3, 3, snapshot(state));
    c.add(200, 7, 0x37, statePack(Id(90), Id(0), Id(1), 0u, 0u));
    return c;
}
void validate(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Read sample query diagnostics");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        check(queue->GetMessage(i, nullptr, &size), "Read sample diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read sample diagnostic");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
std::vector<uintptr_t> bindings(ID3D11DeviceContext *context) {
    std::vector<uintptr_t> result;
    auto keep = [&](IUnknown *object) {
        result.push_back(reinterpret_cast<uintptr_t>(object));
        if (object)
            object->Release();
    };
    ID3D11VertexShader *vs{};
    context->VSGetShader(&vs, nullptr, nullptr);
    keep(vs);
    ID3D11PixelShader *ps{};
    context->PSGetShader(&ps, nullptr, nullptr);
    keep(ps);
    ID3D11ComputeShader *cs{};
    context->CSGetShader(&cs, nullptr, nullptr);
    keep(cs);
    ID3D11ShaderResourceView *srv[128]{};
    context->PSGetShaderResources(0, 128, srv);
    for (auto value : srv)
        keep(value);
    ID3D11UnorderedAccessView *uav[8]{};
    context->CSGetUnorderedAccessViews(0, 8, uav);
    for (auto value : uav)
        keep(value);
    ID3D11RenderTargetView *rtv[8]{};
    ID3D11DepthStencilView *dsv{};
    context->OMGetRenderTargets(8, rtv, &dsv);
    for (auto value : rtv)
        keep(value);
    keep(dsv);
    ID3D11Predicate *predicate{};
    BOOL predicateValue{};
    context->GetPredication(&predicate, &predicateValue);
    keep(predicate);
    result.push_back(predicateValue);
    ID3D11RasterizerState *rs{};
    context->RSGetState(&rs);
    keep(rs);
    ID3D11DepthStencilState *ds{};
    UINT stencil{};
    context->OMGetDepthStencilState(&ds, &stencil);
    keep(ds);
    result.push_back(stencil);
    ID3D11Buffer *so[4]{};
    context->SOGetTargets(4, so);
    for (auto value : so)
        keep(value);
    return result;
}
uint32_t counter(Replay &replay, ID3D11DeviceContext *context, ID3D11UnorderedAccessView *uav) {
    D3D11_BUFFER_DESC desc{4, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
    Com<ID3D11Buffer> output, staging;
    check(replay.nativeDevice()->CreateBuffer(&desc, nullptr, &output), "Create counter observation");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(replay.nativeDevice()->CreateBuffer(&desc, nullptr, &staging), "Create counter staging");
    context->CopyStructureCount(output.Get(), 0, uav);
    context->CopyResource(staging.Get(), output.Get());
    context->Flush();
    QElapsedTimer timer;
    timer.start();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    for (;;) {
        const auto hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == S_OK)
            break;
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
            check(hr, "Read counter observation");
        if (timer.elapsed() > 5000)
            throw std::runtime_error("Counter observation timed out");
        QThread::msleep(1);
    }
    uint32_t result{};
    std::memcpy(&result, mapped.pData, 4);
    context->Unmap(staging.Get(), 0);
    return result;
}
} // namespace
class DxbcSampleQueryTests final : public QObject {
    Q_OBJECT
    std::map<std::pair<uint32_t, bool>, Json> references;
  private slots:
    void nativeComparison_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned mode : {0u, 1u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(mode))) << vendor << mode;
    }
    void nativeComparison() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, mode);
        try {
            auto capture = fixture(4, 0, mode, false);
            std::erase_if(capture.entries,
                          [](const auto &entry) { return entry.id == 90 || entry.id == 200; });
            const D3D11_BUFFER_DESC bd{64,
                                       D3D11_USAGE_DEFAULT,
                                       D3D11_BIND_UNORDERED_ACCESS,
                                       0,
                                       D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                       16};
            capture.add(30, 5, 0x83, statePack(Id(0), Id(0), bd, Id(31)));
            auto data = word(64);
            data.resize(68);
            capture.add(31, 9, 1, data);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 4;
            capture.add(32, 5, 0x8f, statePack(Id(0), Id(0), Id(30), ud));
            const std::string declaration =
                mode ? "Texture2DMSArray<float> t:register(t0);" : "Texture2DMS<float> t:register(t0);";
            const auto shader =
                compileHlsl(declaration +
                                "RWStructuredBuffer<uint4> dst:register(u0);[numthreads(4,1,1)]"
                                "void main(uint3 thread:SV_GroupThreadID){uint w,h,l,n;" +
                                (mode ? "t.GetDimensions(w,h,l,n);" : "t.GetDimensions(w,h,n);") +
                                "dst[thread.x]=uint4(n,asuint(t.GetSamplePosition(thread.x)),0);}",
                            "cs_5_0")
                    .bytecode;
            auto object = std::vector<uint8_t>(48);
            append(object, Id(83));
            capture.add(82, 5, 0x93, object);
            auto bytes = statePack(uint64_t(shader.size()));
            bytes.insert(bytes.end(), shader.begin(), shader.end());
            append(bytes, Id(0));
            capture.add(83, 9, 0x81, bytes);
            State state{};
            state.stages[5].shader = 82;
            state.stages[5].srv[0] = 22;
            state.csUav[0] = 32;
            state.csCount = 1;
            state.sampleMask = UINT32_MAX;
            capture.add(90, 3, 3, snapshot(state));
            capture.add(200, 7, 0x35, statePack(Id(90), Id(0), Id(1), 1u, 1u, 1u));
            QTemporaryDir directory;
            const auto path = directory.filePath("native-query.gpa_frame");
            const auto reportPath = directory.filePath("report.json");
            capture.save(path);
            QProcess process;
            process.start(QCoreApplication::applicationDirPath() + "/FloraDxbcInputProbe.exe",
                          {path, "200", "5", QString::number(vendor), reportPath, "--execute-group",
                           "--compare-native"});
            if (!process.waitForFinished(60000)) {
                process.kill();
                process.waitForFinished(5000);
                QFAIL("Native query comparison timed out");
            }
            QFile file(reportPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto report = Json::parse(file.readAll().toStdString());
            QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                     report.dump().c_str());
            QCOMPARE(report.at("schema").get<unsigned>(), 6u);
            QCOMPARE(report.at("source").get<std::string>(), std::string(InputSnapshot::source));
            QCOMPARE(report.at("adapter_luid"), references.at({vendor, false}).at("adapter_luid"));
            const auto &queries = report.at("sample_queries");
            QCOMPARE(queries.size(), size_t(1));
            QVERIFY(queries[0].at("observation").at("observed").get<bool>());
            QCOMPARE(queries[0].at("observation").at("count").get<unsigned>(), 4u);
            const auto &execution = report.at("execution");
            QVERIFY(execution.at("completed").get<bool>());
            QVERIFY(execution.at("native_output_parity_verified").get<bool>());
            const auto &comparison = execution.at("native_comparison");
            QVERIFY(comparison.at("matching_native_inputs").get<bool>());
            QVERIFY(comparison.at("original_observed_resources_unchanged").get<bool>());
            QCOMPARE(comparison.at("sample_queries_checked").get<unsigned>(), 1u);
            qInfo().noquote() << QString::fromStdString(Json{
                {"native_query_comparison", true},
                {"vendor", vendor},
                {"mode", mode},
                {"sample_queries", queries},
                {"native_comparison", comparison},
                {"passed", true}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void unknownAndInvalid() {
        TextureInputs textures;
        const Register binding{7, {0}};
        bool assisted = true;
        QCOMPARE(textures.sampleQuery(binding, 4, true, false, {}, assisted).valid, uint8_t(0));
        QVERIFY(!assisted);
        textures.views[binding] = {};
        QCOMPARE(textures.sampleQuery(binding, 4, true, false, {}, assisted), Value::known({}));
        auto &view = textures.views[binding];
        view.resource = 123;
        view.dimension = 6;
        QCOMPARE(textures.sampleQuery(binding, 4, true, false, {}, assisted).valid, uint8_t(0));
        auto query = std::make_shared<SampleQueryInput>();
        view.queries = query;
        QCOMPARE(textures.sampleQuery(binding, 4, true, false, {}, assisted).valid, uint8_t(12));
        QVERIFY(!assisted);
        query->count = 1;
        query->positions = {{0, 0, 0, 0}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 textures.sampleQuery(binding, 4, true, false, {}, assisted));
        query->observed = true;
        QCOMPARE(textures.sampleQuery(binding, 4, true, false, {}, assisted).valid, uint8_t(12));
        QVERIFY(assisted);
        QCOMPARE(textures.sampleQuery(binding, 4, true, false, Value::known({UINT32_MAX, 0, 0, 0}), assisted),
                 Value::known({}));
        query->positions[0][2] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 textures.sampleQuery(binding, 4, true, false, {}, assisted));
        query->positions[0][2] = 0;
        query->count = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 textures.sampleQuery(binding, 4, false, true, {}, assisted));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 textures.sampleQuery({14, {0}}, 0, false, true, {}, assisted));
    }
    void stateRecovery_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned mode : {0u, 2u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(mode))) << vendor << mode;
    }
    void stateRecovery() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, mode);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("state.gpa_frame");
            fixture(4, 0, mode, true).save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            options.debug = options.before = true;
            options.until = 200;
            Replay replay(frame, options);
            const auto snapshot = snapshotResourceInputs(replay, 4);
            Com<ID3D11DeviceContext> context;
            Com<ID3D11ShaderResourceView> srv;
            replay.inspectNativeState([&](ID3D11DeviceContext *native, const auto &) {
                context = native;
                if (mode == 0)
                    native->PSGetShaderResources(0, 1, &srv);
            });
            D3D11_BUFFER_DESC bd{64,
                                 D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_UNORDERED_ACCESS,
                                 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                 16};
            Com<ID3D11Buffer> buffer;
            check(replay.nativeDevice()->CreateBuffer(&bd, nullptr, &buffer),
                  "Create original counter buffer");
            const D3D11_UNORDERED_ACCESS_VIEW_DESC ud{
                DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER, {0, 4, D3D11_BUFFER_UAV_FLAG_COUNTER}};
            Com<ID3D11UnorderedAccessView> uav;
            check(replay.nativeDevice()->CreateUnorderedAccessView(buffer.Get(), &ud, &uav),
                  "Create original counter view");
            auto *unordered = uav.Get();
            const UINT initial = 17;
            context->CSSetUnorderedAccessViews(7, 1, &unordered, &initial);
            const D3D11_QUERY_DESC pd{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
            Com<ID3D11Predicate> predicate;
            check(replay.nativeDevice()->CreatePredicate(&pd, &predicate), "Create original predicate");
            context->Begin(predicate.Get());
            context->End(predicate.Get());
            context->SetPredication(predicate.Get(), TRUE);
            const auto before = bindings(context.Get());
            D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
            sourceView.Format = DXGI_FORMAT_R32_FLOAT;
            sourceView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            const auto original = replay.observeTextureSamples(20, sourceView, 5, 1040);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, replay.observeSampleQueries(srv.Get(), {}, 0));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     replay.observeSampleQueries(srv.Get(), {}, 60001));
            unsigned checks = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     replay.observeSampleQueries(srv.Get(), [&] { return ++checks == 3; }));
            QCOMPARE(checks, 3u);
            QCOMPARE(bindings(context.Get()), before);
            const auto actual = replay.observeSampleQueries(srv.Get());
            const auto &expected = mode == 0 ? snapshot.inputs.textures->views.at({7, {0}}).queries
                                             : snapshot.inputs.textures->rasterizerQueries;
            QVERIFY(actual.observed && expected->observed);
            QCOMPARE(actual.count, expected->count);
            QCOMPARE(actual.positions, expected->positions);
            QCOMPARE(bindings(context.Get()), before);
            context->SetPredication(nullptr, FALSE);
            QCOMPARE(counter(replay, context.Get(), uav.Get()), 17u);
            QCOMPARE(replay.observeTextureSamples(20, sourceView, 5, 1040), original);
            validate(replay);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void initTestCase() {
        QTemporaryDir directory;
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool multisample : {false, true}) {
                const auto path = directory.filePath(QString("%1-%2").arg(vendor).arg(multisample));
                QProcess process;
                process.start(QCoreApplication::applicationDirPath() + "/FloraSampleQueryProbe.exe",
                              {path, QString::number(vendor), QString::number(multisample)});
                if (!process.waitForFinished(60000)) {
                    process.kill();
                    process.waitForFinished(5000);
                    QFAIL("Independent sample query probe timed out");
                }
                QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                         process.readAllStandardError().constData());
                QFile file(path + "/results.json");
                QVERIFY(file.open(QIODevice::ReadOnly));
                auto report = Json::parse(file.readAll().toStdString());
                QCOMPARE(report.at("validation_errors").get<unsigned>(), 0u);
                QCOMPARE(report.at("cases").size(), size_t(90));
                references[{vendor, multisample}] = std::move(report);
            }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("count");
        QTest::addColumn<uint32_t>("quality");
        QTest::addColumn<unsigned>("mode");
        QTest::addColumn<bool>("multisample");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t count : {1u, 2u, 4u, 8u})
                for (uint32_t quality : {0u, UINT32_MAX, UINT32_MAX - 1})
                    for (unsigned mode = 0; mode < 5; ++mode)
                        for (bool multisample : {false, true})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(count)
                                                         .arg(quality)
                                                         .arg(mode)
                                                         .arg(multisample)))
                                << vendor << count << quality << mode << multisample;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, count);
        QFETCH(uint32_t, quality);
        QFETCH(unsigned, mode);
        QFETCH(bool, multisample);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("sample-queries.gpa_frame");
            fixture(count, quality, mode, multisample).save(path);
            Frame frame(path.toStdWString());
            ReplayOptions config;
            config.warp = !vendor;
            if (vendor)
                config.vendor = vendor;
            config.debug = config.before = true;
            config.until = 200;
            Replay replay(frame, config);
            const auto observed = snapshotResourceInputs(replay, 4);
            const auto &reference = references.at({vendor, multisample});
            QCOMPARE(observed.adapterLuid, reference.at("adapter_luid").get<uint64_t>());
            const unsigned sourceMode = mode == 4 ? 0 : std::min(mode, 2u);
            const bool unbound = mode >= 3;
            const auto &cases = reference.at("cases");
            const auto found = std::find_if(cases.begin(), cases.end(), [&](const auto &c) {
                return c.at("count") == count && c.at("quality") == quality && c.at("mode") == sourceMode &&
                       c.at("unbound") == unbound;
            });
            QVERIFY(found != cases.end());
            QCOMPARE(found->at("status").get<std::string>(), std::string("observed"));
            const auto expectedCount = found->at("records")[0].at("bits")[0].get<uint32_t>();
            const uint32_t dimension = mode == 0 || mode == 4 ? 4 : mode == 1 ? 9 : 0;
            for (uint32_t stage : {0u, 1u, 5u})
                for (const auto &point : found->at("records"))
                    for (unsigned operation = 0; operation < 3; ++operation)
                        for (uint32_t mask : {5u, 15u}) {
                            auto inputs = observed.inputs;
                            inputs.constantBuffers[0] = {
                                Value::known({point.at("sample").get<uint32_t>(), 0, 0, 0})};
                            auto code =
                                program(stage, dimension, operation ? 111 : 110, operation == 2, mask);
                            Lane lane(code, inputs);
                            lane.step();
                            const auto step = lane.step();
                            auto bits =
                                operation
                                    ? std::array<uint32_t, 4>{operation == 2 ? expectedCount
                                                                             : std::bit_cast<uint32_t>(
                                                                                   float(expectedCount)),
                                                              0, 0, 0}
                                    : std::array<uint32_t, 4>{point.at("bits")[1].get<uint32_t>(),
                                                              point.at("bits")[2].get<uint32_t>(), 0, 0};
                            std::reverse(bits.begin(), bits.end());
                            const auto value = lane.value({stage == 5 ? 0u : 2u, {0}});
                            const uint8_t available = mode == 3 ? (operation ? 7 : 3) : 15;
                            QCOMPARE(value.valid, uint8_t((15 & ~mask) | (available & mask)));
                            for (unsigned component = 0; component < 4; ++component)
                                if (value.valid & (1u << component))
                                    QCOMPARE(value.bits[component],
                                             mask & (1u << component) ? bits[component] : 7 + component);
                            QCOMPARE(step.memoryGpuAssisted, mode != 4 && mode != 3);
                        }
            Program compiled(observed.bytecode);
            Lane lane(compiled, observed.inputs);
            while (!lane.finished())
                lane.step();
            const auto output = lane.value({2, {0}});
            QCOMPARE(output.valid, uint8_t(mode == 3 ? 8 : 15));
            auto expected = found->at("records")[0].at("bits").get<std::array<uint32_t, 4>>();
            expected[3] = 0;
            for (unsigned component = 0; component < 4; ++component)
                if (output.valid & (1u << component))
                    QCOMPARE(output.bits[component], expected[component]);
            validate(replay);
            qInfo().noquote() << QString::fromStdString(Json({{"vendor", vendor},
                                                              {"count", count},
                                                              {"quality", quality},
                                                              {"mode", mode},
                                                              {"multisample", multisample},
                                                              {"adapter_luid", observed.adapterLuid},
                                                              {"semantics_verified", mode != 3},
                                                              {"unknown_preserved", mode == 3},
                                                              {"passed", true}})
                                                            .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcSampleQueryTests)
#include "DxbcSampleQueryTests.moc"
