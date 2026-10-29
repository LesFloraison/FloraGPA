#include "HullCapture.h"
#include "UavGeometryCapture.h"
#include "application/DxbcGeometryDependencies.h"
#include "application/PostTransform.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
class GeometryUavDependencyTests : public QObject {
    Q_OBJECT
  private slots:
    void originalHull() {
        QTemporaryDir directory;
        const auto path = directory.filePath("hull.gpa_frame");
        testing::hullCapture().save(path);
        Frame frame(path.toStdWString());
        for (Id id : {7000, 7002, 7004}) {
            const auto proof = geometryUavIndependence(frame.shader(frame.resource(id).data));
            QVERIFY2(proof.at("proved") == true, proof.dump().c_str());
            if (id == 7002)
                QCOMPARE(proof.at("write_instructions").size(), size_t(2));
        }
    }
    void access_data() {
        QTest::addColumn<QString>("body");
        QTest::addColumn<bool>("proved");
        QTest::newRow("store") << QString("dst[id]=id;") << true;
        QTest::newRow("atomic-no-return") << QString("InterlockedAdd(dst[id],1);") << true;
        QTest::newRow("counter-null") << QString("dst.IncrementCounter();dst[id]=id;") << true;
        QTest::newRow("counter-return") << QString("id=dst.IncrementCounter();") << false;
        QTest::newRow("load") << QString("id=dst[id];") << true;
        QTest::newRow("atomic-return") << QString("uint x;InterlockedAdd(dst[id],1,x);id=x;") << false;
    }
    void access() {
        QFETCH(QString, body);
        QFETCH(bool, proved);
        const auto bytes = testing::compileClassProgram(
            "RWStructuredBuffer<uint> dst:register(u1);float4 main(uint id:SV_VertexID):SV_Position{" +
                body.toStdString() + "return float4(id,0,0,1);}",
            "vs_5_0");
        const auto proof = geometryUavIndependence(bytes);
        QVERIFY2(proof.at("proved") == proved, proof.dump().c_str());
        if (body == "id=dst[id];") {
            QCOMPARE(proof.at("scope"), nlohmann::json("immutable_resource_inputs"));
            QCOMPARE(proof.at("read_dependencies").at("uav_slots"), nlohmann::json::array({1}));
        }
    }
    void liveness_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("proved");
        for (const auto mode : {"dead", "overwrite", "immediate"})
            QTest::newRow(mode) << QString(mode) << true;
        for (const auto mode : {"read", "read-before-write", "partial", "branch", "relative", "srv", "call"})
            QTest::newRow(mode) << QString(mode) << false;
    }
    void resourceFormats_data() {
        QTest::addColumn<QString>("declaration");
        QTest::addColumn<QString>("expression");
        QTest::addColumn<bool>("uav");
        for (bool uav : {false, true})
            for (const auto kind : {"StructuredBuffer<uint>", "ByteAddressBuffer", "Buffer<uint>"}) {
                const auto name = QString(kind);
                QTest::newRow(qPrintable(name + (uav ? "-uav" : "-srv")))
                    << QString("%1%2 src:register(%3);").arg(uav ? "RW" : "").arg(name).arg(uav ? "u2" : "t0")
                    << QString(name == "ByteAddressBuffer" ? "src.Load(id*4)" : "src[id]") << uav;
            }
        QTest::newRow("sample") << QString("Texture2D<float4> src:register(t0);SamplerState s:register(s0);")
                                << QString("asuint(src.SampleLevel(s,float2(id,0),0).x)") << false;
    }
    void resourceFormats() {
        QFETCH(QString, declaration);
        QFETCH(QString, expression);
        QFETCH(bool, uav);
        const auto bytes = testing::compileClassProgram(
            "RWStructuredBuffer<uint> dst:register(u1);" + declaration.toStdString() +
                "float4 main(uint id:SV_VertexID):SV_Position{uint v=" + expression.toStdString() +
                ";InterlockedAdd(dst[0],v);return float4(v,0,0,1);}",
            "vs_5_0");
        const auto proof = geometryUavIndependence(bytes);
        QVERIFY2(proof.at("proved") == true, proof.dump().c_str());
        QCOMPARE(proof.at("scope"), nlohmann::json("immutable_resource_inputs"));
        QCOMPARE(proof.at("read_dependencies").at(uav ? "uav_slots" : "srv_slots"),
                 nlohmann::json::array({uav ? 2 : 0}));
        QCOMPARE(proof.at("write_instructions").size(), size_t(1));
        QCOMPARE(proof.at("write_instructions")[0].at("slot"), nlohmann::json(1));
        for (const auto &[tag, body] : readDxbcParts(bytes)) {
            if (tag != 0x58454853 && tag != 0x52444853)
                continue;
            auto program = readDxbcProgram(body);
            auto found = std::find_if(program.instructions.begin(), program.instructions.end(),
                                      [](const auto &row) { return bool(row[0] & 0x80000000u); });
            QVERIFY(found != program.instructions.end());
            const auto index = size_t(found - program.instructions.begin());
            for (bool reserved : {false, true}) {
                auto changed = program;
                auto &extension = changed.instructions[index][1];
                extension = reserved ? extension | 0x40000000u : (extension & ~63u) | 63u;
                const auto encoded = writeDxbcProgram(changed);
                const auto rejected = geometryUavIndependence(makeDxbc({{tag, encoded}}));
                QVERIFY(rejected.at("proved") == false);
                QCOMPARE(rejected.at("reason"),
                         nlohmann::json("Unsupported or malformed instruction extension"));
            }
        }
    }
    void liveness() {
        QFETCH(QString, mode);
        QFETCH(bool, proved);
        const auto bytes = testing::compileClassProgram(
            "RWStructuredBuffer<uint> dst:register(u1);float4 main(uint id:SV_VertexID):SV_Position{"
            "dst.IncrementCounter();return float4(id,0,0,1);}",
            "vs_5_0");
        std::vector<uint8_t> rewritten;
        for (const auto &[tag, body] : readDxbcParts(bytes)) {
            if (tag != 0x58454853 && tag != 0x52444853)
                continue;
            auto program = readDxbcProgram(body);
            const auto found = std::find_if(program.instructions.begin(), program.instructions.end(),
                                            [](const auto &row) { return (row[0] & 2047) == 178; });
            QVERIFY(found != program.instructions.end());
            const auto index = size_t(found - program.instructions.begin());
            QCOMPARE(found->size(), size_t(5));
            const auto destination = found->at(1), temporary = found->at(2);
            const auto mask = (destination >> 4) & 15;
            QVERIFY(mask && !(mask & (mask - 1)));
            uint32_t component = 0;
            while (!(mask & (1u << component)))
                ++component;
            const auto source = 0x0010000au | (component << 4);
            program.instructions.resize(index + 1);
            auto append = [&](std::vector<uint32_t> row) {
                row[0] |= uint32_t(row.size()) << 24;
                program.instructions.push_back(std::move(row));
            };
            if (mode == "overwrite" || mode == "immediate")
                append({54, destination, temporary, 0x4001, mode == "immediate" ? source : 42});
            if (mode == "read" || mode == "read-before-write" || mode == "partial") {
                if (mode == "partial")
                    append({54, 0x00100002u | ((mask == 1 ? 2u : 1u) << 4), temporary, 0x4001, 42});
                append({54, mode == "read-before-write" ? destination : 0x00102012u,
                        mode == "read-before-write" ? temporary : 0, source, temporary});
            }
            if (mode == "branch") {
                append({31, 0x4001, 1});
                append({54, destination, temporary, 0x4001, 42});
                append({21});
            }
            if (mode == "relative")
                append({54, 0x00102012, 0, 0x0620800a, 0, 0, source, temporary});
            if (mode == "srv")
                append({88, 0x00107000, 0, 0x4444});
            if (mode == "call")
                append({4, 0x0010a000, 0});
            append({62});
            rewritten = writeDxbcProgram(program);
        }
        QVERIFY(!rewritten.empty());
        const auto proof = geometryUavIndependence(makeDxbc({{0x58454853, rewritten}}));
        QVERIFY2(proof.at("proved") == proved, proof.dump().c_str());
    }
    void isolatedInstances_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto mode : {"independent", "read", "ia-alias", "gs", "gs-empty", "gs-read-uav",
                                    "gs-read-srv", "gs-unbound", "gs-alias-uav", "gs-alias-srv"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(mode))) << vendor << QString(mode);
    }
    void isolatedInstances() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            QTemporaryDir directory;
            const auto path = directory.filePath("hull.gpa_frame");
            const bool gs = mode.startsWith("gs"), unbound = mode == "gs-unbound";
            const bool rejectedCase = mode == "read" || mode.contains("alias");
            auto fixture = mode == "read"                       ? testing::hullUavReadCapture()
                           : mode == "gs" || mode == "gs-empty" ? testing::geometryUavEmissionCapture()
                                                                : testing::hullCapture();
            if (gs && mode != "gs" && mode != "gs-empty")
                fixture = testing::geometryResourceReadCapture(
                    mode != "gs-read-srv" && mode != "gs-alias-srv", mode.contains("alias"), unbound);
            if (mode == "ia-alias") {
                fixture.save(path);
                Frame original(path.toStdWString());
                auto state = original.state(199);
                fixture.buffer(9000, 9001, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, 0,
                               {0, 0, 0, 0});
                auto view = testing::statePack(Id(0), Id(0), Id(9000));
                D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT_R32_UINT;
                desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                desc.Buffer.NumElements = 4;
                testing::append(view, desc);
                fixture.add(9002, 5, 0x8f, view);
                state.vb[0] = 9000;
                state.strides[0] = 4;
                state.rtv[2] = 9002;
                state.rtCount = 3;
                const auto code =
                    testing::compileClassProgram("RWBuffer<uint> dst:register(u2);float4 main(uint "
                                                 "id:SV_VertexID,uint inst:SV_InstanceID):SV_Position{"
                                                 "InterlockedAdd(dst[0],1);return float4(id,inst,0,1);}",
                                                 "vs_5_0");
                std::erase_if(fixture.entries, [](const auto &entry) { return entry.id == 7001; });
                auto payload = testing::statePack(uint64_t(code.size()));
                payload.insert(payload.end(), code.begin(), code.end());
                testing::append(payload, Id(0));
                fixture.add(7001, 9, 0x81, payload);
                std::erase_if(fixture.entries, [](const auto &entry) { return entry.id == 199; });
                fixture.add(199, 3, 3, testing::snapshot(state));
            }
            fixture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.until = 200;
            options.before = true;
            options.warp = vendor == 0;
            if (vendor)
                options.vendor = vendor;
            if (mode == "gs-empty")
                options.shaders[8000] = testing::compileClassProgram(
                    "struct V{float4 p:SV_Position;};RWStructuredBuffer<uint> dst:register(u1);"
                    "[maxvertexcount(6)]void main(point V v[1],inout TriangleStream<V> output){"
                    "InterlockedAdd(dst[0],1);uint n=uint(v[0].p.y);"
                    "for(uint j=0;j<n;++j){for(uint i=0;i<3;++i){V o;o.p=v[0].p;"
                    "o.p.x=float(j*3+i);output.Append(o);}output.RestartStrip();}}",
                    "gs_5_0");
            Replay replay(frame, options);
            replay.run();
            const auto before = replay.readBuffer(10);
            const auto extraBefore = mode == "ia-alias" || (gs && mode != "gs" && mode != "gs-empty")
                                         ? replay.readBuffer(9000)
                                         : std::vector<uint8_t>{};
            const auto counter = replay.readCounter(12);
            const auto commands = replay.counts;
            const auto full = inspectPostTransform(replay, 200);
            const auto pixelGeometry = inspectPixelGeometry(replay, 200);
            QCOMPARE(pixelGeometry.bytes, full.bytes);
            const auto partition = validatedPixelGeometryInstances(pixelGeometry);
            QCOMPARE(partition.resolved, !rejectedCase);
            if (!rejectedCase)
                QCOMPARE(full.report.at("vertices"),
                         nlohmann::json(gs ? (unbound || mode == "gs-empty" ? 9 : 18) : 12));
            else
                QVERIFY(full.report.at("vertices").get<unsigned>() > 0);
            nlohmann::json selections = nlohmann::json::array();
            for (uint32_t instance = 0; instance < (gs ? 3u : 2u); ++instance) {
                PostTransformOptions inspect;
                inspect.instance = instance;
                if (rejectedCase) {
                    bool rejected = false;
                    try {
                        inspectPostTransform(replay, 200, inspect);
                    } catch (const std::runtime_error &error) {
                        rejected = QString::fromUtf8(error.what())
                                       .contains(mode == "ia-alias" ? "IA/CB input aliases a written UAV"
                                                                    : "Read resource aliases a written UAV");
                    }
                    QVERIFY(rejected);
                } else {
                    const auto selected = inspectPostTransform(replay, 200, inspect);
                    const auto &selection = selected.report.at("instance_selection");
                    QVERIFY(selection.at("uav_independence").at("input_aliases_excluded") == true);
                    const auto first = mode == "gs-empty" ? instance * (instance ? instance - 1 : 0) / 2 * 3
                                       : unbound          ? instance * 3
                                       : gs               ? instance * (instance + 1) / 2 * 3
                                                          : instance * 6;
                    const auto last = mode == "gs-empty" ? instance * (instance + 1) / 2 * 3
                                      : unbound          ? (instance + 1) * 3
                                      : gs               ? (instance + 1) * (instance + 2) / 2 * 3
                                                         : (instance + 1) * 6;
                    QCOMPARE(selection.at("first_vertex"), nlohmann::json(first));
                    QCOMPARE(selected.bytes, std::vector<uint8_t>(full.bytes.begin() + first * 16,
                                                                  full.bytes.begin() + last * 16));
                    selections.push_back(selection);
                    for (uint64_t ordinal = first / 3; ordinal < last / 3; ++ordinal)
                        QCOMPARE(partition.instance(ordinal), std::optional<uint32_t>(instance));
                }
                QCOMPARE(replay.readBuffer(10), before);
                if (!extraBefore.empty())
                    QCOMPARE(replay.readBuffer(9000), extraBefore);
                QCOMPARE(replay.readCounter(12), counter);
                QVERIFY(replay.counts == commands);
            }
            replay.inspectNativeState([&](auto context, const auto &) {
                if (gs)
                    context->DrawInstanced(1, 3, 0, 0);
                else
                    context->DrawInstanced(6, 2, 0, 9);
            });
            const auto after = replay.readBuffer(10);
            const auto extraAfter = extraBefore.empty() ? std::vector<uint8_t>{} : replay.readBuffer(9000);
            const auto afterCounter = replay.readCounter(12);
            options.before = false;
            Replay clean(frame, options);
            clean.run();
            QCOMPARE(after, clean.readBuffer(10));
            QCOMPARE(afterCounter, clean.readCounter(12));
            if (!extraBefore.empty())
                QCOMPARE(extraAfter, clean.readBuffer(9000));
            const auto root = qEnvironmentVariable("FLORA_GEOMETRY_UAV_EVIDENCE");
            if (!root.isEmpty()) {
                QVERIFY(QDir().mkpath(root));
                nlohmann::json evidence{{"schema", 1},
                                        {"vendor", vendor},
                                        {"mode", mode.toStdString()},
                                        {"frame_sha256", frame.sha256()},
                                        {"full", full.report},
                                        {"pixel_geometry", pixelGeometry.report},
                                        {"selections", selections},
                                        {"before_sha256", sha256(before)},
                                        {"after_sha256", sha256(after)},
                                        {"counter_before", counter},
                                        {"counter_after", afterCounter},
                                        {"storage_restored", true},
                                        {"counter_restored", true},
                                        {"clean_draw_equal", true}};
                QFile file(root + '/' + QString::fromLatin1(QTest::currentDataTag()) + ".json");
                QVERIFY(file.open(QIODevice::WriteOnly));
                const auto bytes = QByteArray::fromStdString(evidence.dump(2));
                QCOMPARE(file.write(bytes), bytes.size());
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(GeometryUavDependencyTests)
#include "GeometryUavDependencyTests.moc"
