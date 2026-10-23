#include "DepthStencilCapture.h"
#include "StreamCapture.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeMesh.h"
#include "core/Dxbc.h"
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11shader.h>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
struct Selection {
    uint32_t topology = 4, count = 4, instances = 1, startVertex = 5, startInstance = 9;
    unsigned indexWidth = 0;
    int32_t base = 5;
    std::vector<uint32_t> indices;
    bool indirect = false, fullSignature = false, minimumPrecision = false;
};
Selection selection(const QString &name) {
    Selection s;
    if (name == "line-adjacency") {
        s.topology = 10;
        s.count = 5;
    }
    if (name == "line-strip-adjacency") {
        s.topology = 11;
        s.count = 5;
    }
    if (name == "triangle-adjacency") {
        s.topology = 12;
        s.count = 7;
    }
    if (name == "triangle-strip-adjacency") {
        s.topology = 13;
        s.count = 9;
    }
    if (name == "patch") {
        s.topology = 35;
        s.count = 7;
    }
    if (name == "instanced" || name.startsWith("indirect"))
        s.instances = 3;
    if (name == "full-signature")
        s.fullSignature = true;
    if (name == "minimum-precision")
        s.minimumPrecision = true;
    if (name == "indexed" || name == "indirect-indexed" || name == "negative-base") {
        s.indexWidth = 4;
        s.indices = {2, 0, 1, 2, 1, 0};
        if (name == "negative-base") {
            s.base = -1;
            s.indices = {2, 3, 4, 2, 4, 3};
        }
    }
    if (name == "strip-cut") {
        s.topology = 5;
        s.indexWidth = 4;
        s.indices = {0, 1, 2, UINT32_MAX, 3, 4, 5, 6};
    }
    if (name == "list-cut") {
        s.indexWidth = 2;
        s.indices = {0, 1, 0xffff, 2, 3, 4, 5};
    }
    if (name == "point-cut") {
        s.topology = 1;
        s.indexWidth = 4;
        s.indices = {0, UINT32_MAX, 1};
    }
    s.indirect = name.startsWith("indirect");
    if (s.indexWidth)
        s.count = uint32_t(s.indices.size());
    return s;
}
Capture fixture(const Selection &s) {
    auto capture = depthStencilCapture();
    capture.entries.erase(
        std::remove_if(capture.entries.begin(), capture.entries.end(),
                       [](const auto &e) { return e.category == 7 || e.category == 3 || e.id == 31; }),
        capture.entries.end());
    std::string source = "struct V{float4 p:SV_Position;";
    if (s.minimumPrecision)
        source += "min16float4 reduced:ATTR0;";
    if (s.fullSignature)
        for (unsigned i = 0; i < 31; ++i)
            source += "uint4 a" + std::to_string(i) + ":ATTR" + std::to_string(i) + ";";
    source += "};V main(uint id:SV_VertexID,uint instance:SV_InstanceID){V o;o.p=float4(id,instance,0,1);";
    if (s.minimumPrecision)
        source += "o.reduced=min16float4(id,instance,7,1);";
    if (s.fullSignature)
        for (unsigned i = 0; i < 31; ++i)
            source += "o.a" + std::to_string(i) + "=uint4(id,instance," + std::to_string(i) + ",0xffffffff);";
    source += "return o;}";
    const auto code = compileClassProgram(source, "vs_5_0");
    auto bytes = statePack(uint64_t(code.size()));
    bytes.insert(bytes.end(), code.begin(), code.end());
    append(bytes, Id(0));
    capture.add(31, 9, 0x81, bytes);
    State state{};
    state.topology = s.topology;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[0].shader = 30;
    auto buffer = [&](Id id, UINT bind, UINT misc, const std::vector<uint8_t> &data) {
        capture.add(id, 5, 0x83,
                    statePack(Id(0), Id(0), uint32_t(data.size()), 0u, bind, 0u, misc, 0u, id + 1));
        auto packed = statePack(uint32_t(data.size()));
        packed.insert(packed.end(), data.begin(), data.end());
        capture.add(id + 1, 9, 1, packed);
    };
    if (s.indexWidth) {
        // Exercise both IA byte offset and Draw start-index offset.
        std::vector<uint8_t> data(2 * s.indexWidth, 0xcd);
        for (const auto index : s.indices)
            if (s.indexWidth == 2)
                append(data, uint16_t(index));
            else
                append(data, index);
        buffer(9000, D3D11_BIND_INDEX_BUFFER, 0, data);
        state.ib = 9000;
        state.ibFormat = s.indexWidth == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        state.ibOffset = s.indexWidth;
    }
    capture.add(990, 3, 3, snapshot(state));
    if (s.indirect) {
        auto args = s.indexWidth
                        ? statePack(0xcdcdcdcdu, s.count, s.instances, 1u, s.base, s.startInstance)
                        : statePack(0xcdcdcdcdu, s.count, s.instances, s.startVertex, s.startInstance);
        buffer(9010, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, args);
        capture.add(1000, 7, s.indexWidth ? 0x3b : 0x3d, statePack(Id(990), Id(0), Id(1), Id(9010), 4u));
    } else if (s.indexWidth)
        capture.add(1000, 7, 0x3a,
                    statePack(Id(990), Id(0), Id(1), s.count, s.instances, 1u, s.base, s.startInstance));
    else
        capture.add(1000, 7, 0x3c,
                    statePack(Id(990), Id(0), Id(1), s.count, s.instances, s.startVertex, s.startInstance));
    return capture;
}
bool installed(unsigned vendor) {
    if (!vendor)
        return true;
    Com<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Enumerate vertex test adapters");
    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> adapter;
        const auto status = factory->EnumAdapters1(i, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND)
            return false;
        check(status, "Enumerate vertex test adapter");
        DXGI_ADAPTER_DESC1 desc{};
        check(adapter->GetDesc1(&desc), "Describe vertex test adapter");
        if (desc.VendorId == vendor)
            return true;
    }
}
} // namespace
class NativeVertexInputsTests : public QObject {
    Q_OBJECT
  private slots:
    void inputReferences_data() {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<unsigned>("vendor");
        for (const auto vendor : {0u, 0x10deu, 0x8086u})
            for (const auto name : {"tail", "line-adjacency", "line-strip-adjacency", "triangle-adjacency",
                                    "triangle-strip-adjacency", "patch", "instanced", "indexed",
                                    "negative-base", "strip-cut", "list-cut", "point-cut", "indirect",
                                    "indirect-indexed", "full-signature", "minimum-precision"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(name).arg(vendor))) << QString(name) << vendor;
    }
    void inputReferences() {
        try {
            QFETCH(QString, scenario);
            QFETCH(unsigned, vendor);
            if (!installed(vendor))
                QSKIP("Requested adapter is not installed; WARP is covered separately");
            const auto s = selection(scenario);
            QTemporaryDir directory;
            const auto capture = directory.filePath("frame.gpa_frame");
            fixture(s).save(capture);
            Frame frame(capture.toStdWString());
            ReplayOptions options;
            options.warp = vendor == 0;
            if (vendor)
                options.vendor = vendor;
            options.until = 1000;
            options.before = true;
            Replay replay(frame, options);
            if (vendor)
                QCOMPARE(replay.adapterDescription().VendorId, vendor);
            replay.run();
            const auto original = replay.readTexture(20);
            const auto counts = replay.counts;
            PostTransformOptions inspect;
            inspect.stage = "vs-inputs";
            const auto geometry = inspectPostTransform(replay, 1000, inspect);
            std::vector<uint32_t> valid;
            for (uint32_t i = 0; i < s.count; ++i) {
                const auto index = s.indexWidth ? s.indices[i] : i;
                if (s.indexWidth && index == (s.indexWidth == 2 ? 0xffffu : UINT32_MAX))
                    continue;
                valid.push_back(index);
            }
            QCOMPARE(geometry.identities.size(), valid.size() * s.instances);
            QCOMPARE(geometry.report.at("inspection_topology"), Json(1));
            QCOMPARE(geometry.report.at("input_topology"), Json(s.topology));
            QCOMPARE(geometry.report.at("vertex_identity").at("shader_rewritten"), Json(false));
            const auto stride = geometry.report.at("stride").get<size_t>();
            QCOMPARE(stride, s.fullSignature ? size_t(512) : s.minimumPrecision ? size_t(32) : size_t(16));
            if (s.minimumPrecision) {
                const auto code = frame.shader(frame.resource(30).data);
                bool osg1 = false;
                for (const auto &[tag, body] : readDxbcParts(code))
                    osg1 |= tag == 0x3147534f;
                QVERIFY(osg1);
                Com<ID3D11ShaderReflection> reflection;
                check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)),
                      "Reflect minimum-precision fixture");
                D3D11_SHADER_DESC description{};
                check(reflection->GetDesc(&description), "Describe minimum-precision fixture");
                const auto &fields = geometry.report.at("attributes");
                QCOMPARE(fields.size(), size_t(description.OutputParameters));
                for (UINT i = 0; i < description.OutputParameters; ++i) {
                    D3D11_SIGNATURE_PARAMETER_DESC parameter{};
                    check(reflection->GetOutputParameterDesc(i, &parameter), "Reflect output signature");
                    QCOMPARE(fields[i].at("semantic"), Json(parameter.SemanticName));
                    QCOMPARE(fields[i].at("index"), Json(parameter.SemanticIndex));
                    QCOMPARE(fields[i].at("register"), Json(parameter.Register));
                    QCOMPARE(fields[i].at("system_value"), Json(unsigned(parameter.SystemValueType)));
                    QCOMPARE(fields[i].at("component_type"), Json(unsigned(parameter.ComponentType)));
                    QCOMPARE(fields[i].at("mask"), Json(parameter.Mask));
                    QCOMPARE(fields[i].at("used_mask"), Json(parameter.ReadWriteMask));
                    QCOMPARE(fields[i].at("stream"), Json(parameter.Stream));
                    QCOMPARE(fields[i].at("minimum_precision"), Json(unsigned(parameter.MinPrecision)));
                }
            }
            for (size_t row = 0; row < geometry.identities.size(); ++row) {
                const auto instance = uint32_t(row / valid.size()), index = valid[row % valid.size()];
                const auto &identity = geometry.identities[row];
                QCOMPARE(identity.instance, instance);
                QCOMPARE(identity.vertexId, index);
                QCOMPARE(identity.vertexIndex,
                         int64_t(index) + (s.indexWidth ? s.base : int64_t(s.startVertex)));
                // The shader independently emits the native system values; do not compare only metadata.
                Reader raw(Bytes(geometry.bytes).subspan(row * stride, stride));
                QCOMPARE(raw.read<float>(), float(index));
                QCOMPARE(raw.read<float>(), float(instance));
                QCOMPARE(raw.read<float>(), 0.f);
                QCOMPARE(raw.read<float>(), 1.f);
                if (s.minimumPrecision) {
                    QCOMPARE(raw.read<float>(), float(index));
                    QCOMPARE(raw.read<float>(), float(instance));
                    QCOMPARE(raw.read<float>(), 7.f);
                    QCOMPARE(raw.read<float>(), 1.f);
                }
                if (s.fullSignature)
                    for (unsigned field = 0; field < 31; ++field) {
                        QCOMPARE(raw.read<uint32_t>(), index);
                        QCOMPARE(raw.read<uint32_t>(), instance);
                        QCOMPARE(raw.read<uint32_t>(), field);
                        QCOMPARE(raw.read<uint32_t>(), UINT32_MAX);
                    }
            }
            const auto tables = postTransformTables(geometry);
            QCOMPARE(tables.at("tables").at("input_references").at("rows").size(),
                     size_t(s.count) * s.instances);
            const auto faces = tables.at("mesh").at("faces");
            if (scenario == "tail" || scenario == "triangle-adjacency" ||
                scenario == "triangle-strip-adjacency")
                QCOMPARE(faces.size(), scenario == "triangle-strip-adjacency" ? size_t(2) : size_t(1));
            if (scenario == "strip-cut")
                QCOMPARE(faces, Json({{1, 2, 3}, {4, 5, 6}, {6, 5, 7}}));
            if (scenario == "list-cut")
                QCOMPARE(faces, Json({{3, 4, 5}}));
            if (scenario == "point-cut")
                QCOMPARE(tables.at("mesh").at("points"), Json({1, 2}));
            if (scenario == "triangle-adjacency")
                QCOMPARE(faces, Json({{1, 3, 5}}));
            if (scenario == "line-adjacency")
                QCOMPARE(tables.at("mesh").at("lines"), Json({{2, 3}}));
            QCOMPARE(replay.readTexture(20), original);
            QCOMPARE(replay.counts, counts);
            if (s.instances > 1) {
                inspect.instance = 1;
                const auto selected = inspectPostTransform(replay, 1000, inspect);
                QCOMPARE(selected.identities.size(), valid.size());
                const auto begin = geometry.bytes.begin() + valid.size() * stride;
                QCOMPARE(selected.bytes, std::vector<uint8_t>(begin, begin + valid.size() * stride));
                for (const auto &identity : selected.identities)
                    QCOMPARE(identity.instance, 1u);
            }
            inspect.maxBytes = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 1000, inspect));
            QCOMPARE(replay.readTexture(20), original);
            QCOMPARE(replay.counts, counts);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void workerExport() {
        try {
            QTemporaryDir directory;
            const auto capture = directory.filePath("frame.gpa_frame"), output = directory.filePath("result");
            fixture(selection("triangle-adjacency")).save(capture);
            const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
            const auto job =
                prepareNativeAnalysisJob(capture, "mesh", output, worker,
                                         {{"gpa_event", 1000}, {"stage", "vs-inputs"}, {"warp", true}});
            QFile report(runNativeAnalysisJob(job, worker, 30));
            QVERIFY(report.open(QIODevice::ReadOnly));
            const auto result = Json::parse(report.readAll().toStdString());
            const auto mesh = readNativeMesh(job, result, output, 64 * 1024 * 1024);
            QCOMPARE(mesh.geometry.identities.size(), size_t(7));
            QCOMPARE(mesh.tables.at("mesh").at("faces"), Json({{1, 3, 5}}));
            QVERIFY(mesh.artifacts.contains("input_references.csv"));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void stateSafety_data() {
        QTest::addColumn<unsigned>("vendor");
        for (const auto vendor : {0u, 0x10deu, 0x8086u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void stateSafety() {
        try {
            QFETCH(unsigned, vendor);
            if (!installed(vendor))
                QSKIP("Requested adapter is not installed; WARP is covered separately");
            QTemporaryDir directory;
            PostTransformOptions inspect;
            inspect.stage = "vs-inputs";
            for (const bool signature : {false, true}) {
                const auto path = directory.filePath(signature ? "signature.gpa_frame" : "so.gpa_frame");
                streamCapture(signature).save(path);
                Frame frame(path.toStdWString());
                ReplayOptions options;
                options.warp = !vendor;
                if (vendor)
                    options.vendor = vendor;
                options.until = 150;
                options.before = true;
                Replay replay(frame, options);
                replay.run();
                const auto original = replay.readBuffer(70);
                QCOMPARE(inspectPostTransform(replay, 150, inspect).identities.size(),
                         signature ? size_t(3) : size_t(1));
                QCOMPARE(replay.readBuffer(70), original);
                auto limited = inspect;
                limited.maxBytes = 1;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPostTransform(replay, 150, limited));
                // No replay reset: original bindings and the hidden SO cursor must still be usable.
                replay.inspectNativeState(
                    [&](auto context, const auto &) { context->Draw(signature ? 3 : 1, 0); });
                options.before = false;
                Replay reference(frame, options);
                reference.run();
                QCOMPARE(replay.readBuffer(70), reference.readBuffer(70));
                options.until = 200;
                options.before = true;
                Replay automatic(frame, options);
                automatic.run();
                const auto source = automatic.readBuffer(70);
                const auto vertices = inspectPostTransform(automatic, 200, inspect);
                QCOMPARE(vertices.identities.size(), size_t(6));
                QCOMPARE(vertices.bytes, std::vector<uint8_t>(source.begin() + 16, source.begin() + 112));
                QCOMPARE(automatic.readBuffer(70), source);
            }
            const auto path = directory.filePath("uav.gpa_frame");
            graphicsCounterCapture(true).save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            options.until = 200;
            options.before = true;
            options.shaders[30] = compileClassProgram(
                "RWStructuredBuffer<uint> data:register(u1);"
                "float4 main(uint id:SV_VertexID):SV_Position {uint n=data.IncrementCounter();data[n]=100;"
                "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}",
                "vs_5_0");
            Replay replay(frame, options);
            replay.run();
            const auto storage = replay.readBuffer(10), target = replay.readTexture(20);
            const auto counter = replay.readCounter(12);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                const auto captured = inspectPostTransform(replay, 200, inspect);
                QCOMPARE(captured.identities.size(), size_t(3));
                QCOMPARE(replay.readBuffer(10), storage);
                QCOMPARE(replay.readTexture(20), target);
                QCOMPARE(replay.readCounter(12), counter);
            }
            replay.inspectNativeState([](auto context, const auto &) { context->Draw(3, 0); });
            options.before = false;
            Replay reference(frame, options);
            reference.run();
            QCOMPARE(replay.readBuffer(10), reference.readBuffer(10));
            QCOMPARE(replay.readCounter(12), reference.readCounter(12));
            for (const bool visible : {false, true}) {
                const auto predicatePath =
                    directory.filePath(visible ? "visible.gpa_frame" : "hidden.gpa_frame");
                predicateCapture(visible, 0).save(predicatePath);
                Frame predicateFrame(predicatePath.toStdWString());
                ReplayOptions predicateOptions;
                predicateOptions.warp = !vendor;
                if (vendor)
                    predicateOptions.vendor = vendor;
                predicateOptions.until = 2000;
                predicateOptions.before = true;
                Replay predicated(predicateFrame, predicateOptions);
                predicated.run();
                const auto before = predicated.readTexture(20);
                const auto vertices = inspectPostTransform(predicated, 2000, inspect);
                QCOMPARE(vertices.identities.size(), visible ? size_t(3) : size_t(0));
                QCOMPARE(predicated.readTexture(20), before);
                predicated.inspectNativeState([](auto context, const auto &) { context->Draw(3, 0); });
                predicateOptions.before = false;
                Replay expected(predicateFrame, predicateOptions);
                expected.run();
                QCOMPARE(predicated.readTexture(20), expected.readTexture(20));
                predicateOptions.before = true;
                predicateOptions.disabled.insert(2000);
                Replay disabled(predicateFrame, predicateOptions);
                disabled.run();
                const auto empty = inspectPostTransform(disabled, 2000, inspect);
                QVERIFY(empty.identities.empty());
                QVERIFY(empty.bytes.empty());
                QVERIFY(postTransformTables(empty).at("tables").at("input_references").at("rows").empty());
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativeVertexInputsTests)
#include "NativeVertexInputsTests.moc"
