#include "SyntheticCapture.h"
#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "application/DxbcTypedBuffer.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
std::vector<uint8_t> storage(uint32_t size, uint32_t seed) {
    std::vector<uint8_t> result(size);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = uint8_t(seed + i * 37);
    return result;
}
void buffer(testing::Capture &capture, Id id, Id data, Bytes initial, UINT bind, UINT stride = 0) {
    std::erase_if(capture.entries, [&](auto &e) { return e.id == id || e.id == data; });
    std::vector<uint8_t> desc(16);
    testing::append(desc, D3D11_BUFFER_DESC{UINT(initial.size()), D3D11_USAGE_DEFAULT, bind, 0,
                                            stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0u, stride});
    testing::append(desc, data);
    capture.add(id, 5, 0x83, desc);
    auto bytes = testing::word(UINT(initial.size()));
    bytes.insert(bytes.end(), initial.begin(), initial.end());
    capture.add(data, 9, 1, bytes);
}
void shader(testing::Capture &capture, Bytes code) {
    std::erase_if(capture.entries, [](auto &e) { return e.id == 16 || e.id == 111; });
    std::vector<uint8_t> bytes;
    testing::append(bytes, uint64_t(code.size()));
    bytes.insert(bytes.end(), code.begin(), code.end());
    testing::append(bytes, Id(0));
    capture.add(16, 9, 0x81, bytes);
}
void views(testing::Capture &capture, uint32_t format) {
    std::erase_if(capture.entries, [](auto &e) { return e.id == 6 || e.id == 9 || e.id == 12; });
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT(format);
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv.Buffer = {1, 3};
    std::vector<uint8_t> raw(16);
    testing::append(raw, Id(4));
    testing::append(raw, srv);
    capture.add(6, 5, 0x8c, raw);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    // RGB32 is used only by the SRV probe; retain a valid unused UAV.
    uav.Format = format == 6 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT(format);
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer = {2, 3, 0};
    raw.assign(16, 0);
    testing::append(raw, Id(7));
    testing::append(raw, uav);
    capture.add(9, 5, 0x8f, raw);
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.Buffer = {0, 1, 0};
    raw.assign(16, 0);
    testing::append(raw, Id(10));
    testing::append(raw, uav);
    capture.add(12, 5, 0x8f, raw);
}
std::string hash(Bytes bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
ReplayOptions options(uint32_t vendor) {
    ReplayOptions result;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    result.before = true;
    result.until = 100;
    result.debug = true;
    return result;
}
void validation(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Read typed probe validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read typed message size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read typed message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words result{code};
    for (auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words reg(uint32_t kind, uint32_t index, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), index};
}
Program raw(std::vector<Words> rows) {
    rows.insert(rows.begin(), {op(104, {{4}}), op(155, {{1, 1, 1}})});
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50050, 0}, rows})}}));
}
} // namespace
class DxbcTypedBufferTests final : public QObject {
    Q_OBJECT
  private slots:
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<uint32_t>("index");
        QTest::addColumn<bool>("literal");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format : {41u, 42u, 43u, 2u, 11u, 31u, 30u, 50u, 62u, 24u, 26u, 6u})
                for (uint32_t mode : {0u, 1u, 2u}) {
                    if ((format == 6 && mode != 0) ||
                        (mode == 2 && format != 41 && format != 42 && format != 43))
                        continue;
                    for (uint32_t index : {0u, 3u, UINT32_MAX})
                        for (bool literal : {false, true})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(format)
                                                         .arg(mode)
                                                         .arg(index)
                                                         .arg(literal)))
                                << vendor << format << mode << index << literal;
                }
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, mode);
        QFETCH(uint32_t, index);
        QFETCH(bool, literal);
        try {
            const auto info = *typedBufferFormat(format);
            const auto type = info.returnType == 5 ? "float" : info.returnType == 4 ? "uint" : "int";
            const std::string address = literal ? std::to_string(index) + "u" : "p.x";
            const auto expression = std::string(info.returnType == 5   ? "asfloat(v)"
                                                : info.returnType == 4 ? "v"
                                                                       : "asint(v)");
            const std::string declarations =
                mode == 0 ? std::string("Buffer<") + type + "4> src:register(t0);"
                          : std::string("RWBuffer<") + type +
                                (mode == 2 ? "> src:register(u0);" : "4> dst:register(u0);");
            const std::string body = mode == 0   ? "result[0]=asuint(src[" + address + "].wzyx);"
                                     : mode == 1 ? "dst[" + address + "]=" + expression + ";"
                                                 : "result[0]=uint4(asuint(src[" + address + "]),17,23,31);";
            const auto code =
                compileHlsl(
                    "cbuffer Params:register(b0){uint4 p;uint4 v;}" + declarations +
                        "RWStructuredBuffer<uint4> result:register(u1);[numthreads(1,1,1)]void main(){" +
                        body + "}",
                    "cs_5_0")
                    .bytecode;
            auto capture = testing::computeCapture(true);
            shader(capture, code);
            const auto input = storage(info.bytes * 8, 19), output = storage(info.bytes * 8, 71);
            buffer(capture, 4, 5, input, D3D11_BIND_SHADER_RESOURCE);
            buffer(capture, 7, 8, output, D3D11_BIND_UNORDERED_ACCESS);
            buffer(capture, 10, 11, std::vector<uint8_t>(16, 0), D3D11_BIND_UNORDERED_ACCESS, 16);
            std::vector<uint8_t> constants;
            testing::append(constants, std::array<uint32_t, 4>{index, 0, 0, 0});
            testing::append(constants,
                            info.returnType == 5
                                ? std::array<uint32_t, 4>{0xbf800000, 0x3f000000, 0x3fc00000, 0x7fc12345}
                                : std::array<uint32_t, 4>{0x80000001, 0xffffffff, 0x12345678, 17});
            buffer(capture, 2, 3, constants, D3D11_BIND_CONSTANT_BUFFER);
            views(capture, format);
            QTemporaryDir directory;
            const auto path = directory.path() + "/typed.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            auto configuration = options(vendor);
            Replay replay(frame, configuration);
            const auto snapshot = snapshotBufferInputs(replay, 5);
            QCOMPARE(snapshot.bindings.at({7, {0}}).format,
                     mode == 0 ? format : 0u); // Unused SRVs are normalized to null.
            const auto initial = snapshot.inputs.memory->buffers();
            Program program(snapshot.bytecode);
            Group group(program, snapshot.inputs);
            std::vector<GroupStep> trace;
            uint32_t assisted = 0;
            while (!group.finished()) {
                trace.push_back(group.step());
                assisted += trace.back().step.memoryGpuAssisted;
            }
            QCOMPARE(assisted, 1u);
            configuration.before = false;
            Replay reference(frame, configuration);
            reference.run();
            const auto &cpu = snapshot.inputs.memory->buffers().at(mode == 1 ? 7 : 10);
            const auto gpu = reference.readBuffer(mode == 1 ? 7 : 10);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"probe", "dxbc-typed-buffer"},
                                {"vendor", vendor},
                                {"format", format},
                                {"mode", mode},
                                {"index", index},
                                {"literal", literal},
                                {"shader_sha256", hash(code)},
                                {"frame_sha256", snapshot.frameSha256},
                                {"adapter_vendor", snapshot.adapterVendor},
                                {"adapter_device", snapshot.adapterDevice},
                                {"adapter_luid", snapshot.adapterLuid},
                                {"cpu_sha256", hash(cpu.bytes)},
                                {"gpu_sha256", hash(gpu)},
                                {"gpu_assisted_steps", assisted},
                                {"source", InputSnapshot::source}})
                    .dump());
            QCOMPARE(cpu.bytes, gpu);
            QVERIFY(std::all_of(cpu.valid.begin(), cpu.valid.end(), [](uint8_t v) { return v == 1; }));
            auto reversed = snapshot.inputs.memory->buffers();
            for (auto step = trace.rbegin(); step != trace.rend(); ++step)
                for (auto change = step->step.memoryChanges.rbegin();
                     change != step->step.memoryChanges.rend(); ++change) {
                    auto &data = reversed.at(change->resource);
                    std::copy(change->before.begin(), change->before.end(),
                              data.bytes.begin() + change->offset);
                    std::copy(change->beforeValid.begin(), change->beforeValid.end(),
                              data.valid.begin() + change->offset);
                }
            for (const auto &[id, data] : initial) {
                QCOMPARE(reversed.at(id).bytes, data.bytes);
                QCOMPARE(reversed.at(id).valid, data.valid);
            }
            QCOMPARE(replay.readBuffer(7), output);
            QCOMPARE(replay.readBuffer(4), input);
            validation(replay);
            validation(reference);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void validityAndUnchangedWrites() {
        try {
            QTemporaryDir directory;
            const auto path = directory.path() + "/device.gpa_frame";
            testing::computeCapture(true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            replay.run();
            auto gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), true);
            auto data = Buffer::known(std::vector<uint8_t>(32, 0));
            data.valid[0] = 0;
            Inputs input;
            input.memory =
                std::make_shared<Memory>(std::map<uint64_t, Buffer>{{1, data}},
                                         std::map<Register, BufferView>{{{7, {0}}, {1, 0, 32, 0, {}, 3}},
                                                                        {{30, {0}}, {1, 0, 32, 0, {}, 3}}},
                                         256 * 1024 * 1024, 16 * 1024 * 1024, gpu);
            auto load = raw({op(88 | 0x800, {reg(7, 0), {0x4444}}),
                             op(45, {reg(0, 1), {0x4001, 0}, {0x107e46, 0}}), op(62)});
            Lane lane(load, input);
            lane.step();
            QCOMPARE(lane.value({0, {1}}).valid, uint8_t(14));
            input.constantBuffers[0] = {{{0, 0, 0, 0}, 5}};
            auto store = raw({op(89, {{0x208e46, 0, 1}}), op(156 | 0x800, {reg(30, 0), {0x4444}}),
                              op(164, {reg(30, 0), {0x4001, 0}, {0x208e46, 0, 0}}), op(62)});
            Lane writer(store, input);
            const auto change = writer.step();
            QCOMPARE(change.memoryChanges.size(), size_t(1));
            QCOMPARE(change.memoryChanges[0].before, change.memoryChanges[0].after);
            QCOMPARE(input.memory->buffers().at(1).valid,
                     (std::vector<uint8_t>{1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                           1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));
            QVERIFY(!typedBufferFormat(39));
            auto masked = raw({op(88 | 0x800, {reg(7, 0), {0x4444}}), op(54, {reg(0, 1), {0x4001, 17}}),
                               op(45, {reg(0, 1, 9), {0x4001, 0}, {0x1071b6, 0}}), op(62)});
            // Restore the original validity before checking a masked, swizzled load.
            input.memory =
                std::make_shared<Memory>(std::map<uint64_t, Buffer>{{1, data}},
                                         std::map<Register, BufferView>{{{7, {0}}, {1, 0, 32, 0, {}, 3}}},
                                         256 * 1024 * 1024, 16 * 1024 * 1024, gpu);
            Lane selected(masked, input);
            selected.step();
            selected.step();
            QCOMPARE(selected.value({0, {1}}).valid, uint8_t(7));
            QCOMPARE(selected.value({0, {1}}).bits, (std::array<uint32_t, 4>{0, 17, 17, 0}));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void packedValidity() {
        try {
            QTemporaryDir directory;
            const auto path = directory.path() + "/device.gpa_frame";
            testing::computeCapture(true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            replay.run();
            auto gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), true);
            Inputs input;
            input.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(std::vector<uint8_t>(8))}},
                std::map<Register, BufferView>{{{30, {0}}, {1, 0, 8, 0, {}, 24}}}, 256 * 1024 * 1024,
                16 * 1024 * 1024, gpu);
            input.constantBuffers[0] = {{{0, 0, 0, 0}, 5}};
            auto store = raw({op(89, {{0x208e46, 0, 1}}), op(156 | 0x800, {reg(30, 0), {0x5555}}),
                              op(164, {reg(30, 0), {0x4001, 0}, {0x208e46, 0, 0}}), op(62)});
            Lane writer(store, input);
            const auto step = writer.step();
            QCOMPARE(step.memoryChanges.size(), size_t(1));
            QCOMPARE(step.memoryChanges[0].afterValid, (std::vector<uint8_t>{1, 0, 0, 0}));
            QCOMPARE(input.memory->buffers().at(1).valid, (std::vector<uint8_t>{1, 0, 0, 0, 1, 1, 1, 1}));
            QCOMPARE(input.memory->buffers().at(1).bytes, std::vector<uint8_t>(8));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void rejectionAndCancellation() {
        try {
            for (bool typedFirst : {false, true}) {
                const auto typed = op(156 | 0x800, {reg(30, 0), {0x4444}});
                const auto untyped = op(157, {reg(30, 0)});
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, raw({typedFirst ? typed : untyped,
                                                                  typedFirst ? untyped : typed, op(62)}));
            }
            QTemporaryDir directory;
            const auto path = directory.path() + "/device.gpa_frame";
            testing::computeCapture(true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            replay.run();
            bool cancelled = false;
            auto gpu =
                std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return cancelled; });
            Inputs input;
            input.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(std::vector<uint8_t>(16))}},
                std::map<Register, BufferView>{{{30, {0}}, {1, 0, 16, 0, {}, 42}}}, 256 * 1024 * 1024,
                16 * 1024 * 1024, gpu);
            auto bad = raw({op(156 | 0x800, {reg(30, 0), {0x4444}}),
                            op(164, {reg(30, 0, 1), {0x4001, 0}, {0x4001, 17}}), op(62)});
            Lane invalid(bad, input);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalid.step());
            QCOMPARE(gpu->operations(), uint64_t(0));
            auto p = raw({op(156 | 0x800, {reg(30, 0), {0x4444}}),
                          op(164, {reg(30, 0), {0x4001, 0}, {0x4001, 17}}), op(62)});
            Lane lane(p, input);
            cancelled = true;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, lane.step());
            QCOMPARE(input.memory->buffers().at(1).bytes, std::vector<uint8_t>(16));
            QVERIFY(lane.registers().empty());
            cancelled = false;
            const auto changed = lane.step();
            QVERIFY(changed.memoryGpuAssisted);
            QCOMPARE(gpu->operations(), uint64_t(1));
            Inputs null;
            null.memory = std::make_shared<Memory>(std::map<uint64_t, Buffer>{},
                                                   std::map<Register, BufferView>{{{30, {0}}, {}}});
            Lane unbound(p, null);
            QVERIFY(unbound.step().memoryChanges.empty());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcTypedBufferTests)
#include "DxbcTypedBufferTests.moc"
