#include "SyntheticCapture.h"
#include "application/DxbcExecutionTrace.h"
#include "application/HlslCompilation.h"
#include "application/ShaderSourceLines.h"
#include "core/Dxbc.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
std::vector<Json> read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read trace test output");
    std::vector<Json> rows;
    while (!file.atEnd())
        rows.push_back(Json::parse(file.readLine().toStdString()));
    return rows;
}
std::vector<uint8_t> bytes(const Json &hex) {
    const auto raw = QByteArray::fromHex(QByteArray::fromStdString(hex.get<std::string>()));
    return {raw.begin(), raw.end()};
}
struct Projection {
    std::map<std::string, Json> registers, counters;
    std::map<std::pair<std::string, uint64_t>, Buffer> storage;
    void initial(const Json &r) {
        const auto kind = r.at("record");
        if (kind == "register")
            registers[r.at("lane").dump() + ":" + r.at("register").dump()] = r.at("value");
        if (kind == "counter")
            counters[r.at("view").dump()] = r.at("value");
        if (kind == "storage") {
            const auto size = r.at("bytes").get<size_t>();
            storage[{r.at("space"), r.at("resource")}] = {std::vector<uint8_t>(size),
                                                          std::vector<uint8_t>(size)};
        }
        if (kind == "storage_bytes") {
            auto &b = storage.at({r.at("space"), r.at("resource")});
            const auto offset = r.at("offset").get<size_t>();
            const auto data = bytes(r.at("bits_hex")), valid = bytes(r.at("valid_hex"));
            std::copy(data.begin(), data.end(), b.bytes.begin() + offset);
            std::copy(valid.begin(), valid.end(), b.valid.begin() + offset);
        }
    }
    void apply(const Json &r, bool forward) {
        const char *before = forward ? "before" : "after", *after = forward ? "after" : "before";
        auto registersDelta = r.at("register_changes"), countersDelta = r.at("counter_changes");
        if (!forward) {
            std::reverse(registersDelta.begin(), registersDelta.end());
            std::reverse(countersDelta.begin(), countersDelta.end());
        }
        for (const auto &c : registersDelta) {
            auto &v = registers[r.at("lane").dump() + ":" + c.at("register").dump()];
            if (v.is_null())
                v = {{"bits", {0, 0, 0, 0}}, {"valid_components", 0}};
            if (v != c.at(before))
                throw std::runtime_error("Trace register chain mismatch");
            v = c.at(after);
        }
        for (const auto &c : countersDelta) {
            auto &v = counters.at(c.at("view").dump());
            if (v != c.at(before))
                throw std::runtime_error("Trace counter chain mismatch");
            v = c.at(after);
        }
        for (const char *space : {"buffer", "shared", "texture"}) {
            auto delta = r.at(std::string(space) + "_changes");
            if (!forward)
                std::reverse(delta.begin(), delta.end());
            for (const auto &c : delta) {
                auto &b = storage.at({space, c.at("resource")});
                const auto offset = c.at("offset").get<size_t>();
                const auto prior = bytes(c.at(std::string(before) + "_hex"));
                const auto priorValid = bytes(c.at(std::string(before) + "_valid_hex"));
                const auto next = bytes(c.at(std::string(after) + "_hex"));
                const auto nextValid = bytes(c.at(std::string(after) + "_valid_hex"));
                if (offset > b.bytes.size() || prior.size() > b.bytes.size() - offset ||
                    !std::equal(prior.begin(), prior.end(), b.bytes.begin() + offset) ||
                    !std::equal(priorValid.begin(), priorValid.end(), b.valid.begin() + offset))
                    throw std::runtime_error("Trace storage chain mismatch");
                std::copy(next.begin(), next.end(), b.bytes.begin() + offset);
                std::copy(nextValid.begin(), nextValid.end(), b.valid.begin() + offset);
            }
        }
    }
};
InputSnapshot fixture() {
    InputSnapshot s;
    s.stage = 5;
    s.event = 19;
    s.shader = 7;
    s.bytecode = compileHlsl(R"(
        RWByteAddressBuffer output:register(u0);
        groupshared uint g[4];
        [numthreads(2,2,1)]void main(uint i:SV_GroupIndex,uint3 group:SV_GroupID){
            g[i]=i+group.x; GroupMemoryBarrierWithGroupSync();
            uint v=g[(i+1)%4];
            for(uint turn=0;turn<3;++turn)v=v*3+turn;
            output.Store(i*4,v);
        })",
                             "cs_5_0")
                     .bytecode;
    s.inputs.memory = std::make_shared<Memory>(
        std::map<uint64_t, Buffer>{{11, Buffer::known(std::vector<uint8_t>(16, 0x8d))}},
        std::map<Register, BufferView>{{{30, {0}}, {11, 0, 16, 0}}});
    return s;
}
} // namespace
class DxbcExecutionTraceTests : public QObject {
    Q_OBJECT
  private slots:
    void losslessStep() {
        GroupStep group{UINT64_MAX, 17, 3, {}, true};
        auto &s = group.step;
        s.sequence = 23;
        s.instruction = 29;
        s.wordOffset = 97;
        s.nextInstruction = 30;
        s.callstack = {31, 41};
        s.changes = {{{0, {7}},
                      {{0x80000000, 0x7fc0abcd, 0xffffffff, 0}, 5},
                      {{0, 0xff800000, 0x12345678, 0x7f800000}, 10}}};
        const MemoryChange c{UINT64_MAX, 9, {0, 0xff}, {0x80, 1}, {1, 0}, {0, 1}};
        s.memoryChanges = {c};
        s.sharedMemoryChanges = {c};
        s.textureChanges = {c};
        s.counterChanges = {{17, {CounterKind::Append, {}}, {CounterKind::Ordered, UINT32_MAX}}};
        s.syncFlags = 15;
        s.syncLoopIterations = {{33, UINT64_MAX}};
        s.discarded = s.memoryGpuAssisted = s.instructionGpuAssisted = s.atomic = true;
        const auto row = Json::parse(executionTraceStep(group).dump());
        QCOMPARE(row.at("sequence").get<uint64_t>(), UINT64_MAX);
        QCOMPARE(row.at("register_changes")[0]["before"]["bits"][1], Json(0x7fc0abcdu));
        QCOMPARE(row.at("register_changes")[0]["before"]["valid_components"], Json(5));
        QCOMPARE(row.at("register_changes")[0]["after"]["valid_components"], Json(10));
        QCOMPARE(row.at("return_instruction_stack"), Json({31, 41}));
        QCOMPARE(row.at("next_instruction"), Json(30));
        for (const auto *key : {"buffer_changes", "shared_changes", "texture_changes"}) {
            QCOMPARE(row.at(key)[0]["before_hex"], Json("00ff"));
            QCOMPARE(row.at(key)[0]["after_hex"], Json("8001"));
            QCOMPARE(row.at(key)[0]["before_valid_hex"], Json("0100"));
            QCOMPARE(row.at(key)[0]["after_valid_hex"], Json("0001"));
        }
        QVERIFY(row.at("counter_changes")[0]["before"]["value"].is_null());
        QCOMPARE(row.at("counter_changes")[0]["after"]["value"], Json(UINT32_MAX));
        QCOMPARE(row.at("sync_loop_iterations"), Json({{33, UINT64_MAX}}));
        for (const char *key :
             {"released_barrier", "atomic", "discarded", "memory_gpu_assisted", "instruction_gpu_assisted"})
            QCOMPARE(row.at(key), Json(true));
        s.nextInstruction.reset();
        QVERIFY(executionTraceStep(group).at("next_instruction").is_null());
        s.changes[0].after.valid = 16;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionTraceStep(group));
        s.changes[0].after.valid = 1;
        s.memoryChanges[0].after.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionTraceStep(group));
        s.memoryChanges[0] = c;
        s.memoryChanges[0].afterValid[0] = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, executionTraceStep(group));
    }
    void cooperativeRoundTrip() {
        QTemporaryDir directory;
        auto input = fixture();
        const auto initial = input.inputs.memory->buffers().at(11);
        GroupTraceOptions options;
        options.group = {7, 3, 1};
        options.thread = {1, 1, 0};
        const auto path = directory.filePath("trace.jsonl");
        const auto summary = exportGroupExecutionTrace(path, input, options);
        const auto rows = read(path);
        QCOMPARE(summary.at("bytes").get<qint64>(), QFileInfo(path).size());
        QCOMPARE(summary.at("records").get<size_t>(), rows.size());
        QCOMPARE(rows.front().at("selected_lane"), Json(3));
        QCOMPARE(rows.front().at("captured_gpu_schedule"), Json(false));
        QCOMPARE(rows.back().at("completed"), Json(true));
        Projection projection;
        std::vector<Json> steps;
        uint64_t sequence = 0, barriers = 0;
        for (const auto &r : rows) {
            if (r.at("record") != "step")
                projection.initial(r);
            else {
                QCOMPARE(r.at("sequence").get<uint64_t>(), sequence++);
                barriers += r.at("released_barrier").get<bool>();
                projection.apply(r, true);
                steps.push_back(r);
            }
        }
        QCOMPARE(barriers, uint64_t(1));
        QCOMPARE(projection.storage.at({"buffer", 11}).bytes, input.inputs.memory->buffers().at(11).bytes);
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            projection.apply(*it, false);
        QCOMPARE(projection.storage.at({"buffer", 11}).bytes, initial.bytes);
        QCOMPARE(projection.storage.at({"buffer", 11}).valid, initial.valid);
        const auto &shared = projection.storage.at({"shared", 0});
        QVERIFY(std::all_of(shared.valid.begin(), shared.valid.end(), [](auto v) { return v == 0; }));
        for (const auto &r : rows)
            if (r.at("record") == "register")
                QCOMPARE(projection.registers.at(r.at("lane").dump() + ":" + r.at("register").dump()),
                         r.at("value"));
    }
    void embeddedSource_data() {
        QTest::addColumn<bool>("debug");
        QTest::newRow("embedded") << true;
        QTest::newRow("stripped") << false;
    }
    void embeddedSource() {
        QFETCH(bool, debug);
        const char source[] = "RWByteAddressBuffer output:register(u0);\n"
                              "[numthreads(1,1,1)]\nvoid main(){\n"
                              "uint v=output.Load(0);\noutput.Store(0,v+7);\n}\n";
        Com<ID3DBlob> blob;
        check(D3DCompile(source, sizeof(source) - 1, "trace-source.hlsl", nullptr, nullptr, "main", "cs_5_0",
                         debug ? D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION : 0, 0, &blob, nullptr),
              "Compile trace source fixture");
        auto input = fixture();
        const auto *start = static_cast<const uint8_t *>(blob->GetBufferPointer());
        input.bytecode.assign(start, start + blob->GetBufferSize());
        QTemporaryDir directory;
        const auto path = directory.filePath("trace.jsonl");
        exportGroupExecutionTrace(path, input);
        const auto rows = read(path);
        const auto expected = shaderSourceLines(input.bytecode);
        const auto locations = shaderLinesByOffset(expected);
        QCOMPARE(rows.at(1).at("data"), expected);
        size_t mapped = 0;
        for (const auto &r : rows)
            if (r.at("record") == "instruction") {
                const auto key = std::to_string(r.at("word_offset").get<uint64_t>() * 4);
                QCOMPARE(r.at("source"), locations.value(key, Json(nullptr)));
                mapped += !r.at("source").is_null();
            }
        if (debug)
            QVERIFY(mapped > 0);
        else
            QCOMPARE(mapped, size_t(0));
    }
    void inputObservationBits() {
        auto input = fixture();
        auto textures = std::make_shared<TextureInputs>();
        input.inputs.textures = textures;
        TextureInput t;
        t.storage = Buffer::known(std::vector<uint8_t>(8193, 0xa5));
        t.storage.valid[4096] = 0;
        textures->resources[91] = t;
        textures->metadata[91].minLod = -0.0f;
        auto &view = textures->views[{7, {3}}];
        view.resource = 91;
        view.depthSlices = {2, 3};
        view.planeSlice = 1;
        view.nativeDescriptor = {0x80000000, UINT32_MAX};
        auto samples = std::make_shared<TextureInputView::Samples>();
        samples->values.resize(513, {0x80000000, 0x7fc0abcd, UINT32_MAX, 0});
        view.samples = samples;
        auto queries = std::make_shared<SampleQueryInput>();
        queries->observed = true;
        queries->positions = {{0x80000000, 0x7fc0abcd, 0, 0}};
        view.queries = queries;
        textures->samplers[{6, {2}}].descriptor = std::array<uint32_t, 13>{0x80000000, UINT32_MAX};
        textures->samplers[{6, {3}}] = {};
        input.unavailable[{7, {4}}] = "Unobserved test binding";
        input.unavailableCounters[92] = "Unobserved test counter";
        QTemporaryDir directory;
        const auto path = directory.filePath("trace.jsonl");
        exportGroupExecutionTrace(path, input);
        Projection projection;
        size_t chunks = 0, values = 0;
        bool metadata = false, sampler = false, nullSampler = false, query = false, unavailable = false;
        for (const auto &r : read(path)) {
            projection.initial(r);
            if (r.at("record") == "storage_bytes" && r.at("space") == "texture")
                ++chunks;
            if (r.at("record") == "multisample_values") {
                QCOMPARE(r.at("first").get<size_t>(), values);
                for (const auto &v : r.at("bits"))
                    QCOMPARE(v, Json(samples->values.at(values++)));
            }
            if (r.at("record") == "texture_metadata") {
                QCOMPARE(r.at("min_lod_bits"), Json(0x80000000u));
                metadata = true;
            }
            if (r.at("record") == "sample_queries") {
                QCOMPARE(r.at("position_bits"), Json(queries->positions));
                query = true;
            }
            if (r.at("record") == "sampler") {
                if (r.at("register").at("indices")[0] == 2) {
                    QCOMPARE(r.at("d3d11_descriptor_bits"),
                             Json(*textures->samplers.at({6, {2}}).descriptor));
                    sampler = true;
                } else {
                    QVERIFY(r.at("d3d11_descriptor_bits").is_null());
                    nullSampler = true;
                }
            }
            if (r.at("record") == "unavailable_binding")
                unavailable = true;
        }
        QCOMPARE(chunks, size_t(3));
        QCOMPARE(values, size_t(513));
        QVERIFY(metadata && sampler && nullSampler && query && unavailable);
        QCOMPARE(projection.storage.at({"texture", 91}).bytes, t.storage.bytes);
        QCOMPARE(projection.storage.at({"texture", 91}).valid, t.storage.valid);
    }
    void failurePublication_data() {
        QTest::addColumn<QString>("failure");
        for (const auto *name :
             {"bytes", "steps", "cancel", "late-cancel", "thread", "group", "stage", "existing"})
            QTest::newRow(name) << QString(name);
    }
    void failurePublication() {
        QFETCH(QString, failure);
        QTemporaryDir directory;
        const auto path = directory.filePath("trace.jsonl");
        auto input = fixture();
        GroupTraceOptions options;
        if (failure == "bytes")
            options.maxBytes = 4096;
        if (failure == "steps")
            options.maxSteps = 2;
        if (failure == "thread")
            options.thread[0] = 2;
        if (failure == "group")
            options.group[0] = 65535;
        if (failure == "stage")
            input.stage = 0;
        if (failure == "existing") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("existing"), qint64(8));
        }
        unsigned checks = 0;
        unsigned late = 0;
        if (failure == "late-cancel") {
            auto countInput = fixture();
            exportGroupExecutionTrace(directory.filePath("count.jsonl"), countInput, {}, [&] {
                ++late;
                return false;
            });
            QVERIFY(late > 10);
            late -= 4;
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportGroupExecutionTrace(path, input, options, [&] {
                                     ++checks;
                                     return (failure == "cancel" && checks >= 50) ||
                                            (failure == "late-cancel" && checks >= late);
                                 }));
        if (failure == "existing") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("existing"));
        } else
            QVERIFY(!QFileInfo::exists(path));
        auto fresh = fixture();
        QVERIFY(exportGroupExecutionTrace(directory.filePath("retry.jsonl"), fresh).at("completed") == true);
    }
    void capturedInputs_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("ordered");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool ordered : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(ordered))) << vendor << ordered;
    }
    void capturedInputs() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, ordered);
        QTemporaryDir directory;
        const auto capture = directory.filePath("capture.gpa_frame");
        testing::computeCapture(ordered).save(capture);
        Frame frame(capture.toStdWString());
        ReplayOptions config;
        config.until = 100;
        config.before = true;
        config.warp = !vendor;
        if (vendor)
            config.vendor = vendor;
        Replay replay(frame, config);
        const auto snapshot = snapshotResourceInputs(replay, 5);
        const auto original = replay.readBuffer(7);
        const auto counterBefore = replay.readCounter(12);
        const auto path = directory.filePath("trace.jsonl");
        const auto report = exportGroupExecutionTrace(path, snapshot);
        const auto rows = read(path);
        QCOMPARE(rows.front().at("frame_sha256"), Json(frame.sha256()));
        QCOMPARE(rows.front().at("gpa_event"), Json(100));
        QCOMPARE(rows.front().at("gpa_shader"), Json(snapshot.shader));
        QCOMPARE(rows.front().at("adapter").at("luid"), Json(snapshot.adapterLuid));
        Projection projection;
        std::vector<Json> steps;
        uint64_t counterChanges = 0;
        for (const auto &r : rows)
            if (r.at("record") == "step") {
                projection.apply(r, true);
                counterChanges += r.at("counter_changes").size();
                steps.push_back(r);
            } else
                projection.initial(r);
        QVERIFY(counterChanges > 0);
        for (const auto &[id, b] : snapshot.inputs.memory->buffers()) {
            QCOMPARE(projection.storage.at({"buffer", id}).bytes, b.bytes);
            QCOMPARE(projection.storage.at({"buffer", id}).valid, b.valid);
        }
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            projection.apply(*it, false);
        QCOMPARE(projection.counters.at("12").at("value"), Json(counterBefore));
        QCOMPARE(replay.readBuffer(7), original);
        QCOMPARE(replay.readCounter(12), counterBefore);
        QVERIFY(report.at("completed") == true);
        qInfo().noquote() << QString::fromStdString(report.dump());
    }
};
QTEST_GUILESS_MAIN(DxbcExecutionTraceTests)
#include "DxbcExecutionTraceTests.moc"
