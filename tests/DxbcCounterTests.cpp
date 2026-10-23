#include "SyntheticCapture.h"
#include "application/DxbcCounterSnapshot.h"
#include "application/Experiment.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words result{code};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words reg(uint32_t kind, uint32_t index, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), index};
}
Words imm(uint32_t value) { return {0x4001, value}; }
Program raw(std::vector<Words> rows) {
    rows.insert(rows.begin(), {op(104, {{4}}), op(155, {{1, 1, 1}})});
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50050, 0}, rows})}}));
}
Words declaration(bool ordered, uint32_t slot = 0) {
    return op(158 | (ordered ? 0x800000u : 0), {reg(30, slot), {4}});
}
Inputs inputs(CounterKind kind, std::optional<uint32_t> value) {
    Inputs result;
    result.memory = std::make_shared<Memory>(
        std::map<uint64_t, Buffer>{{7, Buffer::known(std::vector<uint8_t>(16, 0x42))}},
        std::map<Register, BufferView>{{{30, {0}}, {7, 0, 16, 4, 12}}}, 256 * 1024 * 1024, 16 * 1024 * 1024,
        nullptr, false, std::map<uint64_t, Counter>{{12, {kind, value}}});
    return result;
}
std::vector<uint32_t> words(const std::vector<uint8_t> &bytes, uint32_t count) {
    std::vector<uint32_t> result(count);
    std::memcpy(result.data(), bytes.data(), count * 4);
    return result;
}
std::string hash(Bytes bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
void replaceShader(testing::Capture &capture, Bytes shader) {
    std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 16; });
    std::vector<uint8_t> payload;
    testing::append(payload, uint64_t(shader.size()));
    payload.insert(payload.end(), shader.begin(), shader.end());
    testing::append(payload, Id(0));
    capture.add(16, 9, 0x81, payload);
}
void validateDevice(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Read counter validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read counter message size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read counter message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
} // namespace
class DxbcCounterTests final : public QObject {
    Q_OBJECT
  private slots:
    void arithmeticAndTrace() {
        for (bool ordered : {false, true})
            for (bool consume : {false, true})
                for (uint32_t initial : {0u, 3u, UINT32_MAX}) {
                    auto state = inputs(ordered ? CounterKind::Ordered : CounterKind::Append, initial);
                    const auto p = raw(
                        {declaration(ordered), op(consume ? 179 : 178, {reg(0, 0, 4), reg(30, 0)}), op(62)});
                    Lane lane(p, state);
                    const auto trace = lane.step();
                    const uint32_t after = consume ? initial - 1 : initial + 1;
                    QCOMPARE(lane.value({0, {0}}).valid, uint8_t(4));
                    QCOMPARE(lane.value({0, {0}}).bits[2], consume ? after : initial);
                    QCOMPARE(state.memory->counters().at(12).value, std::optional<uint32_t>(after));
                    QCOMPARE(trace.counterChanges.size(), size_t(1));
                    QVERIFY(trace.atomic);
                    QCOMPARE(trace.counterChanges[0].view, uint64_t(12));
                    QCOMPARE(trace.counterChanges[0].before.value, std::optional<uint32_t>(initial));
                    QCOMPARE(trace.counterChanges[0].after.value, std::optional<uint32_t>(after));
                    lane.step();
                    QVERIFY(lane.finished());
                }
    }
    void unknownAndUnbound() {
        auto p = raw({declaration(true), op(178, {reg(0, 0, 1), reg(30, 0)}), op(62)});
        auto state = inputs(CounterKind::Ordered, {});
        Lane unknown(p, state);
        const auto trace = unknown.step();
        QCOMPARE(unknown.value({0, {0}}).valid, uint8_t(0));
        QVERIFY(!state.memory->counters().at(12).value);
        QVERIFY(!trace.counterChanges[0].before.value);
        Inputs unbound;
        unbound.memory = std::make_shared<Memory>(std::map<uint64_t, Buffer>{},
                                                  std::map<Register, BufferView>{{{30, {0}}, {}}});
        Lane nullLane(p, unbound);
        QVERIFY(nullLane.step().counterChanges.empty());
        QCOMPARE(nullLane.value({0, {0}}).valid, uint8_t(0));
        Lane missing(p, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, missing.step());
    }
    void viewIdentity() {
        Inputs state;
        state.memory = std::make_shared<Memory>(
            std::map<uint64_t, Buffer>{{7, Buffer::known(std::vector<uint8_t>(16))}},
            std::map<Register, BufferView>{{{30, {0}}, {7, 0, 16, 4, 12}},
                                           {{30, {1}}, {7, 0, 16, 4, 12}},
                                           {{30, {2}}, {7, 0, 16, 4, 18}}},
            256 * 1024 * 1024, 16 * 1024 * 1024, nullptr, false,
            std::map<uint64_t, Counter>{{12, {CounterKind::Ordered, 3}}, {18, {CounterKind::Ordered, 19}}});
        const auto p = raw({declaration(true, 0), declaration(true, 1), declaration(true, 2),
                            op(178, {reg(0, 0, 1), reg(30, 0)}), op(178, {reg(0, 0, 2), reg(30, 1)}),
                            op(178, {reg(0, 0, 4), reg(30, 2)}), op(62)});
        Lane lane(p, state);
        while (!lane.finished())
            lane.step();
        QCOMPARE(lane.value({0, {0}}), (Value{{3, 4, 19, 0}, 7}));
        QCOMPARE(state.memory->counters().at(12).value, std::optional<uint32_t>(5));
        QCOMPARE(state.memory->counters().at(18).value, std::optional<uint32_t>(20));
        const auto mixed =
            raw({declaration(true, 0), declaration(true, 1), op(178, {reg(0, 0, 1), reg(30, 0)}),
                 op(179, {reg(0, 0, 2), reg(30, 1)}), op(62)});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Lane(mixed, state));
    }
    void validationAndRollback() {
        auto state = inputs(CounterKind::Ordered, 0);
        const auto append = raw({declaration(false), op(178, {reg(0, 0, 1), reg(30, 0)}), op(62)});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Lane(append, state));
        auto appendState = inputs(CounterKind::Append, 0);
        const auto ordinary = raw({declaration(false), op(62)});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Lane(ordinary, appendState));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 raw({declaration(true), op(178, {reg(0, 0, 1), reg(30, 0)}),
                                      op(179, {reg(0, 0, 1), reg(30, 0)}), op(62)}));
        for (bool noReturn : {false, true}) {
            auto rows =
                std::vector<Words>{declaration(true), op(178, {reg(0, 0, noReturn ? 1 : 3), reg(30, 0)})};
            if (!noReturn)
                rows.push_back(op(62));
            auto p = raw(rows);
            Lane lane(p, state);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
            QCOMPARE(state.memory->counters().at(12).value, std::optional<uint32_t>(0));
            QVERIFY(lane.registers().empty());
        }
    }
    void appendLifetimeAndValidity() {
        auto state = inputs(CounterKind::Append, 1);
        const auto initial = state.memory->buffers().at(7);
        QCOMPARE(initial.valid, (std::vector<uint8_t>{1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
        auto p = raw({declaration(false), op(178, {reg(0, 0, 1), reg(30, 0)}),
                      op(168, {reg(30, 0, 1), {0x10000a, 0}, imm(0), imm(17)}), op(62)});
        Lane first(p, state), second(p, state);
        const auto allocation = first.step();
        QCOMPARE(allocation.memoryChanges.size(), size_t(1));
        first.step();
        QCOMPARE(words(state.memory->buffers().at(7).bytes, 2)[1], 17u);
        second.step(); // This invocation owns index two, not index one.
        auto invalid = raw({declaration(false), op(178, {reg(0, 0, 1), reg(30, 0)}),
                            op(168, {reg(30, 0, 1), imm(1), imm(0), imm(99)}), op(62)});
        Lane alien(invalid, state);
        alien.step();
        const auto before = state.memory->buffers().at(7);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, alien.step());
        QCOMPARE(state.memory->buffers().at(7).bytes, before.bytes);
        QCOMPARE(state.memory->buffers().at(7).valid, before.valid);
        auto unknown = inputs(CounterKind::Append, {});
        QVERIFY(std::all_of(unknown.memory->buffers().at(7).valid.begin(),
                            unknown.memory->buffers().at(7).valid.end(), [](uint8_t v) { return v == 0; }));
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("ordered");
        QTest::addColumn<bool>("consume");
        QTest::addColumn<uint32_t>("threads");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool ordered : {false, true})
                for (bool consume : {false, true})
                    for (uint32_t threads : {1u, 4u})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4")
                                                     .arg(vendor)
                                                     .arg(ordered ? "ordered" : "append")
                                                     .arg(consume ? "consume" : "allocate")
                                                     .arg(threads)))
                            << vendor << ordered << consume << threads;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, ordered);
        QFETCH(bool, consume);
        QFETCH(uint32_t, threads);
        try {
            const auto type = ordered   ? "RWStructuredBuffer<uint>"
                              : consume ? "ConsumeStructuredBuffer<uint>"
                                        : "AppendStructuredBuffer<uint>";
            const std::string body =
                ordered ? (consume ? "uint i=data.DecrementCounter();output[tid]=data[i];"
                                   : "uint i=data.IncrementCounter();data[i]=100+tid;output[tid]=i;")
                        : (consume ? "output[tid]=data.Consume();" : "data.Append(100+tid);output[tid]=tid;");
            const auto source = std::string("RWStructuredBuffer<uint> output:register(u0);") + type +
                                " data:register(u1);[numthreads(" + std::to_string(threads) +
                                ",1,1)]void main(uint tid:SV_GroupIndex){" + body + "}";
            const auto shader = compileHlsl(source, "cs_5_0").bytecode;
            auto capture = testing::computeCapture(ordered);
            replaceShader(capture, shader);
            std::erase_if(capture.entries, [](const auto &e) { return e.id == 10 || e.id == 11; });
            capture.buffer(10, 11, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                           {11, 12, 13, 14});
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.path() + "/counter.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            Experiment experiment(frame);
            experiment.setUavCounter(frame, 12, consume ? 4 : 0, 100);
            ReplayOptions options;
            options.until = 100;
            options.before = true;
            options.warp = !vendor;
            options.debug = true;
            if (vendor)
                options.vendor = vendor;
            experiment.apply(frame, options);
            Replay before(frame, options);
            before.run();
            const auto originalCounter = before.readCounter(12);
            const auto snapshot = snapshotCounters(before, 100);
            QCOMPARE(snapshot.event, uint64_t(100));
            QVERIFY(snapshot.unavailable.empty());
            QCOMPARE(snapshot.bindings.at({30, {1}}), uint64_t(12));
            QCOMPARE(snapshot.counters.at(12).value, std::optional<uint32_t>(consume ? 4 : 0));
            QCOMPARE(before.readCounter(12), originalCounter);
            Inputs state;
            state.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{7, Buffer::known(before.readBuffer(7))},
                                           {10, Buffer::known(before.readBuffer(10))}},
                std::map<Register, BufferView>{{{30, {0}}, {7, 0, 16, 4}}, {{30, {1}}, {10, 0, 16, 4, 12}}},
                256 * 1024 * 1024, 16 * 1024 * 1024, nullptr, false, snapshot.counters);
            const auto initialBuffers = state.memory->buffers();
            Program program(shader);
            Group group(program, state);
            std::vector<GroupStep> trace;
            while (!group.finished())
                trace.push_back(group.step());
            options.before = false;
            Replay after(frame, options);
            after.run();
            const auto expectedCount = consume ? 4 - threads : threads;
            QCOMPARE(after.readCounter(12), expectedCount);
            QCOMPARE(state.memory->counters().at(12).value, std::optional<uint32_t>(expectedCount));
            const auto output = state.memory->buffers().at(7), storage = state.memory->buffers().at(10);
            const auto gpuOutput = after.readBuffer(7), gpuStorage = after.readBuffer(10);
            QVERIFY(std::all_of(output.valid.begin(), output.valid.end(), [](uint8_t v) { return v == 1; }));
            auto cpuValues = words(output.bytes, threads), gpuValues = words(gpuOutput, threads);
            std::sort(cpuValues.begin(), cpuValues.end());
            std::sort(gpuValues.begin(), gpuValues.end());
            std::vector<uint32_t> expected;
            for (uint32_t i = 0; i < threads; ++i)
                expected.push_back(consume ? 15 - threads + i : i);
            QCOMPARE(cpuValues, expected);
            QCOMPARE(gpuValues, expected);
            QCOMPARE(std::vector<uint8_t>(output.bytes.begin() + threads * 4, output.bytes.end()),
                     std::vector<uint8_t>(gpuOutput.begin() + threads * 4, gpuOutput.end()));
            if (!consume) {
                auto a = words(storage.bytes, threads), b = words(gpuStorage, threads);
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                for (uint32_t i = 0; i < threads; ++i) {
                    QCOMPARE(a[i], 100 + i);
                    QCOMPARE(b[i], 100 + i);
                }
                QVERIFY(std::all_of(storage.valid.begin(), storage.valid.begin() + threads * 4,
                                    [](uint8_t v) { return v == 1; }));
            }
            // Reverse every private mutation, including counter-only instructions.
            auto reversed = state.memory->buffers();
            auto counters = state.memory->counters();
            uint32_t changes = 0;
            for (auto step = trace.rbegin(); step != trace.rend(); ++step) {
                for (auto c = step->step.counterChanges.rbegin(); c != step->step.counterChanges.rend();
                     ++c) {
                    QCOMPARE(counters.at(c->view), c->after);
                    counters[c->view] = c->before;
                    ++changes;
                }
                for (auto c = step->step.memoryChanges.rbegin(); c != step->step.memoryChanges.rend(); ++c) {
                    auto &buffer = reversed.at(c->resource);
                    QCOMPARE(std::vector<uint8_t>(buffer.bytes.begin() + c->offset,
                                                  buffer.bytes.begin() + c->offset + c->after.size()),
                             c->after);
                    QCOMPARE(std::vector<uint8_t>(buffer.valid.begin() + c->offset,
                                                  buffer.valid.begin() + c->offset + c->afterValid.size()),
                             c->afterValid);
                    std::copy(c->before.begin(), c->before.end(), buffer.bytes.begin() + c->offset);
                    std::copy(c->beforeValid.begin(), c->beforeValid.end(), buffer.valid.begin() + c->offset);
                }
            }
            QCOMPARE(changes, threads);
            QCOMPARE(counters, snapshot.counters);
            for (const auto &[id, buffer] : initialBuffers) {
                QCOMPARE(reversed.at(id).bytes, buffer.bytes);
                QCOMPARE(reversed.at(id).valid, buffer.valid);
            }
            QCOMPARE(before.readCounter(12), originalCounter);
            QCOMPARE(before.readBuffer(7), initialBuffers.at(7).bytes);
            QCOMPARE(before.readBuffer(10), initialBuffers.at(10).bytes);
            validateDevice(before);
            validateDevice(after);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto frameBytes = file.readAll();
            const auto adapter = before.adapterDescription();
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json(
                    {{"probe", "dxbc-hidden-counter"},
                     {"ordered", ordered},
                     {"consume", consume},
                     {"threads", threads},
                     {"vendor", vendor},
                     {"adapter", QString::fromWCharArray(adapter.Description).toStdString()},
                     {"adapter_vendor", adapter.VendorId},
                     {"luid_low", adapter.AdapterLuid.LowPart},
                     {"luid_high", adapter.AdapterLuid.HighPart},
                     {"shader_sha256", hash(shader)},
                     {"frame_sha256",
                      QCryptographicHash::hash(frameBytes, QCryptographicHash::Sha256).toHex().toStdString()},
                     {"cpu_output_sha256", hash(output.bytes)},
                     {"gpu_output_sha256", hash(gpuOutput)},
                     {"cpu_storage_sha256", hash(storage.bytes)},
                     {"gpu_storage_sha256", hash(gpuStorage)},
                     {"counter_after", expectedCount},
                     {"steps", trace.size()},
                     {"counter_changes", changes},
                     {"comparison", "independent expected permutations; untouched output bytes exact"},
                     {"gpu_schedule_recovered", false},
                     {"passed", true}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void snapshotPreconditionsAndCancellation() {
        QTemporaryDir directory;
        auto capture = testing::computeCapture(true);
        const auto path = directory.path() + "/counter.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        options.uavCounters[100][12] = 3;
        Replay replay(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotCounters(replay, 100));
        replay.run();
        const auto initial = replay.readCounter(12);
        const auto bytes = replay.readBuffer(10);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotCounters(replay, 110));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotCounters(replay, 90));
        unsigned checks = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 snapshotCounters(replay, 100, [&] { return ++checks == 3; }));
        QCOMPARE(checks, 3u);
        QCOMPARE(replay.readCounter(12), initial);
        QCOMPARE(replay.readBuffer(10), bytes);
        QCOMPARE(snapshotCounters(replay, 100).counters.at(12).value, std::optional<uint32_t>(3));
        QCOMPARE(replay.readCounter(12), initial);
    }
    void wrapHardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("consume");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool consume : {false, true})
                QTest::newRow(
                    qPrintable(QString("%1-%2").arg(vendor).arg(consume ? "underflow" : "overflow")))
                    << vendor << consume;
    }
    void wrapHardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, consume);
        try {
            const auto source =
                std::string("RWStructuredBuffer<uint> output:register(u0);RWStructuredBuffer<uint> "
                            "data:register(u1);") +
                "[numthreads(1,1,1)]void main(){output[0]=data." +
                (consume ? "DecrementCounter();}" : "IncrementCounter();}");
            const auto shader = compileHlsl(source, "cs_5_0").bytecode;
            auto capture = testing::computeCapture(true);
            replaceShader(capture, shader);
            QTemporaryDir directory;
            const auto path = directory.path() + "/wrap.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = !vendor;
            options.debug = true;
            if (vendor)
                options.vendor = vendor;
            options.until = 100;
            options.before = true;
            options.uavCounters[100][12] = consume ? 0 : UINT32_MAX;
            Replay before(frame, options);
            before.run();
            const auto snapshot = snapshotCounters(before, 100);
            QCOMPARE(before.readCounter(12), 0u);
            Inputs state;
            state.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{7, Buffer::known(before.readBuffer(7))},
                                           {10, Buffer::known(before.readBuffer(10))}},
                std::map<Register, BufferView>{{{30, {0}}, {7, 0, 16, 4}}, {{30, {1}}, {10, 0, 16, 4, 12}}},
                256 * 1024 * 1024, 16 * 1024 * 1024, nullptr, false, snapshot.counters);
            Program program(shader);
            Group group(program, state);
            while (!group.finished())
                group.step();
            options.before = false;
            Replay after(frame, options);
            after.run();
            QCOMPARE(after.readCounter(12), consume ? UINT32_MAX : 0u);
            QCOMPARE(state.memory->counters().at(12).value,
                     std::optional<uint32_t>(consume ? UINT32_MAX : 0u));
            const auto gpu = after.readBuffer(7);
            const auto cpu = state.memory->buffers().at(7);
            QCOMPARE(words(gpu, 1)[0], UINT32_MAX);
            QCOMPARE(cpu.bytes, gpu);
            QVERIFY(std::all_of(cpu.valid.begin(), cpu.valid.end(), [](uint8_t v) { return v == 1; }));
            QCOMPARE(after.readBuffer(10), before.readBuffer(10));
            validateDevice(before);
            validateDevice(after);
            const auto adapter = before.adapterDescription();
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"probe", "dxbc-hidden-counter-wrap"},
                                {"consume", consume},
                                {"vendor", vendor},
                                {"shader_sha256", hash(shader)},
                                {"adapter", QString::fromWCharArray(adapter.Description).toStdString()},
                                {"adapter_vendor", adapter.VendorId},
                                {"counter_after", after.readCounter(12)},
                                {"cpu_output_sha256", hash(cpu.bytes)},
                                {"gpu_output_sha256", hash(gpu)},
                                {"comparison", "exact returned raw bits, counter, and storage"},
                                {"passed", true}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void unknownReplaySnapshot() {
        auto capture = testing::computeCapture(true);
        const auto shader =
            compileHlsl(
                "RWStructuredBuffer<uint> data:register(u1);[numthreads(1,1,1)]void main(){data[0]=55;}",
                "cs_5_0")
                .bytecode;
        replaceShader(capture, shader);
        std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 90; });
        QTemporaryDir directory;
        const auto path = directory.path() + "/unknown.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        Replay replay(frame, options);
        replay.run();
        const auto snapshot = snapshotCounters(replay, 100);
        QVERIFY(!snapshot.counters.at(12).value);
        QVERIFY(!snapshot.unavailable.at(12).empty());
        QCOMPARE(snapshot.bindings.at({30, {1}}), uint64_t(12));
    }
    void graphicsSnapshot() {
        for (bool ordered : {false, true}) {
            auto capture = testing::graphicsCounterCapture(ordered);
            QTemporaryDir directory;
            const auto path = directory.path() + "/graphics.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = true;
            options.until = 200;
            options.before = true;
            options.initialUavCounters[12] = 2;
            options.uavCounters[200][12] = 3;
            Replay replay(frame, options);
            replay.run();
            const auto initial = replay.readCounter(12);
            const auto snapshot = snapshotCounters(replay, 200);
            QCOMPARE(snapshot.counters.at(12).kind, ordered ? CounterKind::Ordered : CounterKind::Append);
            QCOMPARE(snapshot.counters.at(12).value, std::optional<uint32_t>(3));
            QVERIFY(!snapshot.bindings.empty());
            QCOMPARE(replay.readCounter(12), initial);
        }
    }
};
QTEST_GUILESS_MAIN(DxbcCounterTests)
#include "DxbcCounterTests.moc"
