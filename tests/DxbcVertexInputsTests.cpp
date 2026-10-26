#include "DepthStencilCapture.h"
#include "application/DxbcInputSnapshot.h"
#include "application/DxbcVertexInputs.h"
#include "application/Experiment.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeDebugSession.h"
#include "application/NativeVertexDebug.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <d3d11sdklayers.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
template <typename T> void append(std::vector<uint8_t> &bytes, const T &value) {
    const auto *data = reinterpret_cast<const uint8_t *>(&value);
    bytes.insert(bytes.end(), data, data + sizeof value);
}
std::vector<uint8_t> compile(const std::string &source, const char *profile = "vs_5_0") {
    Com<ID3DBlob> binary, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "vertex-input-test", nullptr, nullptr, "main",
                               profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                        : "Compile failed");
    const auto *data = static_cast<const uint8_t *>(binary->GetBufferPointer());
    return {data, data + binary->GetBufferSize()};
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> adapter;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create vertex test factory");
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate vertex test adapters");
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc), "Describe vertex test adapter");
                if (desc.VendorId == vendor) {
                    adapter = candidate;
                    break;
                }
            }
            if (!adapter)
                throw std::runtime_error("Requested vertex test adapter is unavailable");
        }
        check(D3D11CreateDevice(adapter.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create vertex test device");
    }
};
void adapters() {
    QTest::addColumn<uint32_t>("vendor");
    for (uint32_t v : {0u, 4318u, 32902u})
        QTest::newRow(qPrintable(QString::number(v))) << v;
}
std::vector<D3D11_SIGNATURE_PARAMETER_DESC> signature(Bytes code) {
    // Semantic names are borrowed from the reflection object; only numeric
    // fields are used after this function returns.
    Com<ID3D11ShaderReflection> reflect;
    check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflect)), "Reflect test input signature");
    D3D11_SHADER_DESC desc{};
    check(reflect->GetDesc(&desc), "Describe test shader");
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> result(desc.InputParameters);
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        check(reflect->GetInputParameterDesc(i, &result[i]), "Read test input signature");
        result[i].SemanticName = nullptr;
    }
    return result;
}
Value input(const std::map<Register, Value> &row, const D3D11_SIGNATURE_PARAMETER_DESC &sig) {
    return row.at(Register{1, {sig.Register}});
}
std::array<uint32_t, 4> words(unsigned vertex, unsigned field) {
    return {0x80000000u | vertex, 0x7fc12345u + field, 0xffffffffu - vertex - field, 1u + field};
}
testing::Capture frameFixture(bool indirect, Bytes shader) {
    using testing::statePack;
    auto capture = testing::depthStencilCapture();
    std::erase_if(capture.entries,
                  [](const auto &e) { return e.category == 3 || e.category == 7 || e.id == 31; });
    auto code = statePack(uint64_t(shader.size()));
    code.insert(code.end(), shader.begin(), shader.end());
    append(code, Id(0));
    capture.add(31, 9, 0x81, code);
    capture.add(9000, 9, 1, statePack(5u, std::array{'D', 'A', 'T', 'A', '\0'}));
    auto layout = statePack(1u, Id(9000), 0u, UINT(DXGI_FORMAT_R32G32B32A32_FLOAT), 2u, 0u, 0u, 0u,
                            UINT(shader.size()));
    layout.insert(layout.end(), shader.begin(), shader.end());
    capture.add(9001, 9, 0x84, layout);
    capture.add(9002, 5, 0x82, statePack(Id(0), Id(0), Id(9001)));
    auto buffer = [&](Id id, UINT bind, UINT misc, const std::vector<uint8_t> &data) {
        capture.add(id, 5, 0x83,
                    statePack(Id(0), Id(0),
                              D3D11_BUFFER_DESC{UINT(data.size()), D3D11_USAGE_DEFAULT, bind, 0, misc, 0},
                              id + 1));
        auto payload = statePack(UINT(data.size()));
        payload.insert(payload.end(), data.begin(), data.end());
        capture.add(id + 1, 9, 1, payload);
    };
    std::vector<uint8_t> vertices;
    for (unsigned v = 0; v < 6; ++v)
        append(vertices, std::array{float(v), 10.f + v, 20.f + v, 30.f + v});
    buffer(9010, D3D11_BIND_VERTEX_BUFFER, 0, vertices);
    const auto indices = statePack(std::array<uint16_t, 6>{99, 99, 2, 0, 0xffff, 1});
    buffer(9020, D3D11_BIND_INDEX_BUFFER, 0, indices);
    State state{};
    state.topology = 1;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[0].shader = 30;
    state.layout = 9002;
    state.vb[2] = 9010;
    state.strides[2] = 16;
    state.ib = 9020;
    state.ibFormat = DXGI_FORMAT_R16_UINT;
    state.ibOffset = 2;
    capture.add(990, 3, 3, testing::snapshot(state));
    if (indirect) {
        buffer(9030, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, statePack(0xcdcdcdcdu, 4u, 2u, 1u, 1, 5u));
        capture.add(1000, 7, 0x3b, statePack(Id(990), Id(0), Id(1), Id(9030), 4u));
    } else
        capture.add(1000, 7, 0x3a, statePack(Id(990), Id(0), Id(1), 4u, 2u, 1u, 1, 5u));
    return capture;
}
} // namespace
class DxbcVertexInputsTests final : public QObject {
    Q_OBJECT
  private slots:
    void executionJobs_data() { adapters(); }
    void executionJobs() {
        QFETCH(uint32_t, vendor);
        using Json = nlohmann::json;
        const auto load = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read vertex job artifact");
            return file.readAll();
        };
        try {
            const auto code = compile("float4 main(float4 a:DATA,uint v:SV_VertexID,uint i:SV_InstanceID)"
                                      ":SV_Position{return a+float4(v,i,0,0);}");
            const std::string edited = "float4 main(float4 a:DATA,uint v:SV_VertexID,uint i:SV_InstanceID)"
                                       ":SV_Position{return a+float4(v,i,0,0)*2;}";
            for (bool indirect : {false, true}) {
                QTemporaryDir dir;
                const auto capture = dir.filePath("frame.gpa_frame");
                frameFixture(indirect, code).save(capture);
                Frame frame(capture.toStdWString());
                Experiment experiment(frame);
                experiment.setShader(frame, 30, compile(edited), edited, "main");
                experiment.setBuffer(frame, 1000, 9010, 32, testing::statePack(100.f));
                experiment.save(dir.filePath("experiment.json"));
                Json selection{{"gpa_event", 1000}, {"vertex", 3}, {"instance", 1}};
                if (vendor)
                    selection["gpu_vendor"] = vendor;
                else
                    selection["warp"] = true;
                const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
                auto job = prepareNativeAnalysisJob(capture, "debug-vertex", dir.filePath("worker"), worker,
                                                    selection, dir.filePath("experiment.json"));
                const auto result = Json::parse(load(runNativeAnalysisJob(job, worker, 30)).toStdString());
                NativeArtifactBytes artifacts;
                for (const auto *name : {"debug.json", "trace.jsonl", "shader.dxbc"})
                    artifacts[name] = load(dir.filePath("worker/") + name);
                NativeDebugSession session(job, result, artifacts);
                QCOMPARE(result.at("data").at("invocation").at("input_reference"), Json(3));
                QCOMPARE(result.at("data").at("invocation").at("instance"), Json(1));
                auto &model = session.model();
                const auto initial = model.registerValues();
                model.move(model.size());
                bool output = false;
                for (const auto &r : model.registerValues())
                    if (r.at("name") == "o0") {
                        const std::array expected{102.f, 14.f, 22.f, 32.f};
                        for (unsigned c = 0; c < 4; ++c)
                            QCOMPARE(r.at("bits")[c], Json(std::bit_cast<uint32_t>(expected[c])));
                        QCOMPARE(r.at("valid_components"), Json(15));
                        output = true;
                    }
                QVERIFY(output);
                model.move(0);
                QCOMPARE(model.registerValues(), initial);
                for (const auto &module : result.at("execution").at("loaded_modules")) {
                    const auto path = QString::fromStdString(module.get<std::string>()).toLower();
                    QVERIFY(!path.contains("renderdoc") && !path.contains("intelswtools\\gpa\\"));
                }
                QProcess cli;
                QStringList args{"analyze",
                                 capture,
                                 "debug-vertex",
                                 "--gpa-event",
                                 "1000",
                                 "--vertex",
                                 "3",
                                 "--instance",
                                 "1",
                                 "--experiment",
                                 dir.filePath("experiment.json"),
                                 "--out",
                                 dir.filePath("cli")};
                if (vendor)
                    args << "--gpu-vendor" << QString::number(vendor);
                else
                    args << "--warp";
                cli.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
                QVERIFY(cli.waitForFinished(30000));
                QCOMPARE(cli.exitCode(), 0);
                const auto cliResult = Json::parse(load(dir.filePath("cli/result.json")).toStdString());
                QCOMPARE(cliResult.at("data").at("shader_sha256"), result.at("data").at("shader_sha256"));
                QCOMPARE(cliResult.at("data").at("trace").at("steps"),
                         result.at("data").at("trace").at("steps"));
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void executionRejection() {
        QTemporaryDir dir;
        const auto code = compile("float4 main(float4 a:DATA):SV_Position{return a;}");
        const auto capture = dir.filePath("frame.gpa_frame");
        frameFixture(false, code).save(capture);
        Frame frame(capture.toStdWString());
        ReplayOptions config;
        config.until = 1000;
        config.before = config.prepareBeforeDraw = config.warp = true;
        Replay replay(frame, config);
        for (unsigned mode = 0; mode < 5; ++mode) {
            VertexTraceOptions options;
            options.selection = {3, 1};
            if (mode == 0)
                options.selection.reference = 2;
            if (mode == 1)
                options.selection.instance = 2;
            if (mode == 2)
                options.maxSteps = 1;
            if (mode == 3)
                options.maxBytes = 10;
            const auto path = dir.filePath(QString("failure%1.jsonl").arg(mode));
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                debugNativeVertex(replay, path, options, 1048576, {}, {},
                                  mode == 4 ? std::optional<uint32_t>(99) : std::nullopt));
            QVERIFY(!QFileInfo::exists(path));
        }
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, debugNativeVertex(replay, dir.filePath("cancel.jsonl"),
                                                                       {}, 1048576, [] { return true; }));
        QVERIFY(!QFileInfo::exists(dir.filePath("cancel.jsonl")));
        VertexTraceOptions valid;
        valid.selection = {3, 1};
        const auto result = debugNativeVertex(replay, dir.filePath("retry.jsonl"), valid, 1048576);
        QVERIFY(result.at("trace").at("completed") == true);
    }
    void fullSignature_data() { adapters(); }
    void fullSignature() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            std::string source = "struct I{";
            for (unsigned i = 0; i < 32; ++i)
                source += "uint4 a" + std::to_string(i) + ":DATA" + std::to_string(i) + ";";
            source += "};float4 main(I i):SV_Position{uint4 o=0;";
            for (unsigned i = 0; i < 32; ++i)
                source += "o^=i.a" + std::to_string(i) + ";";
            source += "return asfloat(o);}";
            const auto code = compile(source);
            const auto transformed = vertexInputShader(code);
            QCOMPARE(transformed.fields.size(), size_t(32));
            QCOMPARE(transformed.stride, 512u);
            std::vector<VertexInputElement> layout;
            for (unsigned i = 0; i < 32; ++i)
                layout.push_back({"DATA", i, DXGI_FORMAT_R32G32B32A32_UINT, 0, i * 16});
            std::vector<uint8_t> bytes;
            for (unsigned v = 0; v < 5; ++v)
                for (unsigned f = 0; f < 32; ++f)
                    append(bytes, words(v, f));
            VertexInputDraw draw;
            draw.count = 3;
            draw.start = 2;
            const auto captured =
                captureVertexInputs(device.device.Get(), !vendor, code, layout, {{0, {bytes, 512, 0}}}, draw);
            QCOMPARE(captured.rows.size(), size_t(3));
            const auto selected =
                captureVertexInputs(device.device.Get(), !vendor, code, layout, {{0, {bytes, 512, 0}}}, draw,
                                    65536, {}, 5000, VertexInputInvocation{2, 0});
            QCOMPARE(selected.rows.at(0), captured.rows.at(2));
            const auto sig = signature(code);
            QCOMPARE(sig.size(), size_t(32));
            for (unsigned v = 0; v < 3; ++v)
                for (const auto &s : sig) {
                    const auto value = input(captured.rows[v], s);
                    QCOMPARE(value.valid, uint8_t(15));
                    QCOMPARE(value.bits, words(v + 2, s.SemanticIndex));
                }
            QCOMPARE(captured.identity.at("adapter_vendor"), nlohmann::json(vendor ? vendor : 0x1414u));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void offsetsAndBits_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("indexed");
        QTest::addColumn<uint32_t>("stepRate");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool indexed : {false, true})
                for (uint32_t rate : {0u, 1u, 2u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(indexed).arg(rate)))
                        << vendor << indexed << rate;
    }
    void offsetsAndBits() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, indexed);
        QFETCH(uint32_t, stepRate);
        try {
            Device device(vendor);
            const auto code = compile(
                "struct I{float4 f:DATA0;uint4 u:DATA1;int4 s:DATA2;float4 inst:DATA3;"
                "uint vertex:SV_VertexID;uint instance:SV_InstanceID;};"
                "RWByteAddressBuffer forbidden:register(u0);"
                "float4 main(I i):SV_Position{uint old;forbidden.InterlockedAdd(0,1,old);"
                "return asfloat(asuint(i.f)^i.u^asuint(i.s)^asuint(i.inst)^i.vertex^i.instance^old);}");
            const std::vector<VertexInputElement> layout{
                {"DATA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0},
                {"DATA", 1, DXGI_FORMAT_R32G32B32A32_UINT, 0, 16},
                {"DATA", 2, DXGI_FORMAT_R32G32B32A32_SINT, 0, 32},
                {"DATA", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, 1, stepRate}};
            std::vector<uint8_t> bytes(16, 0xcd), instances(16, 0xef), indices;
            for (unsigned v = 0; v < 16; ++v) {
                for (unsigned f = 0; f < 3; ++f)
                    append(bytes, words(v, f));
                append(instances, words(v, 3));
            }
            for (uint32_t index : {99u, 99u, 2u, 0u, 1u, 2u, UINT32_MAX, 3u})
                append(indices, index);
            VertexInputDraw draw;
            draw.count = indexed ? 6 : 4;
            draw.instances = 4;
            draw.start = indexed ? 1 : 5;
            draw.startInstance = 3;
            if (indexed) {
                draw.indices = indices;
                draw.indexOffset = 4;
                draw.indexFormat = DXGI_FORMAT_R32_UINT;
                draw.baseVertex = 5;
            }
            const auto captured = captureVertexInputs(device.device.Get(), !vendor, code, layout,
                                                      {{0, {bytes, 48, 16}}, {1, {instances, 16, 16}}}, draw);
            const std::vector<unsigned> vertices =
                indexed ? std::vector<unsigned>{2, 0, 1, 2, 3} : std::vector<unsigned>{0, 1, 2, 3};
            QCOMPARE(captured.rows.size(), vertices.size() * 4);
            const auto sig = signature(code);
            for (const auto instance : {0u, 1u, 3u}) {
                const auto reference = indexed ? 5u : 3u;
                const auto selected =
                    captureVertexInputs(device.device.Get(), !vendor, code, layout,
                                        {{0, {bytes, 48, 16}}, {1, {instances, 16, 16}}}, draw, 64 * 1024, {},
                                        5000, VertexInputInvocation{reference, instance});
                QCOMPARE(selected.rows.size(), size_t(1));
                QCOMPARE(selected.identity.at("records"), nlohmann::json(1));
                for (const auto &s : sig) {
                    const auto actual = input(selected.rows.front(), s);
                    const auto expected =
                        input(captured.rows[instance * vertices.size() + vertices.size() - 1], s);
                    QCOMPARE(actual.valid, expected.valid);
                    QCOMPARE(actual.bits, expected.bits);
                }
            }
            for (unsigned instance = 0; instance < 4; ++instance)
                for (unsigned v = 0; v < vertices.size(); ++v)
                    for (const auto &s : sig) {
                        const auto value = input(captured.rows[instance * vertices.size() + v], s);
                        if (s.SystemValueType == D3D_NAME_VERTEX_ID ||
                            s.SystemValueType == D3D_NAME_INSTANCE_ID) {
                            const auto c = std::countr_zero(unsigned(s.Mask));
                            QVERIFY(value.valid & (1u << c));
                            QCOMPARE(value.bits[c],
                                     s.SystemValueType == D3D_NAME_VERTEX_ID ? vertices[v] : instance);
                        } else {
                            QCOMPARE(value.valid, uint8_t(15));
                            const auto record = s.SemanticIndex == 3
                                                    ? (3 + (stepRate ? instance / stepRate : 0))
                                                    : vertices[v] + 5;
                            QCOMPARE(value.bits, words(record, s.SemanticIndex));
                        }
                    }
            // The original body has a UAV atomic but the private input program
            // contains no resources or writes and needs no bound UAV.
            const auto copyShader = vertexInputShader(code);
            for (const auto &[tag, b] : readDxbcParts(copyShader.bytecode))
                if (tag == 0x58454853 || tag == 0x52444853)
                    for (const auto &row : readDxbcProgram(b).instructions)
                        QVERIFY((row[0] & 2047) == 95 || (row[0] & 2047) == 96 || (row[0] & 2047) == 97 ||
                                (row[0] & 2047) == 101 || (row[0] & 2047) == 54 || (row[0] & 2047) == 62);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void conversionAndPrecision_data() { adapters(); }
    void conversionAndPrecision() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            for (const bool minimum : {false, true}) {
                const auto code =
                    compile(std::string("float4 main(") + (minimum ? "min16float4" : "float4") +
                            " a:DATA0,float4 b:DATA1,float4 c:DATA2):SV_Position{return float4(a)+b+c;}");
                const std::vector<VertexInputElement> layout{{"DATA", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0},
                                                             {"DATA", 1, DXGI_FORMAT_R16G16_FLOAT, 0, 4},
                                                             {"DATA", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8}};
                std::vector<uint8_t> bytes;
                append(bytes, 0xffff0000u);
                append(bytes, 0xc0003c00u);
                append(bytes, std::array{3.f, 4.f, 5.f});
                VertexInputDraw draw;
                draw.count = 1;
                const auto captured = captureVertexInputs(device.device.Get(), !vendor, code, layout,
                                                          {{0, {bytes, 20, 0}}}, draw);
                const auto selected =
                    captureVertexInputs(device.device.Get(), !vendor, code, layout, {{0, {bytes, 20, 0}}},
                                        draw, 65536, {}, 5000, VertexInputInvocation{0, 0});
                QCOMPARE(selected.rows, captured.rows);
                const std::array expected{std::array{0.f, 0.f, 1.f, 1.f}, std::array{1.f, -2.f, 0.f, 1.f},
                                          std::array{3.f, 4.f, 5.f, 1.f}};
                for (const auto &s : signature(code)) {
                    const auto value = input(captured.rows.at(0), s);
                    QCOMPARE(value.valid, uint8_t(15));
                    for (unsigned c = 0; c < 4; ++c)
                        QCOMPARE(value.bits[c], std::bit_cast<uint32_t>(expected[s.SemanticIndex][c]));
                }
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void sparseAndEmpty_data() { adapters(); }
    void sparseAndEmpty() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            const auto code = compile("float4 main(float4 a:DATA):SV_Position{return a.xzxz;}");
            const auto generated = vertexInputShader(code);
            QCOMPARE(generated.fields.size(), size_t(1));
            QCOMPARE(generated.fields[0].mask, 5u);
            const std::array<uint32_t, 4> values{0x7fc01234, 0xdeadbeef, 0x80000000, 0xcdcdcdcd};
            const Bytes bytes(reinterpret_cast<const uint8_t *>(values.data()), sizeof values);
            VertexInputDraw draw;
            draw.count = 1;
            const auto captured = captureVertexInputs(device.device.Get(), !vendor, code,
                                                      {{"DATA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0}},
                                                      {{0, {bytes, 16, 0}}}, draw);
            const auto value = captured.rows.at(0).at(Register{1, {0}});
            QCOMPARE(value.valid, uint8_t(5));
            QCOMPARE(value.bits[0], values[0]);
            QCOMPARE(value.bits[2], values[2]);
            const auto empty = compile("float4 main():SV_Position{return 0;}");
            draw.count = 3;
            const auto noInputs = captureVertexInputs(device.device.Get(), !vendor, empty, {}, {}, draw);
            QCOMPARE(noInputs.rows.size(), size_t(3));
            for (const auto &row : noInputs.rows)
                QVERIFY(row.empty());
            draw.instances = 0;
            QVERIFY(captureVertexInputs(device.device.Get(), !vendor, empty, {}, {}, draw).rows.empty());
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void legacyProfilesAndShiftedMask_data() { adapters(); }
    void legacyProfilesAndShiftedMask() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            for (const auto profile : {"vs_4_0", "vs_4_1", "vs_5_0"}) {
                const auto code = compile("float4 main(float4 a:DATA):SV_Position{return a.ywyw;}", profile);
                const auto generated = vertexInputShader(code);
                QCOMPARE(generated.fields.at(0).mask, 10u);
                const std::array<uint32_t, 4> values{0xcdcdcdcd, 0x7fc05678, 0xdeadbeef, 0x80000000};
                const Bytes bytes(reinterpret_cast<const uint8_t *>(values.data()), sizeof values);
                VertexInputDraw draw;
                draw.count = 1;
                const auto captured = captureVertexInputs(device.device.Get(), !vendor, code,
                                                          {{"DATA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0}},
                                                          {{0, {bytes, 16, 0}}}, draw);
                const auto value = captured.rows.at(0).at(Register{1, {0}});
                const auto selected = captureVertexInputs(
                    device.device.Get(), !vendor, code, {{"DATA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0}},
                    {{0, {bytes, 16, 0}}}, draw, 65536, {}, 5000, VertexInputInvocation{0, 0});
                QCOMPARE(selected.rows, captured.rows);
                QCOMPARE(value.valid, uint8_t(10));
                QCOMPARE(value.bits[1], values[1]);
                QCOMPARE(value.bits[3], values[3]);
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void rejectsAndCancellation() {
        Device device(0);
        const auto code = compile("float4 main(uint i:SV_VertexID):SV_Position{return i;}");
        VertexInputDraw draw;
        draw.count = 3;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 captureVertexInputs(device.device.Get(), true, code, {}, {}, draw, 1));
        QVERIFY_THROWS_EXCEPTION(
            OperationCancelled,
            captureVertexInputs(device.device.Get(), true, code, {}, {}, draw, 1024, [] { return true; }));
        draw.indexFormat = DXGI_FORMAT_R16_UINT;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 captureVertexInputs(device.device.Get(), true, code, {}, {}, draw));
        const auto ps = compile("float4 main():SV_Target{return 0;}", "ps_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vertexInputShader(ps));
        auto truncated = code;
        truncated.resize(20);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vertexInputShader(truncated));
    }
    void selectedLargeDraw_data() { adapters(); }
    void selectedBoundaries_data() { adapters(); }
    void selectedBoundaries() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            const auto code =
                compile("float4 main(float4 a:DATA0,uint4 b:DATA1,float4 c:DATA2,float4 d:DATA3,"
                        "float4 e:DATA4,float f:DATA5,uint v:SV_VertexID,uint i:SV_InstanceID):SV_Position"
                        "{return a+asfloat(b)+c+d+e+f+v+i;}");
            const std::vector<VertexInputElement> layout{
                {"DATA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0},
                {"DATA", 1, DXGI_FORMAT_R32G32B32A32_UINT, 1, 0, 1, 2},
                {"DATA", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0, 1, 1},
                {"DATA", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, 1, 0},
                {"DATA", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0},
                {"DATA", 5, DXGI_FORMAT_R32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT}};
            std::vector<uint8_t> bytes;
            for (unsigned v = 0; v < 4; ++v)
                append(bytes, words(v, 0));
            const std::map<uint32_t, VertexInputBuffer> buffers{
                {0, {bytes, 20, 0}}, {1, {bytes, 16, 0}}, {2, {bytes, 16, 0}}, {4, {bytes, 0, 0}}};
            for (const auto format : {0u, uint32_t(DXGI_FORMAT_R16_UINT), uint32_t(DXGI_FORMAT_R32_UINT)}) {
                std::vector<uint8_t> indices;
                VertexInputDraw draw;
                draw.count = 2;
                draw.instances = 4;
                draw.startInstance = 1;
                draw.indexFormat = format;
                if (format) {
                    if (format == DXGI_FORMAT_R16_UINT) {
                        append(indices, uint16_t(3));
                        append(indices, uint16_t(2));
                    } else {
                        append(indices, uint32_t(3));
                        append(indices, uint32_t(2));
                    }
                    draw.indices = indices;
                    draw.baseVertex = -1;
                } else
                    draw.start = 1;
                const auto full =
                    captureVertexInputs(device.device.Get(), !vendor, code, layout, buffers, draw);
                for (const auto reference : {0u, 1u}) {
                    const auto selected =
                        captureVertexInputs(device.device.Get(), !vendor, code, layout, buffers, draw, 65536,
                                            {}, 5000, VertexInputInvocation{reference, 3});
                    QCOMPARE(selected.rows.size(), size_t(1));
                    for (const auto &s : signature(code)) {
                        const auto actual = input(selected.rows.front(), s);
                        const auto expected = input(full.rows.at(6 + reference), s);
                        QCOMPARE(actual.valid, expected.valid);
                        QCOMPARE(actual.bits, expected.bits);
                    }
                }
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void selectedLargeDraw() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            const auto code =
                compile("uint4 main(uint v:SV_VertexID,uint i:SV_InstanceID):DATA{return uint4(v,i,0,0);}");
            VertexInputDraw draw;
            draw.count = draw.instances = UINT32_MAX;
            draw.startInstance = 17;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, captureVertexInputs(device.device.Get(), !vendor,
                                                                             code, {}, {}, draw, 4096));
            const auto selected =
                captureVertexInputs(device.device.Get(), !vendor, code, {}, {}, draw, 4096, {}, 5000,
                                    VertexInputInvocation{UINT32_MAX - 1, UINT32_MAX - 2});
            QCOMPARE(selected.rows.size(), size_t(1));
            QVERIFY(selected.identity.at("budgeted_bytes").get<uint64_t>() < 4096);
            for (const auto &s : signature(code)) {
                const auto value = input(selected.rows.front(), s);
                const auto component = std::countr_zero(unsigned(s.Mask));
                QCOMPARE(value.bits[component],
                         s.SystemValueType == D3D_NAME_VERTEX_ID ? UINT32_MAX - 1 : UINT32_MAX - 2);
            }
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     captureVertexInputs(device.device.Get(), !vendor, code, {}, {}, draw,
                                                         4096, {}, 5000,
                                                         VertexInputInvocation{UINT32_MAX, 0}));
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     captureVertexInputs(
                                         device.device.Get(), !vendor, code, {}, {}, draw, 4096,
                                         [] { return true; }, 5000, VertexInputInvocation{0, 0}));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void frameInputs_data() { adapters(); }
    void frameInputs() {
        QFETCH(uint32_t, vendor);
        try {
            const std::string shader = "float4 main(float4 a:DATA,uint v:SV_VertexID,uint i:SV_InstanceID)"
                                       ":SV_Position{return a+float4(v,i,0,0);}";
            const auto code = compile(shader);
            const auto editedCode =
                compile("float4 main(float4 a:DATA,uint v:SV_VertexID,uint i:SV_InstanceID)"
                        ":SV_Position{return a+float4(v,i,0,0)*2;}");
            for (bool indirect : {false, true}) {
                QTemporaryDir directory;
                const auto path = directory.filePath("frame.gpa_frame");
                frameFixture(indirect, code).save(path);
                Frame frame(path.toStdWString());
                ReplayOptions options;
                options.before = options.prepareBeforeDraw = options.debug = true;
                options.until = 1000;
                options.warp = !vendor;
                if (vendor)
                    options.vendor = vendor;
                options.shaders[30] = editedCode;
                options.buffers[1000][9010] = {{32, testing::statePack(100.f)}};
                Replay replay(frame, options);
                const auto snapshot = snapshotVertexInputs(replay, {3, 1});
                QCOMPARE(snapshot.resources.bytecode, editedCode);
                QCOMPARE(snapshot.resources.event, Id(1000));
                QCOMPARE(snapshot.resources.shader, Id(30));
                QCOMPARE(snapshot.invocation.at("vertex_id"), nlohmann::json(1));
                QCOMPARE(snapshot.invocation.at("vertex_index"), nlohmann::json(2));
                QCOMPARE(snapshot.invocation.at("layout"), nlohmann::json(9002));
                QCOMPARE(snapshot.invocation.at("helper").at("records"), nlohmann::json(1));
                QCOMPARE(snapshot.invocation.at("helper").at("original_input_references"), nlohmann::json(4));
                QCOMPARE(snapshot.invocation.at("helper").at("original_instances"), nlohmann::json(2));
                QCOMPARE(snapshot.invocation.at("indirect").is_null(), !indirect);
                Program program(snapshot.resources.bytecode);
                Lane lane(program, snapshot.resources.inputs);
                while (!lane.finished())
                    lane.step();
                const auto output = lane.value(Register{2, {0}});
                QCOMPARE(output.valid, uint8_t(15));
                const std::array expected{102.f, 14.f, 22.f, 32.f};
                for (unsigned c = 0; c < 4; ++c)
                    QCOMPARE(output.bits[c], std::bit_cast<uint32_t>(expected[c]));
                const auto restored = replay.readBuffer(9010);
                QCOMPARE(Reader(Bytes(restored).subspan(32)).read<float>(), 2.f);
                QVERIFY(!replay.counts.contains("DrawIndexedInstanced"));
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotVertexInputs(replay, {2, 0}));
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotVertexInputs(replay, {4, 0}));
                const auto retry = snapshotVertexInputs(replay, {3, 1});
                QCOMPARE(retry.resources.inputs.registers, snapshot.resources.inputs.registers);
                if (indirect) {
                    // Resolve the argument buffer after the experiment edit,
                    // then restore both argument and vertex storage on exit.
                    auto edited = options;
                    edited.buffers[1000][9030] = {{16, testing::statePack(2)}};
                    edited.buffers[1000][9010] = {{48, testing::statePack(200.f)}};
                    Replay arguments(frame, edited);
                    const auto changed = snapshotVertexInputs(arguments, {3, 1});
                    QCOMPARE(changed.invocation.at("draw_parameters").at("base_vertex"), nlohmann::json(2));
                    QCOMPARE(changed.invocation.at("vertex_index"), nlohmann::json(3));
                    Lane changedLane(program, changed.resources.inputs);
                    while (!changedLane.finished())
                        changedLane.step();
                    const auto changedOutput = changedLane.value(Register{2, {0}});
                    QCOMPARE(changedOutput.bits[0], std::bit_cast<uint32_t>(202.f));
                    QCOMPARE(changedOutput.bits[1], std::bit_cast<uint32_t>(15.f));
                    const auto argumentBytes = arguments.readBuffer(9030),
                               vertexBytes = arguments.readBuffer(9010);
                    QCOMPARE(Reader(Bytes(argumentBytes).subspan(16)).read<int32_t>(), 1);
                    QCOMPARE(Reader(Bytes(vertexBytes).subspan(48)).read<float>(), 3.f);
                }
                options.disabled.insert(1000);
                Replay disabled(frame, options);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotVertexInputs(disabled, {0, 0}));
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcVertexInputsTests)
#include "DxbcVertexInputsTests.moc"
